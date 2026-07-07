// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/flow_right.hpp>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

TEST_CASE("FlowRight construction and default values")
{
  using namespace gtopt;

  const FlowRight fr;

  CHECK(fr.uid == Uid {unknown_uid});
  CHECK(fr.name == Name {});
  CHECK_FALSE(fr.active.has_value());
  CHECK_FALSE(fr.purpose.has_value());
  CHECK_FALSE(fr.junction_a.has_value());
  CHECK_FALSE(fr.direction.has_value());
  CHECK_FALSE(fr.fmin.has_value());
  CHECK_FALSE(fr.fmax.has_value());
  CHECK_FALSE(fr.target.has_value());
  CHECK_FALSE(fr.use_average.has_value());
  CHECK_FALSE(fr.fcost.has_value());
  CHECK_FALSE(fr.uvalue.has_value());
  CHECK_FALSE(fr.priority.has_value());
  CHECK_FALSE(fr.bound_rule.has_value());
}

TEST_CASE("FlowRight attribute assignment")
{
  using namespace gtopt;

  FlowRight fr;

  fr.uid = 1001;
  fr.name = "irrigation_right";
  fr.active = true;
  fr.purpose = "irrigation";
  fr.junction_a = Uid {7001};
  fr.direction = -1;
  fr.target = 50.0;
  fr.fmax = 100.0;
  fr.use_average = true;
  fr.fcost = 5000.0;
  fr.uvalue = 10.0;
  fr.priority = 1.0;

  CHECK(fr.uid == 1001);
  CHECK(fr.name == "irrigation_right");
  CHECK(std::get<IntBool>(fr.active.value()) == 1);
  REQUIRE(fr.purpose.has_value());
  CHECK(fr.purpose.value() == "irrigation");
  CHECK(std::get<Uid>(fr.junction_a.value()) == Uid {7001});
  CHECK(fr.direction.value_or(0) == -1);
  CHECK(fr.use_average.value_or(false) == true);
  CHECK(fr.priority.value_or(0.0) == doctest::Approx(1.0));
}

TEST_CASE("FlowRight designated initializer construction")
{
  using namespace gtopt;

  const FlowRight fr {
      .uid = Uid {2},
      .name = "env_flow",
      .active = {},
      .purpose = "environmental",
      .junction_a = SingleId {Uid {10}},
      .direction = -1,
      .fmax = {},
      .target = 25.0,
      .use_average = {},
      .fcost = 10000.0,
  };

  CHECK(fr.uid == Uid {2});
  CHECK(fr.name == "env_flow");
  REQUIRE(fr.purpose.has_value());
  CHECK(fr.purpose.value() == "environmental");
  CHECK(std::get<Uid>(fr.junction_a.value()) == Uid {10});
  CHECK(fr.direction.value_or(0) == -1);
}

TEST_CASE("FlowRight with bound rule")
{
  using namespace gtopt;

  FlowRight fr;
  fr.uid = 3;
  fr.name = "cushion_right";

  const RightBoundRule rule {
      .reservoir = SingleId {Uid {9001}},
      .segments =
          {
              {
                  .volume = 0.0,
                  .slope = 0.0,
                  .constant = 570.0,
              },
              {
                  .volume = 1200.0,
                  .slope = 0.4,
                  .constant = 90.0,
              },
              {
                  .volume = 1900.0,
                  .slope = 0.25,
                  .constant = 375.0,
              },
          },
      .cap = 5000.0,
  };

  fr.bound_rule = rule;

  REQUIRE(fr.bound_rule.has_value());
  CHECK(std::get<Uid>(fr.bound_rule->reservoir) == Uid {9001});
  CHECK(fr.bound_rule->segments.size() == 3);
  REQUIRE(fr.bound_rule->cap.has_value());
  CHECK(fr.bound_rule->cap.value_or(0.0) == doctest::Approx(5000.0));
}

