// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_update_context.cpp
 * @brief     Tests for the ReservoirRefCache value-channel that decouples
 *            HasUpdateLP elements from `sys.element<ReservoirLP>` lookups.
 *
 * The contract under test:
 *
 *   physical_eini_from_cache(sys, scenario, stage, cache)
 *     ==  sys.linear_interface().get_col_sol()[cache.eini_col]
 *           (when sys.linear_interface().is_optimal())
 *
 *   physical_efin_from_cache(sys, cache)
 *     ==  sys.linear_interface().get_col_sol()[cache.efin_col]
 *           (when sys.linear_interface().is_optimal())
 *
 * Both helpers fall through to a warm-vector or default value when
 * the current sys's LP has not been solved yet.  Neither traverses
 * `prev_phase_sys`, looks up a `StateVariable` by key, or accesses
 * `sys.element<ReservoirLP>(...)`.  The LP's structural constraints
 * make this sufficient:
 *   * Non-daily_cycle storage — `eini_col == prev_phase_efin_col` via
 *     the state-link constraint, so reading current `eini_col` yields
 *     the predecessor's solved efin.
 *   * Daily-cycle storage — `efin_col == eini_col` via the in-phase
 *     close constraint, so reading current `eini_col` yields the
 *     current phase's optimised volume.
 *
 * Background: a previous regression that propagated a wrong eini value
 * to the next phase took roughly a week to debug.  These tests pin
 * the cache helpers against the LP's solved column values so any
 * future divergence is caught at unit-test time.
 */

#include <array>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/update_context.hpp>

#include "fixture_helpers.hpp"

using namespace gtopt;
// NOLINTBEGIN(bugprone-argument-comment)

// NOLINTBEGIN(bugprone-unchecked-optional-access)
using gtopt::test_fixtures::make_single_stage_phases;
using gtopt::test_fixtures::make_uniform_blocks;
using gtopt::test_fixtures::make_uniform_stages;

namespace
{

/// 2-phase, 1-scenario, 1-stage-per-phase, 1-reservoir hydro fixture.
/// Same shape as `make_3phase_hydro_planning` in `sddp_helpers.hpp` but
/// trimmed to what these tests need.  The reservoir uses
/// `use_state_variable=true` so `efin` is registered as a StateVariable
/// in every phase.  Daily_cycle is left at its default (false).
inline auto make_two_phase_reservoir_planning(double eini_value = 100.0)
    -> Planning
{
  Array<Block> block_array = make_uniform_blocks(48, 1.0);
  Array<Stage> stage_array = make_uniform_stages(2, 24);
  Array<Phase> phase_array = make_single_stage_phases(2);

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "bus1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 50.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 500.0,
          .emin = 0.0,
          .emax = 500.0,
          .eini = eini_value,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 10.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.0,
      },
  };

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
              },
          },
      .phase_array = std::move(phase_array),
  };

  PlanningOptions options;
  options.model_options.demand_fail_cost = 1000.0;
  options.model_options.use_single_bus = OptBool {true};
  options.model_options.scale_objective = OptReal {1.0};

  System system = {
      .name = "two_phase_reservoir",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = std::move(system),
  };
}

}  // namespace

// ─── 1. ReservoirRefCache field population ──────────────────────────────────

TEST_CASE(  // NOLINT
    "make_reservoir_ref_cache: populates eini/efin/default/scale from rsv")
{
  auto planning = make_two_phase_reservoir_planning(/*eini=*/250.0);
  PlanningLP planning_lp(std::move(planning));

  auto& sys0 = planning_lp.system(first_scene_index(), first_phase_index());
  const auto& rsv = sys0.elements<ReservoirLP>().front();
  const auto& scenario = sys0.scene().scenarios().front();
  const auto& stage = sys0.phase().stages().front();

  const auto cache = make_reservoir_ref_cache(rsv, scenario, stage);

  CHECK(cache.rsv_uid == Uid {1});
  CHECK(cache.default_volume == doctest::Approx(250.0));
  CHECK(cache.eini_col == rsv.eini_col_at(scenario, stage));
  CHECK(cache.efin_col == rsv.efin_col_at(scenario, stage));
  CHECK(cache.energy_scale == doctest::Approx(rsv.energy_scale()));
}

// ─── 2. physical_eini_from_cache: first-phase first-stage default fallback ──

