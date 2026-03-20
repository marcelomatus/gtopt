/**
 * @file      test_flow_discharge_lp.hpp
 * @brief     Unit tests verifying that Flow.discharge values are loaded into
 *            the LP as fixed-variable column bounds and that aperture updates
 *            correctly switch per-scenario discharge data
 * @date      2026-03-19
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. Scalar discharge is loaded as fixed LP column bounds (lowb == uppb)
 *  2. Per-block vector discharge produces correct per-block bounds
 *  3. Multi-scenario 3-D discharge: each scenario's columns get correct values
 *  4. Discharge values participate in junction balance (LP solution uses them)
 *  5. Aperture update correctly replaces discharge bounds per-block
 *  6. Two independent aperture clones have independent discharge bounds
 *  7. Aperture update changes LP objective (more inflow → lower cost)
 */

#pragma once

#include <doctest/doctest.h>
#include <gtopt/flow_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── 1. Scalar discharge loaded as fixed LP bounds ──────────────────────────

TEST_CASE("Flow discharge scalar loaded into LP bounds")  // NOLINT
{
  // A constant discharge = 25.0 m³/s should produce flow columns with
  // lowb == uppb == 25.0 for every block.

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 80.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
          .drain = true,
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "river_inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 25.0,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 200.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 2.0,
              },
              {
                  .uid = Uid {3},
                  .duration = 1.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 3,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {1},
              },
          },
  };

  const System system = {
      .name = "ScalarDischargeTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  Options opts;
  opts.use_single_bus = true;
  opts.demand_fail_cost = OptReal {1000.0};

  const OptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  auto& li = sys_lp.linear_interface();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Access Flow LP elements
  const auto& flow_elements = sys_lp.elements<FlowLP>();
  REQUIRE(flow_elements.size() == 1);
  const auto& flow_lp = flow_elements[0];

  const auto& scene = sim_lp.scenes()[SceneIndex {0}];
  const auto& scenario = scene.scenarios()[0];
  const auto& phase = sim_lp.phases()[PhaseIndex {0}];
  const auto& stage = phase.stages()[0];

  const auto& fcols = flow_lp.flow_cols_at(scenario, stage);
  REQUIRE(fcols.size() == 3);  // 3 blocks

  const auto col_low = li.get_col_low();
  const auto col_upp = li.get_col_upp();
  const auto col_sol = li.get_col_sol();

  for (const auto& [buid, col] : fcols) {
    // Column bounds must equal the scalar discharge value
    CHECK(col_low[col] == doctest::Approx(25.0));
    CHECK(col_upp[col] == doctest::Approx(25.0));
    // Solution must equal the fixed value
    CHECK(col_sol[col] == doctest::Approx(25.0));
  }
}

// ─── 2. Per-block vector discharge ──────────────────────────────────────────

TEST_CASE("Flow discharge per-block vector loaded into LP bounds")  // NOLINT
{
  // discharge = [10.0, 30.0] for blocks 1 and 2 respectively.
  // Each block's flow column should have the corresponding bound.

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 80.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
          .drain = true,
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  // Per-block discharge as 3-D vector: [scenario][stage][block]
  const STBRealFieldSched discharge_sched {
      std::vector<std::vector<std::vector<double>>> {
          {
              {
                  10.0,
                  30.0,
              },
          },
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "river_inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = discharge_sched,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 200.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 1.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 2,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {1},
              },
          },
  };

  const System system = {
      .name = "VectorDischargeTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  Options opts;
  opts.use_single_bus = true;
  opts.demand_fail_cost = OptReal {1000.0};

  const OptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  auto& li = sys_lp.linear_interface();
  auto result = li.resolve();
  REQUIRE(result.has_value());

  const auto& flow_lp = sys_lp.elements<FlowLP>()[0];
  const auto& scenario = sim_lp.scenes()[SceneIndex {0}].scenarios()[0];
  const auto& stage = sim_lp.phases()[PhaseIndex {0}].stages()[0];

  const auto& fcols = flow_lp.flow_cols_at(scenario, stage);
  REQUIRE(fcols.size() == 2);

  const auto col_low = li.get_col_low();
  const auto col_upp = li.get_col_upp();

  // Block 1 (uid=1) → 10.0, Block 2 (uid=2) → 30.0
  const double expected[] = {10.0, 30.0};
  int idx = 0;
  for (const auto& [buid, col] : fcols) {
    CHECK(col_low[col] == doctest::Approx(expected[idx]));
    CHECK(col_upp[col] == doctest::Approx(expected[idx]));
    ++idx;
  }
}