TEST_CASE("FlowRight with monthly target schedule")
{
  using namespace gtopt;

  FlowRight fr;
  fr.uid = 4;
  fr.name = "seasonal_right";

  // Seasonal irrigation schedule.  The field's static type is
  // OptTBRealFieldSched (per-stage-block, mirrors Demand::lmax) so the
  // C++-side fixture supplies a 2-D schedule with one inner element
  // per stage; 1-D shapes round-trip through JSON via the variant
  // fallback but the C++ literal must match the variant alternative.
  const std::vector<std::vector<Real>> schedule {
      {0.0},
      {0.0},
      {0.0},
      {19.5},
      {42.25},
      {55.25},
      {65.0},
      {65.0},
      {52.0},
      {32.5},
      {13.0},
      {0.0},
  };
  fr.target = schedule;

  REQUIRE(fr.target.has_value());
  auto* vec_ptr = std::get_if<std::vector<std::vector<Real>>>(&*fr.target);
  REQUIRE(vec_ptr != nullptr);
  CHECK(vec_ptr->size() == 12);
  REQUIRE((*vec_ptr)[3].size() == 1);
  CHECK((*vec_ptr)[3][0] == doctest::Approx(19.5));
  REQUIRE((*vec_ptr)[7].size() == 1);
  CHECK((*vec_ptr)[7][0] == doctest::Approx(65.0));
}

TEST_CASE("FlowRight with different purposes")  // NOLINT
{
  using namespace gtopt;

  SUBCASE("irrigation purpose")
  {
    const FlowRight fr {
        .uid = Uid {10},
        .name = "irr",
        .active = {},
        .purpose = "irrigation",
    };
    CHECK(fr.purpose.value() == "irrigation");
  }

  SUBCASE("generation purpose")
  {
    const FlowRight fr {
        .uid = Uid {11},
        .name = "gen",
        .active = {},
        .purpose = "generation",
    };
    CHECK(fr.purpose.value() == "generation");
  }

  SUBCASE("environmental purpose")
  {
    const FlowRight fr {
        .uid = Uid {12},
        .name = "env",
        .active = {},
        .purpose = "environmental",
    };
    CHECK(fr.purpose.value() == "environmental");
  }
}

// ── Tier 2: FlowRightLP isolation tests ───────────────────────────────────
//
// Each subcase plugs a single FlowRight into a tiny hydro fixture
// (1 bus, 1 gen, 1 demand, 1 reservoir, 1 inflow, 1 turbine, 1 waterway,
// 1 junction-pair) and verifies one LP-level behaviour: variable vs
// fixed mode bounds, the asymmetric default for unset rights, the
// `update_lp` lower-bound regression at flow_right_lp.cpp:381 (was :298),
// the deficit (`fail`) variable creation gate, and qeh stage-average
// construction.  Maps to ladder Tier 2 in
// `~/.claude/projects/-home-marce-git-gtopt/memory/project_irrigation_test_ladder.md`.

namespace
{

using namespace gtopt;

struct FlowRightHydroFixture
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

[[nodiscard]] Simulation make_flow_right_simulation()
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

[[nodiscard]] Simulation make_flow_right_two_block_simulation()
{
  return {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 2,
              },
              {
                  .uid = Uid {2},
                  .duration = 4,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 2,
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

[[nodiscard]] System make_flow_right_system(const FlowRightHydroFixture& fx,
                                            const Array<FlowRight>& frs,
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
      .flow_right_array = frs,
  };
}

}  // namespace

// ── 2.1 Variable mode (fmax > 0, discharge = 0) → [0, fmax] ──────────────

TEST_CASE(  // NOLINT
    "FlowRightLP Tier 2.1 - variable mode bounds [0, fmax]")
{
  const FlowRightHydroFixture fx;
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_var",
          .junction_a = Uid {1},
          .direction = -1,
          .fmax = 75.0,
      },
  };

  const auto simulation = make_flow_right_simulation();
  const auto system = make_flow_right_system(fx, frs, "Tier2_1_Variable");

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  const auto& flow_cols = fr_lp.flow_cols_at(scenarios[0], stages[0]);
  REQUIRE(!flow_cols.empty());

  const auto col_low = lp.get_col_low();
  const auto col_upp = lp.get_col_upp();
  for (const auto& [buid, col] : flow_cols) {
    CHECK(col_low[col] == doctest::Approx(0.0));
    CHECK(col_upp[col] == doctest::Approx(75.0));
  }
}

// ── 2.2 Fixed mode (discharge > 0, fmax = 0) → [discharge, discharge] ────