TEST_CASE(  // NOLINT
    "physical_eini_from_cache: first stage of first phase returns default")
{
  auto planning = make_two_phase_reservoir_planning(/*eini=*/333.0);
  PlanningLP planning_lp(std::move(planning));

  auto& sys0 = planning_lp.system(first_scene_index(), first_phase_index());
  const auto& rsv = sys0.elements<ReservoirLP>().front();
  const auto& scenario = sys0.scene().scenarios().front();
  const auto& stage = sys0.phase().stages().front();

  const auto cache = make_reservoir_ref_cache(rsv, scenario, stage);

  // Phase 0, stage 0: helper hard-codes the default fallback.
  const auto via_cache = physical_eini_from_cache(sys0, scenario, stage, cache);
  CHECK(via_cache == doctest::Approx(333.0));
}

// ─── 3. physical_efin_from_cache: matches li.get_col_sol()[efin_col] ────────

TEST_CASE(  // NOLINT
    "physical_efin_from_cache: equals li.get_col_sol()[efin_col] after solve")
{
  auto planning = make_two_phase_reservoir_planning(/*eini=*/250.0);
  PlanningLP planning_lp(std::move(planning));

  auto& sys0 = planning_lp.system(first_scene_index(), first_phase_index());
  auto& li0 = sys0.linear_interface();
  const auto result = li0.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);
  REQUIRE(li0.is_optimal());

  const auto& rsv = sys0.elements<ReservoirLP>().front();
  const auto& scenario = sys0.scene().scenarios().front();
  const auto& stage = sys0.phase().stages().front();

  const auto cache = make_reservoir_ref_cache(rsv, scenario, stage);

  const auto via_cache = physical_efin_from_cache(sys0, cache);
  const auto direct = li0.get_col_sol()[cache.efin_col];

  CHECK(via_cache == doctest::Approx(direct));
}

// ─── 4. physical_eini_from_cache: reads eini_col from current sys ───────────

TEST_CASE(  // NOLINT
    "physical_eini_from_cache: reads li.get_col_sol()[eini_col] from current "
    "sys at phase>0")
{
  // After phase 0 solves, phase 1 hasn't been solved yet but its LP
  // has structural state from construction (eini bounds, etc.).  At
  // phase 1's update_lp time, the helper reads phase 1's own
  // `eini_col` from its current LinearInterface — NOT phase 0's
  // efin via prev_sys.  This is the contract: read eini directly.
  auto planning = make_two_phase_reservoir_planning(/*eini=*/250.0);
  PlanningLP planning_lp(std::move(planning));

  auto& sys1 = planning_lp.system(first_scene_index(), PhaseIndex {1});
  auto& li1 = sys1.linear_interface();
  const auto result = li1.resolve();
  REQUIRE(result.has_value());
  REQUIRE(li1.is_optimal());

  const auto& rsv1 = sys1.elements<ReservoirLP>().front();
  const auto& scenario1 = sys1.scene().scenarios().front();
  const auto& stage1 = sys1.phase().stages().front();

  const auto cache = make_reservoir_ref_cache(rsv1, scenario1, stage1);

  const auto via_cache =
      physical_eini_from_cache(sys1, scenario1, stage1, cache);
  const auto direct = li1.get_col_sol()[cache.eini_col];

  CHECK(via_cache == doctest::Approx(direct));
}

// ─── 5. average_volume_from_cache: (vini + vfin) / 2 ────────────────────────

TEST_CASE(  // NOLINT
    "average_volume_from_cache: averages physical_eini and physical_efin")
{
  auto planning = make_two_phase_reservoir_planning(/*eini=*/200.0);
  PlanningLP planning_lp(std::move(planning));

  auto& sys1 = planning_lp.system(first_scene_index(), PhaseIndex {1});
  auto& li1 = sys1.linear_interface();
  REQUIRE(li1.resolve().has_value());
  REQUIRE(li1.is_optimal());

  const auto& rsv1 = sys1.elements<ReservoirLP>().front();
  const auto& scenario1 = sys1.scene().scenarios().front();
  const auto& stage1 = sys1.phase().stages().front();

  const auto cache = make_reservoir_ref_cache(rsv1, scenario1, stage1);

  const auto avg = average_volume_from_cache(sys1, scenario1, stage1, cache);
  const auto vini = li1.get_col_sol()[cache.eini_col];
  const auto vfin = li1.get_col_sol()[cache.efin_col];
  CHECK(avg == doctest::Approx((vini + vfin) / 2.0));
}

