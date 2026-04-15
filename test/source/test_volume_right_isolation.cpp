// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_volume_right_isolation.cpp
 * @brief     Tier 1 isolation tests for VolumeRightLP
 *
 * Each subcase exercises a single VolumeRightLP behaviour against a
 * minimal hydro fixture (1 bus, 1 generator, 1 demand, 1 reservoir,
 * 1 inflow, 1 turbine, 1 waterway) plus exactly one VolumeRight
 * configured for the property under test.  These are unit-style
 * checks — they verify accessor wiring, sentinel handling, reset_month
 * re-provisioning, and the update_lp cache.
 *
 * Maps to the irrigation test ladder Tier 1 in
 * `~/.claude/projects/-home-marce-git-gtopt/memory/project_irrigation_test_ladder.md`.
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/volume_right_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

using namespace gtopt;  // NOLINT(google-build-using-namespace)

/// Common 1-bus / 1-reservoir hydro skeleton shared across subcases.
/// Each subcase plugs in its own `volume_right_array` to drive the
/// behaviour under test.
struct HydroFixture
{
  Array<Bus> bus_array {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  Array<Generator> generator_array {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 200.0,
      },
  };
  Array<Demand> demand_array {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };
  Array<Junction> junction_array {
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
  Array<Waterway> waterway_array {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 500.0,
      },
  };
  Array<Reservoir> reservoir_array {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 3000.0,
          .emin = 100.0,
          .emax = 3000.0,
          .eini = 1500.0,
      },
  };
  Array<Flow> flow_array {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 100.0,
      },
  };
  Array<Turbine> turbine_array {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 2.0,
      },
  };
};

/// Single-stage / single-block simulation.
[[nodiscard]] Simulation make_single_stage_simulation()
{
  return {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
                  .month = MonthType::april,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };
}

[[nodiscard]] System make_system(const HydroFixture& fx,
                                 const Array<VolumeRight>& vrs,
                                 std::string name)
{
  return {
      .name = std::move(name),
      .bus_array = fx.bus_array,
      .demand_array = fx.demand_array,
      .generator_array = fx.generator_array,
      .junction_array = fx.junction_array,
      .waterway_array = fx.waterway_array,
      .flow_array = fx.flow_array,
      .reservoir_array = fx.reservoir_array,
      .turbine_array = fx.turbine_array,
      .volume_right_array = vrs,
  };
}

}  // namespace

// ── 1.1 Constructor + accessors ───────────────────────────────────────────

TEST_CASE(  // NOLINT
    "VolumeRightLP Tier 1.1 - constructor and accessor wiring")
{
  const HydroFixture fx;
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {1},
          .name = "vright1",
          .reservoir = Uid {1},
          .demand = 120.0,
          .fmax = 75.0,
          .fail_cost = 6000.0,
          .saving_rate = 5.0,
      },
  };

  const auto simulation = make_single_stage_simulation();
  const auto system = make_system(fx, vrs, "Tier1_1_Accessors");

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  const auto& vr_lp = system_lp.elements<VolumeRightLP>().front();

  CHECK(vr_lp.volume_right().uid == Uid {1});
  CHECK(vr_lp.volume_right().name == "vright1");

  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  REQUIRE(!scenarios.empty());
  REQUIRE(!stages.empty());
  const auto& scenario = scenarios[0];
  const auto& stage = stages[0];
  const auto suid = stage.uid();
  REQUIRE(!stage.blocks().empty());
  const auto buid = stage.blocks().front().uid();

  // extraction_cols and saving_cols both populated (saving_rate is set).
  const auto& ext = vr_lp.extraction_cols_at(scenario, stage);
  const auto& sav = vr_lp.saving_cols_at(scenario, stage);
  CHECK(!ext.empty());
  CHECK(!sav.empty());
  CHECK(ext.size() == sav.size());

  // param_* getters echo the configured fields.
  CHECK(vr_lp.param_fmax(suid, buid).value_or(-1.0) == doctest::Approx(75.0));
  CHECK(vr_lp.param_demand(suid).value_or(-1.0) == doctest::Approx(120.0));
  CHECK(vr_lp.param_fail_cost() == doctest::Approx(6000.0));
}

