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

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
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

// ─── 6. CRITICAL: helpers do NOT depend on prev_phase_sys() ─────────────────

TEST_CASE(  // NOLINT
    "physical_eini_from_cache: ignores prev_phase_sys (no cross-phase access)")
{
  // Regression test for the latent daily_cycle bug + the user-flagged
  // "wrong eini" propagation footgun.  The helper must read the current
  // sys's `eini_col` regardless of whether prev_phase_sys is set or
  // what its LP says.  Even if we deliberately point prev_phase_sys
  // at a different SystemLP (e.g. a different scene's phase 0 with a
  // different solved efin), the result must NOT change.
  auto planning_a = make_two_phase_reservoir_planning(/*eini=*/100.0);
  PlanningLP planning_a_lp(std::move(planning_a));

  auto& sys1 = planning_a_lp.system(first_scene_index(), PhaseIndex {1});
  REQUIRE(sys1.linear_interface().resolve().has_value());
  REQUIRE(sys1.linear_interface().is_optimal());

  const auto& rsv1 = sys1.elements<ReservoirLP>().front();
  const auto& scenario1 = sys1.scene().scenarios().front();
  const auto& stage1 = sys1.phase().stages().front();

  const auto cache = make_reservoir_ref_cache(rsv1, scenario1, stage1);

  // Baseline read with prev_phase_sys explicitly nullptr.
  sys1.set_prev_phase_sys(nullptr);
  const auto with_null_prev =
      physical_eini_from_cache(sys1, scenario1, stage1, cache);

  // Re-read with prev_phase_sys pointed back at sys0 (still same scene).
  // Result MUST match — the helper does not consult prev_phase_sys.
  auto& sys0 = planning_a_lp.system(first_scene_index(), first_phase_index());
  REQUIRE(sys0.linear_interface().resolve().has_value());
  sys1.set_prev_phase_sys(&sys0);
  const auto with_prev =
      physical_eini_from_cache(sys1, scenario1, stage1, cache);

  CHECK(with_null_prev == doctest::Approx(with_prev));

  // Reset for cleanup
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

  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "bus1",
  }};
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
  const Array<Demand> demand_array = {{
      .uid = Uid {1},
      .name = "load1",
      .bus = Uid {1},
      .capacity = 100.0,
  }};
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
  const Array<Waterway> waterway_array = {{
      .uid = Uid {1},
      .name = "ww1",
      .junction_a = Uid {1},
      .junction_b = Uid {2},
      .fmin = 0.0,
      .fmax = 100.0,
  }};
  // KEY DIFFERENCE: daily_cycle = true.  No state-link constraint
  // across phases; in-phase efin == eini close constraint instead.
  const Array<Reservoir> reservoir_array = {{
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
  }};
  const Array<Flow> flow_array = {{
      .uid = Uid {1},
      .name = "inflow",
      .direction = 1,
      .junction = Uid {1},
      .discharge = 10.0,
  }};
  const Array<Turbine> turbine_array = {{
      .uid = Uid {1},
      .name = "tur1",
      .waterway = Uid {1},
      .generator = Uid {1},
      .production_factor = 1.0,
  }};

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array = {{{.uid = Uid {1}}}},
      .phase_array = std::move(phase_array),
  };

  PlanningOptions options;
  options.demand_fail_cost = 1000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};

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

  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "bus1",
  }};
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
  const Array<Demand> demand_array = {{
      .uid = Uid {1},
      .name = "load1",
      .bus = Uid {1},
      .capacity = 100.0,
  }};
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
  options.demand_fail_cost = 1000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};

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