// ─── 6. CRITICAL: helpers DO route through prev_phase_sys via the
//     StateVariable channel — regression guard against the
//     "simplification" that pinned ``physical_eini_from_cache`` to
//     ``rc.default_volume`` (= JSON eini) whenever the current LP
//     wasn't optimal yet.  Observed on juan/IPLP scen 51 phase 38:
//     ELTORO ``eini = 1731 hm³`` (segment 2 ``constant = 15.09 m³/s``)
//     but predecessor's actual trial drained it to 0; without
//     cross-phase routing the seepage row used segment 2's intercept
//     at empty storage → 15.09 m³/s seepage > 12.18 m³/s afluent →
//     cascade infeasibility through p38 → p1.
//
//     The fix re-introduces the StateVariable channel removed by
//     ``aeeb1c98 simplify(update_context): drop StateVariable detour``.
//     The previous test-of-this-name PINNED THE WRONG BEHAVIOUR
//     (asserted ``ignores prev_phase_sys``); the inverted check
//     below is what the codebase needs to keep enforcing so the
//     cross-phase channel cannot be silently removed again.

TEST_CASE(  // NOLINT
    "physical_eini_from_cache: reads predecessor's efin StateVariable "
    "via prev_phase_sys (cross-phase channel)")
{
  auto planning = make_two_phase_reservoir_planning(/*eini=*/100.0);
  PlanningLP planning_lp(std::move(planning));

  // Solve phase 0 first so we have a non-default solved efin to hand
  // to the predecessor's StateVariable.
  auto& sys0 = planning_lp.system(first_scene_index(), first_phase_index());
  REQUIRE(sys0.linear_interface().resolve().has_value());
  REQUIRE(sys0.linear_interface().is_optimal());

  auto& sys1 = planning_lp.system(first_scene_index(), PhaseIndex {1});
  sys1.set_prev_phase_sys(&sys0);

  const auto& rsv1 = sys1.elements<ReservoirLP>().front();
  const auto& rsv0 = sys0.elements<ReservoirLP>().front();
  const auto& scenario1 = sys1.scene().scenarios().front();
  const auto& stage1 = sys1.phase().stages().front();
  const auto& stage0_last = sys0.phase().stages().back();

  const auto cache = make_reservoir_ref_cache(rsv1, scenario1, stage1);

  // Mimic what ``SDDPMethod::capture_state_variable_values`` does
  // after a successful phase solve: write the freshly-solved efin
  // into the predecessor's ``efin`` StateVariable so
  // ``col_sol_physical()`` returns it on read.
  const auto efin0_col = rsv0.efin_col_at(scenario1, stage0_last);
  const auto prev_solved_efin =
      sys0.linear_interface().get_col_sol()[efin0_col];
  // The fallback default_volume in our two-phase fixture is set to
  // ``eini = 100.0``; the LP normally drives efin away from that
  // value, but if it didn't the assertion below would be
  // uninformative.  We require a meaningful difference.
  REQUIRE(prev_solved_efin != doctest::Approx(cache.default_volume));

  static constexpr auto reservoir_class_name = ReservoirLP::Element::class_name;
  auto svar_opt = planning_lp.simulation().state_variable(StateVariable::key(
      scenario1, stage0_last, reservoir_class_name, rsv0.uid(), "efin"));
  REQUIRE(svar_opt.has_value());
  // Set the predecessor's StateVariable col_sol to the raw
  // (LP-space) value — col_sol_physical() multiplies by var_scale
  // internally.
  const auto var_scale = svar_opt->get().var_scale();
  svar_opt->get().set_col_sol(prev_solved_efin / var_scale);

  // physical_eini_from_cache MUST return the predecessor's
  // StateVariable ``col_sol_physical()`` — i.e. the actual phase-0
  // solved efin, NOT the JSON-default ``eini``.
  const auto vini = physical_eini_from_cache(sys1, scenario1, stage1, cache);
  CHECK(vini == doctest::Approx(prev_solved_efin));
  // And critically NOT the default volume (the failure mode from
  // juan/IPLP p38: defaulting to JSON eini).
  CHECK(vini != doctest::Approx(cache.default_volume));

  // Reset for cleanup
  sys1.set_prev_phase_sys(nullptr);
}