// ── 1.2 Inactive element ──────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "VolumeRightLP Tier 1.2 - inactive element creates no columns")
{
  const HydroFixture fx;
  const Array<VolumeRight> active_vrs = {
      {
          .uid = Uid {1},
          .name = "vactive",
          .reservoir = Uid {1},
          .demand = 50.0,
          .fmax = 25.0,
          .fail_cost = 1000.0,
      },
  };
  const Array<VolumeRight> inactive_vrs = {
      {
          .uid = Uid {1},
          .name = "vinactive",
          .active = false,
          .reservoir = Uid {1},
          .demand = 50.0,
          .fmax = 25.0,
          .fail_cost = 1000.0,
      },
  };

  const auto simulation = make_single_stage_simulation();

  // Build the active version to get a baseline column count.
  std::size_t active_cols = 0;
  {
    const auto system = make_system(fx, active_vrs, "Tier1_2_Active");
    const PlanningOptionsLP options;
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);
    active_cols = system_lp.linear_interface().get_numcols();
  }

  // Inactive version: same fixture but VolumeRight contributes nothing.
  {
    const auto system = make_system(fx, inactive_vrs, "Tier1_2_Inactive");
    const PlanningOptionsLP options;
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    const auto inactive_cols = system_lp.linear_interface().get_numcols();
    CHECK(inactive_cols < active_cols);

    // The accessor must throw because no entry was registered for an
    // inactive stage — confirms add_to_lp early-returned without
    // populating extraction_cols_.
    const auto& vr_lp = system_lp.elements<VolumeRightLP>().front();
    const auto& scenarios = system_lp.scene().scenarios();
    const auto& stages = system_lp.phase().stages();
    REQUIRE(!scenarios.empty());
    REQUIRE(!stages.empty());
    CHECK_THROWS(  // NOLINT(cppcoreguidelines-avoid-do-while)
        (void)vr_lp.extraction_cols_at(scenarios[0], stages[0]));
  }
}

// ── 1.3 No bound_rule + no fmax → DblMax sentinel (regression) ────────────

TEST_CASE(  // NOLINT
    "VolumeRightLP Tier 1.3 - unbounded extraction uses DblMax sentinel")
{
  // Regression for the `Real::max()` sentinel bug: when bound_rule is
  // unset and fmax is unset, the per-block extraction column upper
  // bound must be `LinearProblem::DblMax` (the canonical infinity
  // sentinel), not a finite huge value that would scale into a real
  // numeric coefficient.
  const HydroFixture fx;
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {1},
          .name = "vunbounded",
          .reservoir = Uid {1},
          // demand + fail_cost set so the column has an objective
          // coefficient and is non-degenerate, but no fmax / bound_rule
          // → upper bound must default to DblMax.
          .demand = 50.0,
          .fail_cost = 1000.0,
      },
  };

  const auto simulation = make_single_stage_simulation();
  const auto system = make_system(fx, vrs, "Tier1_3_Sentinel");

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  const auto& vr_lp = system_lp.elements<VolumeRightLP>().front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  REQUIRE(!scenarios.empty());
  REQUIRE(!stages.empty());

  const auto& ext = vr_lp.extraction_cols_at(scenarios[0], stages[0]);
  REQUIRE(!ext.empty());

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // After flatten/normalisation DblMax is clamped to the solver
  // backend's configured infinity (typically 1e20), which is the
  // contract guaranteed by `LinearProblem::clamp_bound`.  Anything
  // >= 1e19 here proves the column upper bound is "effectively
  // infinity" rather than a finite scale-vulnerable value.
  const auto col_upp = lp.get_col_upp();
  for (const auto& [buid, col] : ext) {
    CHECK(col_upp[col] >= 1.0e19);
  }
}

// ── 1.4 bound_rule with floor clamp ───────────────────────────────────────

