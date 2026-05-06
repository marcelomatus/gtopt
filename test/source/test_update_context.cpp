// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_update_context.cpp
 * @brief     Tests for the ReservoirRefCache value-channel that decouples
 *            HasUpdateLP elements from `sys.element<ReservoirLP>` lookups.
 *
 * The contract under test:
 *
 *   physical_eini_from_cache(sys, scenario, stage, cache)
 *     ==  rsv.physical_eini(sys, scenario, stage, default, rsv_sid)
 *
 *   physical_efin_from_cache(sys, cache)
 *     ==  rsv.physical_efin(li, scenario, stage, default)
 *
 * The cache must reproduce these values byte-for-byte regardless of which
 * cross-phase resolution path is active:
 *   * fallback path  — `prev_sys->element<ReservoirLP>(rsv_sid)` lookup.
 *   * StateVariable path — bound predecessor locator triggers a
 *     `sim.state_variable(key)` lookup that reads `col_sol_physical()`
 *     from the simulation's state-variable registry.
 *
 * Background: a previous regression that propagated a wrong efin value to
 * the next phase took roughly a week to debug.  These tests pin the
 * value-channel against the existing `physical_eini` / `physical_efin`
 * oracle so any future divergence is caught at unit-test time.
 */

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/state_variable.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/update_context.hpp>

#include "fixture_helpers.hpp"

using namespace gtopt;  // NOLINT(google-build-using-namespace)
using gtopt::test_fixtures::make_single_stage_phases;
using gtopt::test_fixtures::make_uniform_blocks;
using gtopt::test_fixtures::make_uniform_stages;

namespace
{

/// 2-phase, 1-scenario, 1-stage-per-phase, 1-reservoir hydro fixture.
/// Same shape as `make_3phase_hydro_planning` in `sddp_helpers.hpp` but
/// trimmed to what these tests need.  The reservoir uses
/// `use_state_variable=true` so `efin` is registered as a StateVariable
/// in every phase — this is the fixture invariant the tests rely on.
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
  options.demand_fail_cost = 1000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};

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
  const ReservoirLPSId rsv_sid {Uid {1}};
  const auto& scenario = sys0.scene().scenarios().front();
  const auto& stage = sys0.phase().stages().front();

  const auto cache = make_reservoir_ref_cache(rsv, rsv_sid, scenario, stage);

  CHECK(cache.rsv_uid == Uid {1});
  CHECK(cache.default_volume == doctest::Approx(250.0));
  CHECK(cache.eini_col == rsv.eini_col_at(scenario, stage));
  CHECK(cache.efin_col == rsv.efin_col_at(scenario, stage));
  CHECK(cache.energy_scale == doctest::Approx(rsv.energy_scale()));
  CHECK_FALSE(cache.has_prev_phase);
}

// ─── 2. physical_eini_from_cache: first-phase first-stage default fallback ──

TEST_CASE(  // NOLINT
    "physical_eini_from_cache: first stage of first phase returns default")
{
  auto planning = make_two_phase_reservoir_planning(/*eini=*/333.0);
  PlanningLP planning_lp(std::move(planning));

  auto& sys0 = planning_lp.system(first_scene_index(), first_phase_index());
  const auto& rsv = sys0.elements<ReservoirLP>().front();
  const ReservoirLPSId rsv_sid {Uid {1}};
  const auto& scenario = sys0.scene().scenarios().front();
  const auto& stage = sys0.phase().stages().front();

  const auto cache = make_reservoir_ref_cache(rsv, rsv_sid, scenario, stage);

  // Phase 0, stage 0: physical_eini hard-codes the default fallback;
  // the cache helper must agree.
  const auto via_cache = physical_eini_from_cache(sys0, scenario, stage, cache);
  CHECK(via_cache == doctest::Approx(333.0));

  const auto via_rsv = rsv.physical_eini(sys0, scenario, stage, 333.0, rsv_sid);
  CHECK(via_cache == doctest::Approx(via_rsv));
}

// ─── 3. physical_efin_from_cache: matches rsv.physical_efin after solve ─────

TEST_CASE(  // NOLINT
    "physical_efin_from_cache: equals rsv.physical_efin after solve")
{
  auto planning = make_two_phase_reservoir_planning(/*eini=*/250.0);
  PlanningLP planning_lp(std::move(planning));

  auto& sys0 = planning_lp.system(first_scene_index(), first_phase_index());

  // Solve phase 0's LP so `is_optimal()` flips and `get_col_sol()` is
  // populated.  The cache helper and `rsv.physical_efin(li, ...)` must
  // both consume the same physical-space, bound-clamped view.
  auto& li0 = sys0.linear_interface();
  const auto result = li0.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);
  REQUIRE(li0.is_optimal());

  const auto& rsv = sys0.elements<ReservoirLP>().front();
  const ReservoirLPSId rsv_sid {Uid {1}};
  const auto& scenario = sys0.scene().scenarios().front();
  const auto& stage = sys0.phase().stages().front();

  const auto cache = make_reservoir_ref_cache(rsv, rsv_sid, scenario, stage);

  const auto via_cache = physical_efin_from_cache(sys0, cache);
  const auto via_rsv = rsv.physical_efin(li0, scenario, stage, 250.0);

  CHECK(via_cache == doctest::Approx(via_rsv));

  // Sanity: hydro is cheap so it dispatches.  efin must drop strictly
  // below the initial 250 dam³ — pinning a known direction protects
  // against a false-positive cache==rsv match where both could read
  // an uninitialized 0 from a stalled solver.
  CHECK(via_cache < doctest::Approx(250.0));
  CHECK(via_cache >= doctest::Approx(0.0));
}