TEST_CASE(  // NOLINT
    "physical_eini_from_cache: regression — does NOT fall back to "
    "default_volume when current LP is unsolved but predecessor is")
{
  // The exact regression introduced by ``aeeb1c98``: when ``update_lp``
  // runs at phase p+1 BEFORE the current LP is solved, the cache helper
  // used to fall through to ``rc.default_volume``.  This pins the
  // forward-pass ``update_lp`` behaviour where the predecessor IS
  // solved (so the StateVariable channel can supply the value) but
  // the current phase isn't.
  auto planning = make_two_phase_reservoir_planning(/*eini=*/100.0);
  PlanningLP planning_lp(std::move(planning));

  auto& sys0 = planning_lp.system(first_scene_index(), first_phase_index());
  REQUIRE(sys0.linear_interface().resolve().has_value());
  REQUIRE(sys0.linear_interface().is_optimal());

  auto& sys1 = planning_lp.system(first_scene_index(), PhaseIndex {1});
  sys1.set_prev_phase_sys(&sys0);

  // Deliberately do NOT solve sys1 — emulating the moment in the
  // forward pass when ``update_lp_for_phase(p+1)`` runs *before* the
  // p+1 solve.
  REQUIRE_FALSE(sys1.linear_interface().is_optimal());

  const auto& rsv1 = sys1.elements<ReservoirLP>().front();
  const auto& rsv0 = sys0.elements<ReservoirLP>().front();
  const auto& scenario1 = sys1.scene().scenarios().front();
  const auto& stage1 = sys1.phase().stages().front();
  const auto& stage0_last = sys0.phase().stages().back();
  const auto cache = make_reservoir_ref_cache(rsv1, scenario1, stage1);

  // Set the predecessor's efin StateVariable col_sol manually
  // (mimicking what ``SDDPMethod::capture_state_variable_values``
  // does after a successful forward solve at phase 0).
  const auto efin0_col = rsv0.efin_col_at(scenario1, stage0_last);
  const auto prev_solved_efin =
      sys0.linear_interface().get_col_sol()[efin0_col];
  REQUIRE(prev_solved_efin != doctest::Approx(cache.default_volume));

  static constexpr auto reservoir_class_name = ReservoirLP::Element::class_name;
  auto svar_opt = planning_lp.simulation().state_variable(StateVariable::key(
      scenario1, stage0_last, reservoir_class_name, rsv0.uid(), "efin"));
  REQUIRE(svar_opt.has_value());
  const auto var_scale = svar_opt->get().var_scale();
  svar_opt->get().set_col_sol(prev_solved_efin / var_scale);

  const auto vini = physical_eini_from_cache(sys1, scenario1, stage1, cache);
  // The killer assertion: even though current LP isn't optimal yet,
  // the helper must NOT return ``default_volume`` — it must read
  // the predecessor's StateVariable.
  CHECK(vini != doctest::Approx(cache.default_volume));
  CHECK(vini == doctest::Approx(prev_solved_efin));

  sys1.set_prev_phase_sys(nullptr);
}