TEST_CASE(  // NOLINT
    "VolumeRightLP Tier 1.4 - bound_rule floor clamps extraction upper bound")
{
  // Reservoir eini = 1500 hm3 (well above breakpoint).  Configure a
  // bound_rule whose first segment evaluates to a *negative* value at
  // the reservoir level — the floor must clamp the result to 25.0.
  const HydroFixture fx;
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {1},
          .name = "vfloor",
          .reservoir = Uid {1},
          .demand = 100.0,
          .fail_cost = 5000.0,
          .bound_rule =
              RightBoundRule {
                  .reservoir = Uid {1},
                  .segments =
                      {
                          {
                              .volume = 0.0,
                              .slope = -1.0,
                              .constant = 0.0,
                          },
                      },
                  .floor = 25.0,
              },
      },
  };

  const auto simulation = make_single_stage_simulation();
  const auto system = make_system(fx, vrs, "Tier1_4_Floor");

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  const auto& vr_lp = system_lp.elements<VolumeRightLP>().front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  const auto& ext = vr_lp.extraction_cols_at(scenarios[0], stages[0]);
  REQUIRE(!ext.empty());

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto col_upp = lp.get_col_upp();
  for (const auto& [buid, col] : ext) {
    // Without the floor, slope=-1 * 1500 = -1500 would clamp the
    // column to a negative bound.  The floor should bring it to 25.
    CHECK(col_upp[col] == doctest::Approx(25.0));
  }
}

// ── 1.5 reset_month re-provisioning ───────────────────────────────────────

TEST_CASE(  // NOLINT
    "VolumeRightLP Tier 1.5 - reset_month fixes eini at re-provision stage")
{
  // Single-stage simulation whose stage carries the reset month.
  // For the first (and only) stage, eini_col_at returns the global
  // initial-condition column (no upstream storage balance) so the
  // re-provisioning fix becomes a clean equality bound on that column.
  // Without bound_rule, the provision value must be `emax`.
  const HydroFixture fx;
  constexpr double emax_val = 800.0;
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {1},
          .name = "vreset",
          .reservoir = Uid {1},
          .emax = emax_val,
          .eini = 0.0,
          .demand = 50.0,
          .fail_cost = 1000.0,
          .use_state_variable = false,
          .reset_month = MonthType::april,
      },
  };

  const auto simulation = make_single_stage_simulation();
  const auto system = make_system(fx, vrs, "Tier1_5_Reset");

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  const auto& vr_lp = system_lp.elements<VolumeRightLP>().front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  REQUIRE(scenarios.size() == 1);
  REQUIRE(stages.size() == 1);

  const auto& reset_stage = stages[0];  // April (matches reset_month)
  const auto eini_col = vr_lp.eini_col_at(scenarios[0], reset_stage);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto col_low = lp.get_col_low();
  const auto col_upp = lp.get_col_upp();
  CHECK(col_low[eini_col] == doctest::Approx(emax_val));
  CHECK(col_upp[eini_col] == doctest::Approx(emax_val));
}

// ── 1.6 update_lp cache hit / miss ────────────────────────────────────────

TEST_CASE(  // NOLINT
    "VolumeRightLP Tier 1.6 - update_lp cache returns 0 on no-op")
{
  // First call to update_lp after add_to_lp should be a no-op because
  // initial_rule_bound was already cached during construction.  A
  // second call without any change must also return 0.
  const HydroFixture fx;
  const Array<VolumeRight> vrs = {
      {
          .uid = Uid {1},
          .name = "vcache",
          .reservoir = Uid {1},
          .demand = 100.0,
          .fail_cost = 5000.0,
          .bound_rule =
              RightBoundRule {
                  .reservoir = Uid {1},
                  .segments =
                      {
                          {
                              .volume = 0.0,
                              .slope = 0.10,
                              .constant = 50.0,
                          },
                      },
                  .cap = 5000.0,
              },
      },
  };

  const auto simulation = make_single_stage_simulation();
  const auto system = make_system(fx, vrs, "Tier1_6_Cache");

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& vr_lp = system_lp.elements<VolumeRightLP>().front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  REQUIRE(!scenarios.empty());
  REQUIRE(!stages.empty());

  // Reservoir volume is unchanged from add_to_lp time → bound rule
  // value is identical → cache must short-circuit and return 0.
  const auto first_call = vr_lp.update_lp(system_lp, scenarios[0], stages[0]);
  CHECK(first_call == 0);

  const auto second_call = vr_lp.update_lp(system_lp, scenarios[0], stages[0]);
  CHECK(second_call == 0);
}