// ─── 4. physical_eini_from_cache: cross-phase fallback path ─────────────────

TEST_CASE(  // NOLINT
    "physical_eini_from_cache: cross-phase fallback equals rsv.physical_efin "
    "of predecessor")
{
  // Phase 1's update_lp reads the predecessor's solved efin via
  // `physical_eini`'s cross-phase branch.  When the cache is unbound
  // (no `bind_prev_phase_state_var` call), the helper falls back to
  // `prev_sys->element<ReservoirLP>(rsv_sid).physical_efin(prev_li, ...)`.
  // The fallback value must equal `prev_rsv.physical_efin` directly.
  auto planning = make_two_phase_reservoir_planning(/*eini=*/250.0);
  PlanningLP planning_lp(std::move(planning));

  auto& sys0 = planning_lp.system(first_scene_index(), first_phase_index());
  auto& sys1 = planning_lp.system(first_scene_index(), PhaseIndex {1});

  // Solve phase 0 so the predecessor has an optimal value.
  auto& li0 = sys0.linear_interface();
  REQUIRE(li0.resolve().has_value());
  REQUIRE(li0.is_optimal());

  // Wire the cross-phase pointer the way the SDDP iteration would.
  sys1.set_prev_phase_sys(&sys0);

  const auto& rsv1 = sys1.elements<ReservoirLP>().front();
  const ReservoirLPSId rsv_sid {Uid {1}};
  const auto& scenario1 = sys1.scene().scenarios().front();
  const auto& stage1 = sys1.phase().stages().front();
  ReservoirRefCache cache =
      make_reservoir_ref_cache(rsv1, rsv_sid, scenario1, stage1);
  // Intentionally leave the predecessor locator unbound — exercise the
  // element-lookup fallback branch.
  REQUIRE_FALSE(cache.has_prev_phase);

  const auto via_cache_fallback =
      physical_eini_from_cache(sys1, scenario1, stage1, cache);

  // Oracle: the same value `rsv.physical_eini(sys1, ...)` would compute.
  const auto via_rsv =
      rsv1.physical_eini(sys1, scenario1, stage1, 250.0, rsv_sid);

  CHECK(via_cache_fallback == doctest::Approx(via_rsv));

  // Direct sanity on the predecessor — must equal phase 0's solved efin.
  const auto& rsv0 = sys0.elements<ReservoirLP>().front();
  const auto prev_efin =
      rsv0.physical_efin(li0, scenario1, sys0.phase().stages().back(), 250.0);
  CHECK(via_cache_fallback == doctest::Approx(prev_efin));
}

// ─── 5. CRITICAL: StateVariable channel correctness ─────────────────────────

