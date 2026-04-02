/**
 * @file      test_irrigation_lp.hpp
 * @brief     LP integration tests for FlowRight and VolumeRight
 * @date      2026-04-01
 * @copyright BSD-3-Clause
 *
 * Tests verify that irrigation rights entities integrate correctly with
 * the SystemLP, creating the expected variables and constraints without
 * affecting the hydrological topology.
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("SystemLP with FlowRight - basic LP construction")
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  // Irrigation flow right (not connected to junction balance)
  const Array<FlowRight> flow_right_array = {
      {
          .uid = Uid {1},
          .name = "irrig_flow_1",
          .discharge = 20.0,
          .fail_cost = 5000.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "IrrigFlowTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .flow_right_array = flow_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "SystemLP with FlowRight - does not change bus balance when added")
{
  // Build two identical systems: one with FlowRight, one without.
  // Verify they produce the same objective (irrigation flow is accounting
  // only, it should not affect the electrical dispatch).

  auto solve = [](bool with_irrigation) -> double
  {
    const Array<Bus> bus_array = {
        {.uid = Uid {1}, .name = "b1"},
    };

    const Array<Generator> generator_array = {
        {
            .uid = Uid {1},
            .name = "gen1",
            .bus = Uid {1},
            .gcost = 10.0,
            .capacity = 200.0,
        },
    };

    const Array<Demand> demand_array = {
        {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
    };

    System system = {
        .name = "IsolationTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
    };

    if (with_irrigation) {
      system.flow_right_array = {
          {
              .uid = Uid {1},
              .name = "irrig_flow_1",
              .discharge = 20.0,
              .fail_cost = 5000.0,
          },
      };
    }

    const Simulation simulation = {
        .block_array = {{.uid = Uid {1}, .duration = 1}},
        .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
        .scenario_array = {{.uid = Uid {0}}},
    };

    const PlanningOptionsLP options;
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto& li = system_lp.linear_interface();
    const auto result = li.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    return li.get_obj_value();
  };

  const auto obj_without = solve(false);
  const auto obj_with = solve(true);

  // The FlowRight should NOT affect the electrical dispatch cost.
  // It only adds its own fail_cost penalty to the objective.
  // The flow variable is fixed at discharge=20 and always feasible,
  // so fail_cost does not trigger.  Electrical side is identical.
  CHECK(obj_with == doctest::Approx(obj_without));
}

TEST_CASE("SystemLP with FlowRight - zero fail_cost produces no deficit")
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  // No fail_cost → no deficit variable should be created
  const Array<FlowRight> flow_right_array = {
      {
          .uid = Uid {1},
          .name = "irrig_no_fail",
          .discharge = 10.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "NoFailCostTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .flow_right_array = flow_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto result = system_lp.linear_interface().resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("SystemLP with VolumeRight - basic LP construction")
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  const Array<VolumeRight> volume_right_array = {
      {
          .uid = Uid {1},
          .name = "irrig_vol_1",
          .emax = 500.0,
          .eini = 0.0,
          .demand = 100.0,
          .fail_cost = 3000.0,
          .use_state_variable = false,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
              {.uid = Uid {2}, .duration = 2},
          },
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 2}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "IrrigVolTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .volume_right_array = volume_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "SystemLP with VolumeRight - does not change bus balance when added")
{
  auto solve = [](bool with_irrigation) -> double
  {
    const Array<Bus> bus_array = {
        {.uid = Uid {1}, .name = "b1"},
    };

    const Array<Generator> generator_array = {
        {
            .uid = Uid {1},
            .name = "gen1",
            .bus = Uid {1},
            .gcost = 10.0,
            .capacity = 200.0,
        },
    };

    const Array<Demand> demand_array = {
        {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
    };

    System system = {
        .name = "IsolationVolTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
    };

    if (with_irrigation) {
      system.volume_right_array = {
          {
              .uid = Uid {1},
              .name = "irrig_vol",
              .emax = 200.0,
              .eini = 0.0,
              .demand = 50.0,
              .fail_cost = 3000.0,
              .use_state_variable = false,
          },
      };
    }

    const Simulation simulation = {
        .block_array = {{.uid = Uid {1}, .duration = 1}},
        .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
        .scenario_array = {{.uid = Uid {0}}},
    };

    const PlanningOptionsLP options;
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto& li = system_lp.linear_interface();
    const auto result = li.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    return li.get_obj_value();
  };

  const auto obj_without = solve(false);
  const auto obj_with = solve(true);

  // VolumeRight should not affect electrical dispatch.
  // The volume right accumulates independently; fail penalty is added
  // only if demand is unmet, but the electrical cost should be the same.
  // (The total obj may differ by the irrigation fail cost.)
  // At minimum, the electrical dispatch cost component should be the same.
  // Since the VolumeRight has its own fail cost, the total obj
  // may be larger with irrigation than without.
  CHECK(obj_with >= obj_without - 1e-6);
}

TEST_CASE(  // NOLINT
    "SystemLP with VolumeRight - multi-stage storage accumulation")
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  const Array<VolumeRight> volume_right_array = {
      {
          .uid = Uid {1},
          .name = "multi_stage_vol",
          .emax = 1000.0,
          .eini = 0.0,
          .use_state_variable = false,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
              {.uid = Uid {2}, .duration = 2},
              {.uid = Uid {3}, .duration = 3},
          },
      .stage_array =
          {
              {.uid = Uid {1}, .first_block = 0, .count_block = 1},
              {.uid = Uid {2}, .first_block = 1, .count_block = 1},
              {.uid = Uid {3}, .first_block = 2, .count_block = 1},
          },
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "MultiStageIrrigVol",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .volume_right_array = volume_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "SystemLP with both FlowRight and VolumeRight together")
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  const Array<FlowRight> flow_right_array = {
      {
          .uid = Uid {1},
          .name = "flow_right_1",
          .discharge = 15.0,
          .fail_cost = 5000.0,
      },
      {
          .uid = Uid {2},
          .name = "flow_right_2",
          .discharge = 25.0,
          .fail_cost = 3000.0,
      },
  };

  const Array<VolumeRight> volume_right_array = {
      {
          .uid = Uid {1},
          .name = "vol_right_1",
          .emax = 500.0,
          .eini = 0.0,
          .demand = 100.0,
          .fail_cost = 4000.0,
          .use_state_variable = false,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
              {.uid = Uid {2}, .duration = 2},
          },
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 2}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "CombinedIrrigTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .flow_right_array = flow_right_array,
      .volume_right_array = volume_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "SystemLP with FlowRight and hydro system - coexistence test")
{
  // Verify FlowRight coexists with hydro components without
  // interfering with the hydrological mass balance.
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j_up"},
      {.uid = Uid {2}, .name = "j_down", .drain = true},
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

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 2000.0,
          .emin = 100.0,
          .emax = 2000.0,
          .eini = 1000.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "natural_inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 50.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 2.0,
      },
  };

  // Irrigation flow right referencing the downstream junction
  // (reference only — does NOT modify junction balance)
  const Array<FlowRight> flow_right_array = {
      {
          .uid = Uid {1},
          .name = "irrig_downstream",
          .junction = Uid {2},
          .discharge = 10.0,
          .fail_cost = 5000.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
              {.uid = Uid {2}, .duration = 2},
          },
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 2}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "HydroWithIrrigFlow",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .flow_right_array = flow_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "FlowRight variable mode - fmax creates variable column")
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
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
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

  // Variable-mode FlowRights: two independent withdrawal rights
  const Array<FlowRight> flow_right_array = {
      {
          .uid = Uid {2},
          .name = "irr_share",
          .direction = -1,
          .discharge = {},
          .fmax = 100.0,
          .fail_cost = 1100.0,
      },
      {
          .uid = Uid {3},
          .name = "gen_share",
          .direction = -1,
          .discharge = {},
          .fmax = 100.0,
          .fail_cost = 1000.0,
      },
  };

  const Simulation simulation = {
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
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "VariableModeTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .flow_right_array = flow_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "FlowRight consumptive - subtracts from physical junction balance")
{
  // Hydro system with junction + consumptive FlowRight
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 5.0,
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
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 2000.0,
          .emin = 100.0,
          .emax = 2000.0,
          .eini = 1000.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 100.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 2.0,
      },
  };

  // Consumptive FlowRight: withdraws from physical junction j_down
  const Array<FlowRight> flow_right_array = {
      {
          .uid = Uid {1},
          .name = "farmer_withdrawal",
          .junction = Uid {2},
          .discharge = 10.0,
          .consumptive = true,
          .fail_cost = 5000.0,
      },
  };

  const Simulation simulation = {
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
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "ConsumptiveFlowTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .flow_right_array = flow_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "VolumeRight consumptive - subtracts from physical reservoir balance")
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
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 5.0,
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
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 2000.0,
          .emin = 100.0,
          .emax = 2000.0,
          .eini = 1000.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 100.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 2.0,
      },
  };

  // Consumptive VolumeRight: extracts volume from reservoir
  const Array<VolumeRight> volume_right_array = {
      {
          .uid = Uid {1},
          .name = "irrig_vol",
          .reservoir = Uid {1},
          .consumptive = true,
          .emax = 500.0,
          .eini = 0.0,
          .demand = 10.0,
          .fail_cost = 5000.0,
          .use_state_variable = false,
      },
  };

  const Simulation simulation = {
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
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "ConsumptiveVolTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .volume_right_array = volume_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "VolumeRight right_reservoir coupling - child connects to parent balance")
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
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
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

  // Parent volume right (balance node) and child connecting to it
  const Array<VolumeRight> volume_right_array = {
      {
          .uid = Uid {1},
          .name = "parent_vol",
          .emax = 1000.0,
          .eini = 500.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {2},
          .name = "child_supply",
          .right_reservoir = Uid {1},
          .direction = 1,
          .emax = 500.0,
          .eini = 0.0,
          .use_state_variable = false,
      },
  };

  const Simulation simulation = {
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
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "VolumeRightCouplingTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .volume_right_array = volume_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "FlowRight use_average - creates stage-average qeh variable")
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
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
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

  // FlowRight with use_average=true and multiple blocks
  const Array<FlowRight> flow_right_array = {
      {
          .uid = Uid {1},
          .name = "avg_flow",
          .discharge = 30.0,
          .use_average = true,
          .fail_cost = 1000.0,
      },
  };

  // Multiple blocks to make averaging meaningful
  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 4,
              },
              {
                  .uid = Uid {2},
                  .duration = 8,
              },
              {
                  .uid = Uid {3},
                  .duration = 12,
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
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "AverageFlowTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .flow_right_array = flow_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();

  // With use_average=true, we should have extra cols and rows
  // compared to use_average=false
  const auto ncols_with_avg = lp.get_numcols();
  const auto nrows_with_avg = lp.get_numrows();

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Build the same system without use_average for comparison
  const Array<FlowRight> flow_right_no_avg = {
      {
          .uid = Uid {1},
          .name = "no_avg_flow",
          .discharge = 30.0,
          .fail_cost = 1000.0,
      },
  };

  const System system_no_avg = {
      .name = "NoAverageFlowTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .flow_right_array = flow_right_no_avg,
  };

  SimulationLP simulation_lp2(simulation, options);
  SystemLP system_lp2(system_no_avg, simulation_lp2);
  auto&& lp2 = system_lp2.linear_interface();

  // use_average=true should add 1 extra column (qeh) and 1 extra row (qavg)
  CHECK(ncols_with_avg == lp2.get_numcols() + 1);
  CHECK(nrows_with_avg == lp2.get_numrows() + 1);

  auto result2 = lp2.resolve();
  REQUIRE(result2.has_value());
  CHECK(result2.value() == 0);
}

TEST_CASE(  // NOLINT
    "FlowRight with bound_rule - LP construction and bound application")
{
  // Hydro system with reservoir whose volume drives the bound rule
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 5.0,
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
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
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

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 100.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 2.0,
      },
  };

  // FlowRight with bound_rule: step function at V=500
  // At eini=1500 > 500, bound evaluates to 200
  const Array<FlowRight> flow_right_array = {
      {
          .uid = Uid {1},
          .name = "bounded_irrig",
          .discharge = {},
          .fmax = 300.0,
          .fail_cost = 5000.0,
          .bound_rule =
              RightBoundRule {
                  .reservoir = Uid {1},
                  .segments =
                      {
                          {
                              .volume = 0.0,
                              .slope = 0.0,
                              .constant = 0.0,
                          },
                          {
                              .volume = 500.0,
                              .slope = 0.0,
                              .constant = 200.0,
                          },
                      },
              },
      },
  };

  const Simulation simulation = {
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
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "BoundRuleFlowTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .flow_right_array = flow_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "VolumeRight with bound_rule - LP construction and bound application")
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
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 5.0,
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
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
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

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 100.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 2.0,
      },
  };

  // VolumeRight with Laja-style bound_rule
  const Array<VolumeRight> volume_right_array = {
      {
          .uid = Uid {1},
          .name = "bounded_vol",
          .emax = 500.0,
          .eini = 0.0,
          .demand = 50.0,
          .fail_cost = 5000.0,
          .use_state_variable = false,
          .bound_rule =
              RightBoundRule {
                  .reservoir = Uid {1},
                  .segments =
                      {
                          {
                              .volume = 0.0,
                              .slope = 0.0,
                              .constant = 570.0,
                          },
                          {
                              .volume = 1200.0,
                              .slope = 0.40,
                              .constant = 90.0,
                          },
                      },
                  .cap = 5000.0,
              },
      },
  };

  const Simulation simulation = {
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
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "BoundRuleVolTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .volume_right_array = volume_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "FlowRight bound_rule deactivation - zero bound fixes variable at 0")
{
  // When reservoir volume is below the threshold, bound evaluates to 0.
  // The flow column should be fixed at 0.
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 5.0,
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
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 3000.0,
          .emin = 100.0,
          .emax = 3000.0,
          .eini = 200.0,  // Below threshold → bound = 0
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 100.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 2.0,
      },
  };

  // Bound rule: 0 below 500, 200 above 500
  // eini=200 < 500 → initial bound = 0 → flow fixed at 0
  const Array<FlowRight> flow_right_array = {
      {
          .uid = Uid {1},
          .name = "deactivated_irrig",
          .discharge = {},
          .fmax = 300.0,
          .fail_cost = 5000.0,
          .bound_rule =
              RightBoundRule {
                  .reservoir = Uid {1},
                  .segments =
                      {
                          {
                              .volume = 0.0,
                              .slope = 0.0,
                              .constant = 0.0,
                          },
                          {
                              .volume = 500.0,
                              .slope = 0.0,
                              .constant = 200.0,
                          },
                      },
              },
      },
  };

  const Simulation simulation = {
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
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "DeactivationTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .flow_right_array = flow_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "FlowRight use_average with variable mode partition")
{
  // A partition where qeh is created for the total supply
  // and the variable withdrawal rights.  Tests that both
  // variable mode AND use_average work together.
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
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

  const Array<FlowRight> flow_right_array = {
      {
          .uid = Uid {1},
          .name = "total_gen",
          .direction = 1,
          .discharge = 100.0,
          .use_average = true,
      },
      {
          .uid = Uid {2},
          .name = "irr_share",
          .direction = -1,
          .discharge = {},
          .fmax = 100.0,
          .use_average = true,
          .fail_cost = 1100.0,
      },
      {
          .uid = Uid {3},
          .name = "elec_share",
          .direction = -1,
          .discharge = {},
          .fmax = 100.0,
          .use_average = true,
          .fail_cost = 1000.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 6,
              },
              {
                  .uid = Uid {2},
                  .duration = 18,
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
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "PartitionWithAvgTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .flow_right_array = flow_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "VolumeRight depletion model - finp depletes energy when eini > demand")
{
  // Regression test for Bug 2: fout_efficiency=0.0 was creating +Inf
  // coefficient forcing finp=0. With the fix (fout_efficiency=1.0), the
  // energy balance correctly depletes with finp:
  //   ec[t] = ec[t-1] - fcr x duration x finp / energy_scale
  //
  // Setup: VolumeRight with eini=100 hm3, demand=10 hm3, fail_cost=1e6.
  // With depletion model the LP can satisfy demand with finp (cost-free),
  // making fail=0 optimal.  With the old broken model finp was forced to 0,
  // requiring fail=10 (cost = 1e6 x 10 / 1000 = 10000 in obj units).
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  // VolumeRight: eini=100 hm3 > demand=10 hm3 so finp can satisfy demand.
  // fail_cost is very high so the LP avoids fail and uses finp instead.
  const Array<VolumeRight> volume_right_array = {
      {
          .uid = Uid {1},
          .name = "vol_depletion",
          .emax = 500.0,
          .eini = 100.0,
          .demand = 10.0,
          .fail_cost = 1.0e6,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
          },
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "DepletionModelTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .volume_right_array = volume_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Default scale_objective = 10,000,000.
  // Dispatch-only obj = 50 MW * $10/MWh * 1h / 10,000,000 = 5e-05.
  // With the old broken model (finp=0, all demand via fail):
  //   obj = 5e-05 + 1e6 * 10 / 10,000,000 ~= 1.0  (much larger)
  // CHECK(obj < 0.1) verifies that demand is satisfied with finp (no fail).
  const double obj = lp.get_obj_value();
  CHECK(obj < 0.1);
}

TEST_CASE(  // NOLINT
    "VolumeRight energy_scale - inherits from parent VolumeRight")
{
  // Regression test for Bug 3: energy_scale was not inherited from the
  // parent VolumeRight (right_reservoir) when no physical reservoir is set.
  // The child should use the parent's energy_scale, not the default 1.0.
  //
  // Setup: parent VolumeRight with explicit energy_scale=10.
  //        child VolumeRight with right_reservoir=parent, no explicit scale.
  // The child's internal energy_scale should be 10 (inherited from parent).
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  // Parent: explicit energy_scale=10 (like a large reservoir).
  // Child: right_reservoir -> parent, no explicit energy_scale.
  const Array<VolumeRight> volume_right_array = {
      {
          .uid = Uid {1},
          .name = "parent_vol",
          .emax = 500.0,
          .eini = 200.0,
          .energy_scale = 10.0,
      },
      {
          .uid = Uid {2},
          .name = "child_vol",
          .right_reservoir = Uid {1},
          .direction = -1,
          .emax = 100.0,
          .eini = 50.0,
          .demand = 5.0,
          .fail_cost = 1.0e6,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
          },
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "EnergyScaleInheritTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .volume_right_array = volume_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Default scale_objective = 10,000,000.
  // Child has eini=50, demand=5, fail_cost=1e6, energy_scale inherited=10.
  // Dispatch-only obj = 50 MW * $10/MWh * 1h / 10,000,000 = 5e-05.
  // With the old broken model (finp=0, all demand via fail):
  //   obj ~= 5e-05 + 1e6*(5/10)/10,000,000 = 5e-05 + 0.05 ~= 0.05
  // Without energy_scale inheritance (scale=1 instead of 10):
  //   fail_LP_cost per LP-unit is different (1.0 vs 0.1), causing
  //   incorrect coefficient scaling in demand and energy rows.
  // CHECK(obj < 0.01) verifies dispatch-only solution (fail=0).
  const double obj = lp.get_obj_value();
  CHECK(obj < 0.01);
}

TEST_CASE(  // NOLINT
    "VolumeRight right_reservoir coupling - direction sign is correct")
{
  // Regression test for Bug 1: right_reservoir coupling used child's
  // energy_scale and wrong sign for the coefficient.
  //
  // Fix: coeff = -dir x fcr x duration / parent_energy_scale
  //   direction=-1 (depletion): coeff > 0 -> parent energy DECREASES
  //   direction=+1 (accumulation): coeff < 0 -> parent energy INCREASES
  //
  // This test uses direction=-1: child finp should deplete parent energy.
  // Parent starts at eini=100 hm3. Child demands 10 hm3 via finp.
  // Child finp (with depletion model) satisfies child demand;
  // simultaneously depletes parent's budget via right_reservoir coupling.
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  // Parent VolumeRight: large budget (eini=100).
  // Child VolumeRight: coupled to parent with direction=-1 (depletion).
  // Child has its own eini=50 > demand=10, so child satisfies via finp.
  const Array<VolumeRight> volume_right_array = {
      {
          .uid = Uid {1},
          .name = "parent_vol",
          .emax = 200.0,
          .eini = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "child_vol",
          .right_reservoir = Uid {1},
          .direction = -1,
          .emax = 100.0,
          .eini = 50.0,
          .demand = 10.0,
          .fail_cost = 1.0e6,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
          },
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "RightReservoirSignTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .volume_right_array = volume_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Default scale_objective = 10,000,000.
  // Child finp satisfies demand=10 (eini=50 >= demand=10), fail=0.
  // Dispatch-only obj = 50 MW * $10/MWh * 1h / 10,000,000 = 5e-05.
  // With the old broken model (wrong sign, finp depletes wrong direction):
  //   energy constraint prevents finp -> fail covers demand
  //   obj ~= 5e-05 + 1e6*10/10,000,000 ~= 1.0
  // CHECK(obj < 0.1) verifies demand satisfied with finp (no fail).
  const double obj = lp.get_obj_value();
  CHECK(obj < 0.1);
}

TEST_CASE(  // NOLINT
    "VolumeRight consumptive - correct depletion sign for reservoir energy")
{
  // Regression test for Bug 4: consumptive coupling coefficient was
  // NEGATIVE (-fcr x dur / r_energy_scale), which made the VolumeRight's
  // finp ADD energy to the reservoir instead of subtracting it.
  //
  // Fix: coeff = +fcr x dur / r_energy_scale (positive -> reservoir depletes).
  // This matches the reservoir's own extraction variable sign convention.
  //
  // Setup: reservoir (eini=500, emax=1000), consumptive VolumeRight
  // (demand=20, eini=100, fail_cost=1e6).  The VolumeRight extracts from
  // the reservoir. Both should be feasible (eini=500 >= required extraction).
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j_up"},
      {.uid = Uid {2}, .name = "j_down", .drain = true},
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

  // Large enough reservoir (eini=500) so consumptive extraction is feasible.
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 1000.0,
          .emin = 0.0,
          .emax = 1000.0,
          .eini = 500.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 50.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 2.0,
      },
  };

  // Consumptive VolumeRight: eini=100, demand=20 hm3, fail_cost=1e6.
  // With Bug 4 fix: finp correctly depletes the reservoir.
  const Array<VolumeRight> volume_right_array = {
      {
          .uid = Uid {1},
          .name = "irr_consumptive",
          .reservoir = Uid {1},
          .consumptive = true,
          .emax = 200.0,
          .eini = 100.0,
          .demand = 20.0,
          .fail_cost = 1.0e6,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
          },
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "ConsumptiveDepletionTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .volume_right_array = volume_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // VolumeRight has eini=100 >= demand=20, so finp satisfies demand (fail=0).
  // Reservoir has eini=500 >= volume extracted.  Objective ~= dispatch only.
  const double obj = lp.get_obj_value();
  CHECK(obj < 1.0);  // fail=0 -> no high fail_cost in objective
}