// ─── 3. Multi-scenario discharge: each scenario gets correct bounds ─────────

TEST_CASE(  // NOLINT
    "Flow discharge multi-scenario 3D vector produces correct per-scenario "
    "bounds")
{
  // Scenario 1: discharge = 8 m³/s for all blocks
  // Scenario 2: discharge = 40 m³/s for all blocks
  // The LP is built with columns for both scenarios; each scenario's
  // flow columns should have the corresponding discharge bound.

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 80.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
          .drain = true,
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  const STBRealFieldSched discharge_sched {
      std::vector<std::vector<std::vector<double>>> {
          {
              {
                  8.0,
                  8.0,
              },
          },  // scenario 1
          {
              {
                  40.0,
                  40.0,
              },
          },  // scenario 2
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "river_inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = discharge_sched,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 200.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 1.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 2,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {1},
                  .probability_factor = 0.5,
              },
              {
                  .uid = Uid {2},
                  .probability_factor = 0.5,
              },
          },
  };

  const System system = {
      .name = "MultiScenarioDischargeTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  Options opts;
  opts.use_single_bus = true;
  opts.demand_fail_cost = OptReal {1000.0};

  const OptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  auto& li = sys_lp.linear_interface();
  auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto& flow_lp = sys_lp.elements<FlowLP>()[0];
  const auto& scene = sim_lp.scenes()[SceneIndex {0}];
  const auto& scenarios = scene.scenarios();
  REQUIRE(scenarios.size() == 2);

  const auto& stage = sim_lp.phases()[PhaseIndex {0}].stages()[0];

  const auto col_low = li.get_col_low();
  const auto col_upp = li.get_col_upp();
  const auto col_sol = li.get_col_sol();

  SUBCASE("scenario 1 flow columns have discharge = 8")
  {
    const auto& fcols = flow_lp.flow_cols_at(scenarios[0], stage);
    REQUIRE(fcols.size() == 2);
    for (const auto& [buid, col] : fcols) {
      CHECK(col_low[col] == doctest::Approx(8.0));
      CHECK(col_upp[col] == doctest::Approx(8.0));
      CHECK(col_sol[col] == doctest::Approx(8.0));
    }
  }

  SUBCASE("scenario 2 flow columns have discharge = 40")
  {
    const auto& fcols = flow_lp.flow_cols_at(scenarios[1], stage);
    REQUIRE(fcols.size() == 2);
    for (const auto& [buid, col] : fcols) {
      CHECK(col_low[col] == doctest::Approx(40.0));
      CHECK(col_upp[col] == doctest::Approx(40.0));
      CHECK(col_sol[col] == doctest::Approx(40.0));
    }
  }
}

// ─── 4. Discharge values participate in junction balance ────────────────────