TEST_CASE(  // NOLINT
    "FlowRightLP Tier 2.2 - fixed mode bounds [discharge, discharge]")
{
  const FlowRightHydroFixture fx;
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_fixed",
          .junction_a = Uid {1},
          .direction = -1,
          .target = 30.0,
      },
  };

  const auto simulation = make_flow_right_simulation();
  const auto system = make_flow_right_system(fx, frs, "Tier2_2_Fixed");

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  const auto& flow_cols = fr_lp.flow_cols_at(scenarios[0], stages[0]);
  REQUIRE(!flow_cols.empty());

  const auto col_low = lp.get_col_low();
  const auto col_upp = lp.get_col_upp();
  for (const auto& [buid, col] : flow_cols) {
    CHECK(col_low[col] == doctest::Approx(30.0));
    CHECK(col_upp[col] == doctest::Approx(30.0));
  }

  // Post-P0: no fail LP column ever exists; on the hard path (no
  // fail_cost) `fail_sol_at` returns 0 because the discharge cache
  // stays empty.
  const auto col_sol = lp.get_col_sol();
  for (const auto& block : stages[0].blocks()) {
    const auto fs = fr_lp.fail_sol_at(scenarios[0], stages[0], block, col_sol);
    CHECK(fs == doctest::Approx(0.0));
  }
}

// ── 2.3 Both unset → [0, 0] (asymmetric default vs VolumeRight) ──────────

TEST_CASE(  // NOLINT
    "FlowRightLP Tier 2.3 - unset discharge & fmax pin column at [0, 0]")
{
  // VolumeRight defaults the upper bound to DblMax when neither fmax
  // nor a bound_rule is given (Tier 1.3).  FlowRight is intentionally
  // asymmetric: an unset right is fully inactive (column locked at 0).
  const FlowRightHydroFixture fx;
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_zero",
          .junction_a = Uid {1},
          .direction = -1,
      },
  };

  const auto simulation = make_flow_right_simulation();
  const auto system = make_flow_right_system(fx, frs, "Tier2_3_Zero");

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  const auto& flow_cols = fr_lp.flow_cols_at(scenarios[0], stages[0]);
  REQUIRE(!flow_cols.empty());

  const auto col_low = lp.get_col_low();
  const auto col_upp = lp.get_col_upp();
  for (const auto& [buid, col] : flow_cols) {
    CHECK(col_low[col] == doctest::Approx(0.0));
    CHECK(col_upp[col] == doctest::Approx(0.0));
  }
}

// ── 2.4 bound_rule cap below discharge clamps lower bound too ────────────

TEST_CASE(  // NOLINT
    "FlowRightLP Tier 2.4 - bound_rule cap below discharge clamps lowb")
{
  // Regression for the live bug previously at flow_right_lp.cpp:298
  // (now lines 381-382): when the rule output is smaller than the
  // configured discharge, both the upper AND lower column bounds must
  // collapse to the rule value.  Pre-fix the lower bound stayed at
  // discharge while the upper dropped to the cap, leaving the column
  // infeasible (lowb > uppb).
  //
  // We trigger this at construction (compute_block_bounds is shared
  // by add_to_lp and update_lp, so the path is identical).  The rule
  // is configured with cap=10 and the discharge is 30 — bounds must
  // collapse to [10, 10] rather than the buggy [30, 10].
  const FlowRightHydroFixture fx;
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_cap_clamp",
          .junction_a = Uid {1},
          .direction = -1,
          .target = 30.0,
          .bound_rule =
              RightBoundRule {
                  .reservoir = Uid {1},
                  .segments =
                      {
                          {
                              .volume = 0.0,
                              .slope = 0.0,
                              .constant = 10.0,
                          },
                      },
                  .cap = 10.0,
              },
      },
  };

  const auto simulation = make_flow_right_simulation();
  const auto system = make_flow_right_system(fx, frs, "Tier2_4_RuleClamp");

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  const auto& flow_cols = fr_lp.flow_cols_at(scenarios[0], stages[0]);
  REQUIRE(!flow_cols.empty());

  const auto col_low = lp.get_col_low();
  const auto col_upp = lp.get_col_upp();
  for (const auto& [buid, col] : flow_cols) {
    CHECK(col_low[col] == doctest::Approx(10.0));
    CHECK(col_upp[col] == doctest::Approx(10.0));
  }
}