TEST_CASE(  // NOLINT
    "physical_eini_from_cache: pinned eini-col bound wins over "
    "default_volume when StateVariable lookup misses (aperture path)")
{
  // Reproduces the juan/gtopt_iplp p51 production bug
  // (2026-05-12 INFEAS-PROBE confirmed via IIS):
  //
  //   * Forward pass drains a reservoir (ELTORO/COLBUN/CIPRESES) to 0.
  //   * ``propagate_trial_values`` pins the next phase's ``eini`` col
  //     to that value (lb == ub == 0 via the 'B' setter).
  //   * Backward / aperture clone calls ``update_lp_for_phase`` BEFORE
  //     the clone is solved — so ``li.is_optimal()`` is false.
  //   * The current scenario is the aperture's scenario, NOT the
  //     scene's central scenario the source StateVariable was
  //     registered under → step 1 lookup misses.
  //   * Pre-fix: helper falls through to ``rc.default_volume`` (the
  //     ANNUAL ``eini`` from the JSON, e.g. 1731 Hm³ for ELTORO) →
  //     ReservoirSeepageLP::update_lp selects the wrong piecewise
  //     segment (mid-volume slope/constant) → seepage row forces ~75
  //     m³/s of outflow over 5 blocks → aperture LP genuinely
  //     infeasible.
  //
  // The pinned bound is the single source of truth for "what state
  // was propagated into this phase".  This test fails on pre-fix
  // (returns default_volume) and passes on post-fix (returns the
  // pinned bound).
  auto planning = make_two_phase_reservoir_planning(/*eini=*/100.0);
  PlanningLP planning_lp(std::move(planning));

  auto& sys0 = planning_lp.system(first_scene_index(), first_phase_index());
  REQUIRE(sys0.linear_interface().resolve().has_value());

  auto& sys1 = planning_lp.system(first_scene_index(), PhaseIndex {1});
  // Deliberately do NOT wire prev_phase_sys, AND do NOT populate the
  // predecessor's efin StateVariable's col_sol — both step-1 paths
  // are dead.  This matches the aperture-path failure mode where the
  // scenario key doesn't match the registered StateVariable.

  const auto& rsv1 = sys1.elements<ReservoirLP>().front();
  const auto& scenario1 = sys1.scene().scenarios().front();
  const auto& stage1 = sys1.phase().stages().front();
  const auto cache = make_reservoir_ref_cache(rsv1, scenario1, stage1);

  // Pin the eini column to a known value different from default_volume.
  // This is exactly what propagate_trial_values does after the forward
  // pass solves the predecessor — set both bounds ('B') to the
  // physical trial value via the bulk physical setter.
  constexpr double pinned_phys = 7.5;  // ≠ default_volume (= eini = 100)
  REQUIRE(pinned_phys != doctest::Approx(cache.default_volume));
  {
    auto& li = sys1.linear_interface();
    const std::array<ColIndex, 1> idx {cache.eini_col};
    const std::array<char, 1> lu {'B'};
    const std::array<double, 1> phys {pinned_phys};
    li.set_col_bounds(idx, lu, phys);
    // sys1 was never solved → is_optimal must be false so we exercise
    // the bound-pin short-circuit specifically (not the col_sol step).
    REQUIRE_FALSE(li.is_optimal());
  }

  const auto vini = physical_eini_from_cache(sys1, scenario1, stage1, cache);

  // The killer assertion: the helper must read the pinned bound (the
  // forward-pass state propagated by ``propagate_trial_values``) and
  // NOT fall through to ``default_volume`` (which would re-select the
  // wrong piecewise seepage segment).
  CHECK(vini == doctest::Approx(pinned_phys));
  CHECK(vini != doctest::Approx(cache.default_volume));
}

TEST_CASE(  // NOLINT
    "physical_eini_from_cache: pinned bound takes precedence over "
    "stale StateVariable col_sol")
{
  // When both the pinned bound AND the predecessor's StateVariable
  // disagree (e.g. the cached SV is stale after a release/reconstruct
  // cycle), the pinned bound on the current LP is authoritative —
  // it reflects the most recent propagate_trial_values call.  Pins
  // the precedence order (step 0 before step 1) so a future refactor
  // that swaps them is caught at unit-test time.
  auto planning = make_two_phase_reservoir_planning(/*eini=*/100.0);
  PlanningLP planning_lp(std::move(planning));

  auto& sys0 = planning_lp.system(first_scene_index(), first_phase_index());
  REQUIRE(sys0.linear_interface().resolve().has_value());

  auto& sys1 = planning_lp.system(first_scene_index(), PhaseIndex {1});
  sys1.set_prev_phase_sys(&sys0);

  const auto& rsv0 = sys0.elements<ReservoirLP>().front();
  const auto& rsv1 = sys1.elements<ReservoirLP>().front();
  const auto& scenario1 = sys1.scene().scenarios().front();
  const auto& stage1 = sys1.phase().stages().front();
  const auto& stage0_last = sys0.phase().stages().back();
  const auto cache = make_reservoir_ref_cache(rsv1, scenario1, stage1);

  // Populate the predecessor's StateVariable with a *stale* value
  // that is neither default_volume nor the pinned bound — so we can
  // tell which path the helper took.
  constexpr double stale_sv_phys = 42.0;
  static constexpr auto reservoir_class_name = ReservoirLP::Element::class_name;
  auto svar_opt = planning_lp.simulation().state_variable(StateVariable::key(
      scenario1, stage0_last, reservoir_class_name, rsv0.uid(), "efin"));
  REQUIRE(svar_opt.has_value());
  svar_opt->get().set_col_sol(stale_sv_phys / svar_opt->get().var_scale());

  // Pin the eini column to a DIFFERENT value (the "live" trial value).
  constexpr double pinned_phys = 7.5;
  REQUIRE(pinned_phys != doctest::Approx(stale_sv_phys));
  REQUIRE(pinned_phys != doctest::Approx(cache.default_volume));
  {
    auto& li = sys1.linear_interface();
    const std::array<ColIndex, 1> idx {cache.eini_col};
    const std::array<char, 1> lu {'B'};
    const std::array<double, 1> phys {pinned_phys};
    li.set_col_bounds(idx, lu, phys);
  }

  const auto vini = physical_eini_from_cache(sys1, scenario1, stage1, cache);

  // Helper must prefer the pinned bound over the stale SV.
  CHECK(vini == doctest::Approx(pinned_phys));
  CHECK(vini != doctest::Approx(stale_sv_phys));

  sys1.set_prev_phase_sys(nullptr);
}