TEST_CASE(  // NOLINT
    "Flow discharge values affect hydro generation via junction balance")
{
  // Build a hydro system where the inflow (discharge) directly affects how
  // much the hydro generator can produce.  With higher inflow, more hydro
  // generation displaces expensive thermal generation, resulting in a lower
  // objective value.

  auto solve_with_discharge = [](double discharge_val) -> double
  {
    const Array<Bus> bus_array = {
        {
            .uid = Uid {1},
            .name = "b1",
        },
    };

    const Array<Generator> generator_array = {
        {
            .uid = Uid {1},
            .name = "hydro_gen",
            .bus = Uid {1},
            .gcost = 5.0,
            .capacity = 200.0,
        },
        {
            .uid = Uid {2},
            .name = "thermal_gen",
            .bus = Uid {1},
            .gcost = 100.0,
            .capacity = 200.0,
        },
    };

    const Array<Demand> demand_array = {
        {
            .uid = Uid {1},
            .name = "d1",
            .bus = Uid {1},
            .capacity = 80.0,
        },
    };

    const Array<Junction> junction_array = {
        {
            .uid = Uid {1},
            .name = "j_up",
            .drain = true,
        },
        {
            .uid = Uid {2},
            .name = "j_down",
            .drain = true,
        },
    };

    const Array<Flow> flow_array = {
        {
            .uid = Uid {1},
            .name = "inflow",
            .direction = 1,
            .junction = Uid {1},
            .discharge = discharge_val,
        },
    };

    const Array<Waterway> waterway_array = {
        {
            .uid = Uid {1},
            .name = "ww1",
            .junction_a = Uid {1},
            .junction_b = Uid {2},
            .fmin = 0.0,
            .fmax = 500.0,
        },
    };

    const Array<Turbine> turbine_array = {
        {
            .uid = Uid {1},
            .name = "tur1",
            .waterway = Uid {1},
            .generator = Uid {1},
            .conversion_rate = 1.0,
        },
    };

    const Simulation simulation = {
        .block_array =
            {
                {
                    .uid = Uid {1},
                    .duration = 1.0,
                },
            },
        .stage_array =
            {
                {
                    .uid = Uid {1},
                    .first_block = 0,
                    .count_block = 1,
                },
            },
        .scenario_array =
            {
                {
                    .uid = Uid {1},
                },
            },
    };

    const System system = {
        .name = "DischargeJunctionTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .flow_array = flow_array,
        .turbine_array = turbine_array,
    };

    Options opts;
    opts.use_single_bus = true;
    opts.demand_fail_cost = OptReal {1000.0};
    opts.scale_objective = OptReal {1.0};

    const OptionsLP options(opts);
    SimulationLP sim_lp(simulation, options);
    SystemLP sys_lp(system, sim_lp);

    auto& li = sys_lp.linear_interface();
    const auto res = li.resolve();
    REQUIRE(res.has_value());
    CHECK(res.value() == 0);

    return li.get_obj_value();
  };

  const auto obj_low_inflow = solve_with_discharge(10.0);
  const auto obj_high_inflow = solve_with_discharge(100.0);

  // Higher inflow → more cheap hydro → lower cost
  CHECK(obj_high_inflow < obj_low_inflow);
}

// ─── 5. Aperture update replaces discharge bounds per-block ─────────────────