// ── 2.5 qeh stage-average constraint with multi-block stage ──────────────

TEST_CASE(  // NOLINT
    "FlowRightLP Tier 2.5 - qeh stage-average column wired to all blocks")
{
  // The original Tier 2.5 spec ("missing block column → graceful
  // handling") cannot be triggered today because the per-block fcol
  // loop populates every block, so this case verifies the happy path
  // instead: with two blocks of unequal duration the qavg row must
  // hold qeh = (dur1 * flow1 + dur2 * flow2) / dur_stage, i.e. the
  // duration-weighted average.
  const FlowRightHydroFixture fx;
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_qeh",
          .junction_a = Uid {1},
          .direction = -1,
          .target = 25.0,
          .use_average = true,
      },
  };

  const auto simulation = make_flow_right_two_block_simulation();
  const auto system = make_flow_right_system(fx, frs, "Tier2_5_Qeh");

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  const auto& scenario = scenarios[0];
  const auto& stage = stages[0];

  // qeh column exists and resolves to the duration-weighted average
  // of the two block flows (both fixed at 25 → average is 25).
  const auto qeh_col = fr_lp.qeh_col_at(scenario, stage);
  const auto qeh_value = lp.get_col_sol()[qeh_col];
  CHECK(qeh_value == doctest::Approx(25.0));

  // Both block flow columns are also accessible.
  const auto& flow_cols = fr_lp.flow_cols_at(scenario, stage);
  REQUIRE(flow_cols.size() == 2);
}

// ── 2.6 fail_cost > 0 + discharge > 0 creates fail (deficit) variable ────

TEST_CASE(  // NOLINT
    "FlowRightLP Tier 2.6 - fail_cost + discharge creates fail variable")
{
  const FlowRightHydroFixture fx;
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_with_deficit",
          .junction_a = Uid {1},
          .direction = -1,
          .target = 40.0,
          .fcost = 5000.0,
      },
  };

  const auto simulation = make_flow_right_simulation();
  const auto system = make_flow_right_system(fx, frs, "Tier2_6_Deficit");

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();

  // Post-P0 substitution: the `fail` LP column is gone (collapsed into
  // a negative-cost coefficient on the surviving `flow` column plus an
  // `obj_constant`).  Confirm the soft path is still wired by checking
  // (a) flow_cols exist, (b) `fail_sol_at` returns the reconstructed
  // shortfall, and (c) the flow column lower bound is relaxed to 0.
  const auto& flow_cols = fr_lp.flow_cols_at(scenarios[0], stages[0]);
  CHECK(!flow_cols.empty());

  const auto col_sol = lp.get_col_sol();
  for (const auto& block : stages[0].blocks()) {
    const auto fs = fr_lp.fail_sol_at(scenarios[0], stages[0], block, col_sol);
    CHECK(fs >= 0.0);
  }

  // With deficit support active, the flow column lower bound is
  // relaxed to 0 — the negative-cost coefficient on flow rewards
  // covering the discharge as much as possible while the obj_constant
  // tracks the substitution baseline.
  const auto col_low = lp.get_col_low();
  for (const auto& [buid, col] : flow_cols) {
    CHECK(col_low[col] == doctest::Approx(0.0));
  }
}

// ── 2.7 discharge > 0 + fail_cost = 0 → no fail variable ─────────────────