// ─── 7. Daily-cycle reservoir uses the SAME code path ──────────────────────

namespace
{
inline auto make_two_phase_daily_cycle_planning(double eini_value = 100.0)
    -> Planning
{
  Array<Block> block_array = make_uniform_blocks(48, 1.0);
  Array<Stage> stage_array = make_uniform_stages(2, 24);
  Array<Phase> phase_array = make_single_stage_phases(2);

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "bus1",
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 50.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };
  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };
  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };
  // KEY DIFFERENCE: daily_cycle = true.  No state-link constraint
  // across phases; in-phase efin == eini close constraint instead.
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv_dc",
          .junction = Uid {1},
          .capacity = 500.0,
          .emin = 0.0,
          .emax = 500.0,
          .eini = eini_value,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
          .use_state_variable = false,
          .daily_cycle = true,
      },
  };
  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 10.0,
      },
  };
  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.0,
      },
  };

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array = {{{.uid = Uid {1}}}},
      .phase_array = std::move(phase_array),
  };

  PlanningOptions options;
  options.model_options.demand_fail_cost = 1000.0;
  options.model_options.use_single_bus = OptBool {true};
  options.model_options.scale_objective = OptReal {1.0};

  System system = {
      .name = "two_phase_daily_cycle",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };
  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = std::move(system),
  };
}
}  // namespace

TEST_CASE(  // NOLINT
    "physical_eini_from_cache: daily_cycle reservoir uses same code path")
{
  // Lockguard: the cache helpers must NOT special-case daily_cycle.
  // Both daily_cycle and non-daily_cycle reservoirs go through the
  // same `li.get_col_sol()[eini_col]` read.  For daily_cycle, the
  // in-phase `efin == eini` close constraint guarantees that value
  // equals `efin_col` post-solve.
  auto planning = make_two_phase_daily_cycle_planning(/*eini=*/200.0);
  PlanningLP planning_lp(std::move(planning));

  auto& sys1 = planning_lp.system(first_scene_index(), PhaseIndex {1});
  auto& li1 = sys1.linear_interface();
  REQUIRE(li1.resolve().has_value());
  REQUIRE(li1.is_optimal());

  const auto& rsv1 = sys1.elements<ReservoirLP>().front();
  const auto& scenario1 = sys1.scene().scenarios().front();
  const auto& stage1 = sys1.phase().stages().front();

  const auto cache = make_reservoir_ref_cache(rsv1, scenario1, stage1);

  const auto via_cache =
      physical_eini_from_cache(sys1, scenario1, stage1, cache);
  const auto direct_eini = li1.get_col_sol()[cache.eini_col];
  const auto direct_efin = li1.get_col_sol()[cache.efin_col];

  // physical_eini_from_cache reads eini_col directly.
  CHECK(via_cache == doctest::Approx(direct_eini));

  // Daily-cycle close constraint: efin == eini post-solve.  Both
  // columns hold the same physical value.
  CHECK(direct_eini == doctest::Approx(direct_efin));
}

// ─── 8. Multi-reservoir routing: cache for A doesn't read B's value ─────────