TEST_CASE(  // NOLINT
    "Aperture update correctly replaces discharge bounds per-block")
{
  // Build a 2-scenario system with different per-block discharge values.
  // Verify that update_aperture_lp replaces per-block values individually.

  // Scenario 1: [12.0, 18.0]  (block 1, block 2)
  // Scenario 2: [45.0, 55.0]
  const STBRealFieldSched discharge_sched {
      std::vector<std::vector<std::vector<double>>> {
          {
              {
                  12.0,
                  18.0,
              },
          },
          {
              {
                  45.0,
                  55.0,
              },
          },
      },
  };

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 80.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
          .drain = true,
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = discharge_sched,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 200.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 1.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 2,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {1},
                  .probability_factor = 0.5,
              },
              {
                  .uid = Uid {2},
                  .probability_factor = 0.5,
              },
          },
  };

  const System system = {
      .name = "AperturePerBlockTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  Options opts;
  opts.use_single_bus = true;
  opts.demand_fail_cost = OptReal {1000.0};

  const OptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  auto& li = sys_lp.linear_interface();
  auto result = li.resolve();
  REQUIRE(result.has_value());

  const auto& scene = sim_lp.scenes()[SceneIndex {0}];
  const auto& scenarios = scene.scenarios();
  REQUIRE(scenarios.size() == 2);
  const auto& base = scenarios[0];
  const auto& aperture = scenarios[1];
  const auto& stage = sim_lp.phases()[PhaseIndex {0}].stages()[0];
  const auto& flow_lp = sys_lp.elements<FlowLP>()[0];
  const auto& fcols = flow_lp.flow_cols_at(base, stage);
  REQUIRE(fcols.size() == 2);

  SUBCASE("original has base scenario per-block values [12, 18]")
  {
    // PLP affluent values are loaded as fixed LP variables (lowb == uppb),
    // fixing the discharge at the exact value for each block.
    const auto low = li.get_col_low();
    const auto upp = li.get_col_upp();
    std::vector<double> lo_bounds;
    std::vector<double> up_bounds;
    for (const auto& [buid, col] : fcols) {
      lo_bounds.push_back(low[col]);
      up_bounds.push_back(upp[col]);
    }
    REQUIRE(lo_bounds.size() == 2);
    CHECK(lo_bounds[0] == doctest::Approx(12.0));
    CHECK(lo_bounds[1] == doctest::Approx(18.0));
    // Verify fixed-variable property: low == upp for each block
    CHECK(up_bounds[0] == doctest::Approx(12.0));
    CHECK(up_bounds[1] == doctest::Approx(18.0));
  }

  SUBCASE("after aperture update, per-block values are [45, 55]")
  {
    auto clone = li.clone();
    const auto ok = flow_lp.update_aperture_lp(clone, base, aperture, stage);
    REQUIRE(ok);

    const auto low = clone.get_col_low();
    const auto upp = clone.get_col_upp();
    std::vector<double> lo_bounds;
    std::vector<double> up_bounds;
    for (const auto& [buid, col] : fcols) {
      lo_bounds.push_back(low[col]);
      up_bounds.push_back(upp[col]);
    }
    REQUIRE(lo_bounds.size() == 2);
    CHECK(lo_bounds[0] == doctest::Approx(45.0));
    CHECK(lo_bounds[1] == doctest::Approx(55.0));
    CHECK(up_bounds[0] == doctest::Approx(45.0));
    CHECK(up_bounds[1] == doctest::Approx(55.0));
  }

  SUBCASE("clone solves after aperture update")
  {
    auto clone = li.clone();
    [[maybe_unused]] const auto ok =
        flow_lp.update_aperture_lp(clone, base, aperture, stage);
    auto res = clone.resolve();
    REQUIRE(res.has_value());
    CHECK(res.value() == 0);
  }

  SUBCASE("original bounds unchanged after clone update")
  {
    auto clone = li.clone();
    [[maybe_unused]] const auto ok =
        flow_lp.update_aperture_lp(clone, base, aperture, stage);
    const auto low = li.get_col_low();
    std::vector<double> bounds;
    for (const auto& [buid, col] : fcols) {
      bounds.push_back(low[col]);
    }
    CHECK(bounds[0] == doctest::Approx(12.0));
    CHECK(bounds[1] == doctest::Approx(18.0));
  }
}

// ─── 6. Two independent clones have independent discharge values ────────────