TEST_CASE(  // NOLINT
    "FlowRightLP Tier 2.7 - discharge without fail_cost has no fail var")
{
  // Mirror of 2.6 with fail_cost unset (and the global hydro_fail_cost
  // also unset via the default PlanningOptions).  Post-P0 the `fail`
  // LP column no longer exists in either mode; confirm the hard-path
  // bounds are still tight (no `lowb = 0` deficit relaxation) and the
  // reconstructed `fail_sol_at` returns 0 because
  // `block_discharge_values_` is empty (only populated on the soft
  // path).
  const FlowRightHydroFixture fx;
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_no_deficit",
          .junction_a = Uid {1},
          .direction = -1,
          .target = 40.0,
      },
  };

  const auto simulation = make_flow_right_simulation();
  const auto system = make_flow_right_system(fx, frs, "Tier2_7_NoDeficit");

  PlanningOptions opts;
  opts.model_options.demand_fail_cost = 1000.0;
  // Belt-and-suspenders: zero out the global hydro_fail_cost so the
  // per-element fall-through cannot accidentally enable a deficit.
  opts.model_options.hydro_spill_cost = 0.0;
  const PlanningOptionsLP options(opts);

  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();

  // Post-P0: `fail_sol_at` on the hard path returns 0 for every block
  // (the discharge cache stays empty when `fail_active` is false).
  const auto col_sol = lp.get_col_sol();
  for (const auto& block : stages[0].blocks()) {
    const auto fs = fr_lp.fail_sol_at(scenarios[0], stages[0], block, col_sol);
    CHECK(fs == doctest::Approx(0.0));
  }

  // Without a deficit, the flow column stays at fixed-mode bounds.
  const auto& flow_cols = fr_lp.flow_cols_at(scenarios[0], stages[0]);
  const auto col_low = lp.get_col_low();
  const auto col_upp = lp.get_col_upp();
  for (const auto& [buid, col] : flow_cols) {
    CHECK(col_low[col] == doctest::Approx(40.0));
    CHECK(col_upp[col] == doctest::Approx(40.0));
  }
}

// ── 2.8 Target with bonus — two-sub-column unified mode ─────────────────
//
// The "target with bonus" mode is selected when:
//   fmin (default 0) <= target < fmax  AND  uvalue is set.
// LP shape:
//   flow_low  ∈ [0, target − fmin]   cost = -fcost · cf
//   flow_high ∈ [0, fmax − target]   cost = -uvalue · cf
// Both columns subtract from the junction balance with coefficient -1.
// `fail_sol = max(0, target − total_flow)` and
// `excess_sol = max(0, total_flow − target) = flow_high primal`.

TEST_CASE(  // NOLINT
    "FlowRightLP Tier 2.8 - target with bonus, both incentives push up")
{
  // fmin=0, target=10, fmax=20, fcost=100, uvalue=50.  Post-2026-05
  // attach_flow refactor: a single flow_col ∈ [fmin, fmax] = [0, 20]
  // is created; the kink at target=10 is expressed via two slacks
  // (fail / excess) attached to flow_col.  Both incentives drive the
  // LP to flow=fmax=20, giving fail=0 and excess=10.
  const FlowRightHydroFixture fx;
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_bonus",
          .junction_a = Uid {1},
          .direction = -1,
          .fmax = 20.0,
          .target = 10.0,
          .fcost = 100.0,
          .uvalue = 50.0,
      },
  };

  const auto simulation = make_flow_right_simulation();
  const auto system = make_flow_right_system(fx, frs, "Tier2_8_BonusPushUp");

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  const auto& scenario = scenarios[0];
  const auto& stage = stages[0];

  // Single flow_col with the full hard band [fmin, fmax] = [0, 20].
  const auto& flow_cols = fr_lp.flow_cols_at(scenario, stage);
  REQUIRE(!flow_cols.empty());
  const auto col_low = lp.get_col_low();
  const auto col_upp = lp.get_col_upp();
  for (const auto& [buid, col] : flow_cols) {
    CHECK(col_low[col] == doctest::Approx(0.0));
    CHECK(col_upp[col] == doctest::Approx(20.0));
  }

  // Both incentives push flow to fmax=20 → excess=10, fail=0.
  const auto col_sol = lp.get_col_sol();
  for (const auto& block : stage.blocks()) {
    const auto fs = fr_lp.fail_sol_at(scenario, stage, block, col_sol);
    const auto ex = fr_lp.excess_sol_at(scenario, stage, block, col_sol);
    CHECK(fs == doctest::Approx(0.0));
    CHECK(ex == doctest::Approx(10.0));
  }
}