namespace
{
inline auto make_two_reservoir_planning() -> Planning
{
  Array<Block> block_array = make_uniform_blocks(24, 1.0);
  Array<Stage> stage_array = make_uniform_stages(1, 24);
  Array<Phase> phase_array = make_single_stage_phases(1);

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "bus1",
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "h1",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 50.0,
      },
      {
          .uid = Uid {2},
          .name = "h2",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 50.0,
      },
      {
          .uid = Uid {3},
          .name = "thermal",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };
  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_a",
      },
      {
          .uid = Uid {2},
          .name = "j_b",
      },
      {
          .uid = Uid {3},
          .name = "j_drain",
          .drain = true,
      },
  };
  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww_a",
          .junction_a = Uid {1},
          .junction_b = Uid {3},
          .fmin = 0.0,
          .fmax = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "ww_b",
          .junction_a = Uid {2},
          .junction_b = Uid {3},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };
  // Two reservoirs with DIFFERENT eini values so the test can detect
  // a cross-routing bug by inspecting which value the cache returns.
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {10},
          .name = "rsv_A",
          .junction = Uid {1},
          .capacity = 500.0,
          .emin = 0.0,
          .emax = 500.0,
          .eini = 111.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
          .use_state_variable = true,
      },
      {
          .uid = Uid {20},
          .name = "rsv_B",
          .junction = Uid {2},
          .capacity = 500.0,
          .emin = 0.0,
          .emax = 500.0,
          .eini = 222.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
          .use_state_variable = true,
      },
  };
  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur_a",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.0,
      },
      {
          .uid = Uid {2},
          .name = "tur_b",
          .waterway = Uid {2},
          .generator = Uid {2},
          .production_factor = 1.0,
      },
  };

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array = {{{.uid = Uid {1}}}},
      .phase_array = std::move(phase_array),
  };

  PlanningOptions options;
  options.model_options.demand_fail_cost = 1000.0;
  options.model_options.use_single_bus = OptBool {true};
  options.model_options.scale_objective = OptReal {1.0};

  System system = {
      .name = "two_reservoir",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };
  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = std::move(system),
  };
}
}  // namespace

TEST_CASE(  // NOLINT
    "make_reservoir_ref_cache: two reservoirs route to distinct eini_col")
{
  // Regression test for a wrong-uid cache wiring bug: cache for
  // reservoir A must hold A's `eini_col`, NOT B's.  At phase 0
  // stage 0 (no solve required), `physical_eini_from_cache`
  // returns the cached `default_volume`, which differs between
  // the two reservoirs (111.0 vs 222.0).
  auto planning = make_two_reservoir_planning();
  PlanningLP planning_lp(std::move(planning));

  auto& sys = planning_lp.system(first_scene_index(), first_phase_index());
  const auto& reservoirs = sys.elements<ReservoirLP>();
  REQUIRE(reservoirs.size() == 2);

  const auto& scenario = sys.scene().scenarios().front();
  const auto& stage = sys.phase().stages().front();

  const ReservoirLP* rsv_a = nullptr;
  const ReservoirLP* rsv_b = nullptr;
  for (const auto& r : reservoirs) {
    if (r.uid() == Uid {10}) {
      rsv_a = &r;
    } else if (r.uid() == Uid {20}) {
      rsv_b = &r;
    }
  }
  REQUIRE(rsv_a != nullptr);
  REQUIRE(rsv_b != nullptr);

  const auto cache_a = make_reservoir_ref_cache(*rsv_a, scenario, stage);
  const auto cache_b = make_reservoir_ref_cache(*rsv_b, scenario, stage);

  // 1. Distinct identity.
  CHECK(cache_a.rsv_uid == Uid {10});
  CHECK(cache_b.rsv_uid == Uid {20});

  // 2. Distinct default_volume — the JSON eini differs (111.0 vs 222.0).
  CHECK(cache_a.default_volume == doctest::Approx(111.0));
  CHECK(cache_b.default_volume == doctest::Approx(222.0));

  // 3. Distinct column indices.  A wrong-uid bug would either share
  //    an index (CHECK fires) or end up off the LP entirely.
  CHECK(cache_a.eini_col != cache_b.eini_col);
  CHECK(cache_a.efin_col != cache_b.efin_col);

  // 4. The actual physical_eini reads at phase-0/stage-0 return each
  //    reservoir's distinct default_volume — proves the per-cache
  //    helper routes to the right element.
  CHECK(physical_eini_from_cache(sys, scenario, stage, cache_a)
        == doctest::Approx(111.0));
  CHECK(physical_eini_from_cache(sys, scenario, stage, cache_b)
        == doctest::Approx(222.0));
}

// NOLINTEND(bugprone-argument-comment)

// NOLINTEND(bugprone-argument-comment)