TEST_CASE(  // NOLINT
    "Two aperture clones with different discharge are independent")
{
  // 3 scenarios: dry=5, normal=20, wet=50
  // Clone 1 → update to normal; Clone 2 → update to wet.
  // Verify independence.

  const STBRealFieldSched discharge_sched {
      std::vector<std::vector<std::vector<double>>> {
          {
              {
                  5.0,
              },
          },
          {
              {
                  20.0,
              },
          },
          {
              {
                  50.0,
              },
          },
      },
  };

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hg",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "tg",
          .bus = Uid {1},
          .gcost = 80.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
          .drain = true,
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = discharge_sched,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 200.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {1},
                  .probability_factor = 0.3,
              },
              {
                  .uid = Uid {2},
                  .probability_factor = 0.4,
              },
              {
                  .uid = Uid {3},
                  .probability_factor = 0.3,
              },
          },
  };

  const System system = {
      .name = "IndependentClonesTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  Options opts;
  opts.use_single_bus = true;
  opts.demand_fail_cost = OptReal {1000.0};

  const OptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  auto& li = sys_lp.linear_interface();
  auto result = li.resolve();
  REQUIRE(result.has_value());

  const auto& scene = sim_lp.scenes()[SceneIndex {0}];
  const auto& scenarios = scene.scenarios();
  const auto& stage = sim_lp.phases()[PhaseIndex {0}].stages()[0];
  const auto& flow_lp = sys_lp.elements<FlowLP>()[0];
  const auto& fcols = flow_lp.flow_cols_at(scenarios[0], stage);
  REQUIRE(fcols.size() == 1);

  auto clone_normal = li.clone();
  auto clone_wet = li.clone();

  const auto ok1 = flow_lp.update_aperture_lp(
      clone_normal, scenarios[0], scenarios[1], stage);
  const auto ok2 =
      flow_lp.update_aperture_lp(clone_wet, scenarios[0], scenarios[2], stage);
  REQUIRE(ok1);
  REQUIRE(ok2);

  const auto normal_low = clone_normal.get_col_low();
  const auto wet_low = clone_wet.get_col_low();
  const auto orig_low = li.get_col_low();

  for (const auto& [buid, col] : fcols) {
    CHECK(orig_low[col] == doctest::Approx(5.0));
    CHECK(normal_low[col] == doctest::Approx(20.0));
    CHECK(wet_low[col] == doctest::Approx(50.0));
  }

  // Both clones solve optimally
  auto res_normal = clone_normal.resolve();
  auto res_wet = clone_wet.resolve();
  REQUIRE(res_normal.has_value());
  REQUIRE(res_wet.has_value());
  CHECK(res_normal.value() == 0);
  CHECK(res_wet.value() == 0);
}

// ─── 7. Aperture update changes LP objective ────────────────────────────────

TEST_CASE(  // NOLINT
    "Aperture discharge update measurably changes LP objective value")
{
  // Scenario 1 (base): low discharge = 5 m³/s
  // Scenario 2: high discharge = 80 m³/s
  // Higher inflow → more hydro generation → lower cost.

  const STBRealFieldSched discharge_sched {
      std::vector<std::vector<std::vector<double>>> {
          {
              {
                  5.0,
              },
          },
          {
              {
                  80.0,
              },
          },
      },
  };

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hg",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "tg",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
          .drain = true,
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = discharge_sched,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 200.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {1},
                  .probability_factor = 0.5,
              },
              {
                  .uid = Uid {2},
                  .probability_factor = 0.5,
              },
          },
  };

  const System system = {
      .name = "ApertureObjChangeTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  Options opts;
  opts.use_single_bus = true;
  opts.demand_fail_cost = OptReal {1000.0};
  opts.scale_objective = OptReal {1.0};

  const OptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  auto& li = sys_lp.linear_interface();
  auto base_result = li.resolve();
  REQUIRE(base_result.has_value());
  const double base_obj = li.get_obj_value();

  const auto& scene = sim_lp.scenes()[SceneIndex {0}];
  const auto& base_scenario = scene.scenarios()[0];
  const auto& high_scenario = scene.scenarios()[1];
  const auto& stage = sim_lp.phases()[PhaseIndex {0}].stages()[0];
  const auto& flow_lp = sys_lp.elements<FlowLP>()[0];

  auto clone = li.clone();
  const auto ok =
      flow_lp.update_aperture_lp(clone, base_scenario, high_scenario, stage);
  REQUIRE(ok);

  auto clone_result = clone.resolve();
  REQUIRE(clone_result.has_value());
  const double high_obj = clone.get_obj_value();

  // Higher inflow → more cheap hydro → lower cost
  CHECK(high_obj < base_obj);
}