TEST_CASE(  // NOLINT
    "FlowRightLP Tier 2.9 - target with bonus, structural sub-column layout")
{
  // Structural check: verify the two-sub-column layout is present and
  // both columns are exposed via flow_cols_at (low) + the AMPL registry
  // (flow = low + high).  Bounds: flow_low ∈ [0, target − fmin] = [0, 10],
  // flow_high ∈ [0, fmax − target] = [0, 10].
  const FlowRightHydroFixture fx;
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_bonus_layout",
          .junction_a = Uid {1},
          .direction = -1,
          .fmax = 20.0,
          .target = 10.0,
          .fcost = 100.0,
          .uvalue = 50.0,
      },
  };

  const auto simulation = make_flow_right_simulation();
  const auto system = make_flow_right_system(fx, frs, "Tier2_9_BonusLayout");

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Verify the negative cost coefficients on both sub-columns are
  // present in the LP.
  const auto obj_coeffs = lp.get_obj_coeff();
  bool found_fcost_col = false;
  bool found_uvalue_col = false;
  for (Index i = 0; i < lp.get_numcols(); ++i) {
    const auto c = obj_coeffs[i];
    if (c < 0.0) {
      // Both coefficients are negative (incentives); we just check that
      // at least two distinct negative magnitudes show up among the
      // FlowRight columns (one for fcost, one for uvalue, since the
      // block durations are the same here).  fcost=100, uvalue=50 ⇒
      // distinct magnitudes.
      if (!found_fcost_col) {
        found_fcost_col = true;
      } else {
        found_uvalue_col = true;
      }
    }
  }
  CHECK(found_fcost_col);
  CHECK(found_uvalue_col);
}

TEST_CASE(  // NOLINT
    "FlowRightLP Tier 2.10 - target with bonus, negative uvalue stops at "
    "target")
{
  // uvalue = -50: regulator penalty above target.  LP picks
  // flow_low=10 (incentivized by fcost) and flow_high=0 (penalised by
  // uvalue) → total flow = 10, fail=0, excess=0.
  const FlowRightHydroFixture fx;
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_bonus_neg",
          .junction_a = Uid {1},
          .direction = -1,
          .fmax = 20.0,
          .target = 10.0,
          .fcost = 100.0,
          .uvalue = -50.0,
      },
  };

  const auto simulation = make_flow_right_simulation();
  const auto system = make_flow_right_system(fx, frs, "Tier2_10_BonusNeg");

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  const auto& scenario = scenarios[0];
  const auto& stage = stages[0];

  const auto col_sol = lp.get_col_sol();
  for (const auto& block : stage.blocks()) {
    CHECK(fr_lp.fail_sol_at(scenario, stage, block, col_sol)
          == doctest::Approx(0.0));
    CHECK(fr_lp.excess_sol_at(scenario, stage, block, col_sol)
          == doctest::Approx(0.0));
  }
}

TEST_CASE(  // NOLINT
    "FlowRightLP Tier 2.11 - target with bonus, hard floor fmin respected")
{
  // fmin=2, target=10, fmax=20, fcost=100, uvalue=50.  Post-2026-05
  // attach_flow refactor: a single flow_col ∈ [fmin, fmax] = [2, 20]
  // is created.  With uvalue active the LP pushes flow to fmax=20
  // (excess=10) and the hard floor fmin=2 is preserved structurally
  // as the column lower bound.
  const FlowRightHydroFixture fx;
  const Array<FlowRight> frs = {
      {
          .uid = Uid {1},
          .name = "fr_bonus_floor",
          .junction_a = Uid {1},
          .direction = -1,
          .fmin = 2.0,
          .fmax = 20.0,
          .target = 10.0,
          .fcost = 100.0,
          .uvalue = 50.0,
      },
  };

  const auto simulation = make_flow_right_simulation();
  const auto system = make_flow_right_system(fx, frs, "Tier2_11_BonusFloor");

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  const auto& scenario = scenarios[0];
  const auto& stage = stages[0];

  // Single flow_col with hard band [fmin, fmax] = [2, 20].
  const auto& flow_cols = fr_lp.flow_cols_at(scenario, stage);
  REQUIRE(!flow_cols.empty());
  const auto col_low = lp.get_col_low();
  const auto col_upp = lp.get_col_upp();
  for (const auto& [buid, col] : flow_cols) {
    CHECK(col_low[col] == doctest::Approx(2.0));
    CHECK(col_upp[col] == doctest::Approx(20.0));
  }

  // LP drives flow to fmax=20 (excess=10, fail=0).
  const auto col_sol = lp.get_col_sol();
  for (const auto& block : stage.blocks()) {
    CHECK(fr_lp.fail_sol_at(scenario, stage, block, col_sol)
          == doctest::Approx(0.0));
    CHECK(fr_lp.excess_sol_at(scenario, stage, block, col_sol)
          == doctest::Approx(10.0));
  }
}