TEST_CASE(  // NOLINT
    "physical_eini_from_cache: bound StateVariable equals fallback path")
{
  // The user's regression scenario:
  //
  //   * The cross-phase cache is bound to a StateVariable.
  //   * The forward pass writes a value via `set_col_sol`.
  //   * `physical_eini_from_cache` reads `col_sol_physical()`.
  //
  // The bound path MUST produce the exact same numeric result as the
  // fallback path.  A drift here is the failure mode that took a week
  // to debug — pin both paths against each other so any future skew
  // (wrong reservoir, wrong stage, wrong scenario, stale value) fires
  // an immediate test failure.
  auto planning = make_two_phase_reservoir_planning(/*eini=*/250.0);
  PlanningLP planning_lp(std::move(planning));

  auto& sim = planning_lp.simulation();
  auto& sys0 = planning_lp.system(first_scene_index(), first_phase_index());
  auto& sys1 = planning_lp.system(first_scene_index(), PhaseIndex {1});

  // Solve phase 0 so the predecessor has an optimal value.
  auto& li0 = sys0.linear_interface();
  REQUIRE(li0.resolve().has_value());
  REQUIRE(li0.is_optimal());

  // Replicate `capture_state_variable_values`'s post-solve write so the
  // StateVariable.col_sol_physical() returns the live efin value.  In
  // production this fires automatically inside the SDDP forward pass;
  // calling it manually here keeps the test free of SDDP machinery.
  const auto& rsv0 = sys0.elements<ReservoirLP>().front();
  const auto& scenario = sys0.scene().scenarios().front();
  const auto& stage0 = sys0.phase().stages().front();
  const auto efin_col = rsv0.efin_col_at(scenario, stage0);
  const auto efin_phys = li0.get_col_sol()[efin_col];

  const auto sv_key0 =
      StateVariable::key(scenario,
                         stage0,
                         ReservoirLP::Element::class_name,
                         Uid {1},
                         StorageLP<ObjectLP<Reservoir>>::EfinName);
  auto sv0 = sim.state_variable(sv_key0);
  REQUIRE(sv0.has_value());
  // capture_state_variable_values does `phys / var_scale`; reproduce
  // that here so col_sol_physical() returns `phys` again.
  const auto vs = sv0->get().var_scale();
  sv0->get().set_col_sol((vs != 0.0) ? efin_phys / vs : efin_phys);

  // Wire the cross-phase pointer the way the SDDP iteration would.
  sys1.set_prev_phase_sys(&sys0);

  const auto& rsv1 = sys1.elements<ReservoirLP>().front();
  const ReservoirLPSId rsv_sid {Uid {1}};
  const auto& scenario1 = sys1.scene().scenarios().front();
  const auto& stage1 = sys1.phase().stages().front();

  // Two caches: one unbound (fallback path), one bound (StateVariable).
  ReservoirRefCache cache_fallback =
      make_reservoir_ref_cache(rsv1, rsv_sid, scenario1, stage1);
  ReservoirRefCache cache_bound = cache_fallback;
  bind_prev_phase_state_var(cache_bound, sim, scenario1, stage0);
  REQUIRE(cache_bound.has_prev_phase);
  CHECK(cache_bound.prev_phase_index == stage0.phase_index());
  CHECK(cache_bound.prev_phase_last_stage_uid == stage0.uid());

  const auto via_fallback =
      physical_eini_from_cache(sys1, scenario1, stage1, cache_fallback);
  const auto via_bound =
      physical_eini_from_cache(sys1, scenario1, stage1, cache_bound);

  // Both paths must agree on the value, and both must equal the
  // physical efin solved at phase 0.
  CHECK(via_fallback == doctest::Approx(efin_phys));
  CHECK(via_bound == doctest::Approx(efin_phys));
  CHECK(via_bound == doctest::Approx(via_fallback));
}

// ─── 6. CRITICAL: bound but predecessor NOT yet solved ──────────────────────

TEST_CASE(  // NOLINT
    "physical_eini_from_cache: bound StateVariable with non-optimal "
    "predecessor falls back to default")
{
  // Before phase 0's first solve, `prev_li.is_optimal()` is false even
  // though the StateVariable's `col_sol` defaults to 0.0.  The helper
  // must NOT return that 0 — it must fall through to the
  // warm/default branch.  This is the test that catches a "always read
  // col_sol blindly" bug, which otherwise produces "previous phase
  // ran with reservoir at 0 dam³" failures on iter 0.
  auto planning = make_two_phase_reservoir_planning(/*eini=*/175.0);
  PlanningLP planning_lp(std::move(planning));

  auto& sim = planning_lp.simulation();
  auto& sys0 = planning_lp.system(first_scene_index(), first_phase_index());
  auto& sys1 = planning_lp.system(first_scene_index(), PhaseIndex {1});

  // Deliberately do NOT solve phase 0.
  REQUIRE_FALSE(sys0.linear_interface().is_optimal());

  sys1.set_prev_phase_sys(&sys0);

  const auto& rsv1 = sys1.elements<ReservoirLP>().front();
  const ReservoirLPSId rsv_sid {Uid {1}};
  const auto& scenario1 = sys1.scene().scenarios().front();
  const auto& stage1 = sys1.phase().stages().front();
  const auto& stage0 = sys0.phase().stages().front();

  ReservoirRefCache cache =
      make_reservoir_ref_cache(rsv1, rsv_sid, scenario1, stage1);
  bind_prev_phase_state_var(cache, sim, scenario1, stage0);
  REQUIRE(cache.has_prev_phase);

  // The StateVariable for phase 0's reservoir efin exists and its
  // col_sol is 0 (no capture has fired).  Looking it up by key
  // confirms the publisher exists; the helper must still NOT read
  // that 0 because `prev_li.is_optimal()` is false.
  static constexpr auto reservoir_class_name = ReservoirLP::Element::class_name;
  auto sv_phase0 = sim.state_variable(
      StateVariable::key(reservoir_class_name,
                         cache.rsv_uid,
                         StorageLP<ObjectLP<Reservoir>>::EfinName,
                         cache.prev_phase_index,
                         cache.prev_phase_last_stage_uid,
                         scenario1.scene_index(),
                         scenario1.uid()));
  REQUIRE(sv_phase0.has_value());
  CHECK(sv_phase0->get().col_sol_physical() == doctest::Approx(0.0));

  // Helper must NOT return 0 — the `prev_li.is_optimal()` gate skips
  // the cross-phase branch and falls through to warm/default.  No
  // warm vector is loaded in this fixture, so we land on default.
  const auto via_cache =
      physical_eini_from_cache(sys1, scenario1, stage1, cache);
  CHECK(via_cache == doctest::Approx(175.0));

  // Oracle: rsv.physical_eini agrees.
  const auto via_rsv =
      rsv1.physical_eini(sys1, scenario1, stage1, 175.0, rsv_sid);
  CHECK(via_cache == doctest::Approx(via_rsv));
}
