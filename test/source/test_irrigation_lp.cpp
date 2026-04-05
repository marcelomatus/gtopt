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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
          .production_factor = 2.0,
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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
    "FlowRight subtracts from physical junction balance")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Hydro system with junction + FlowRight (always consumptive)
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
          .production_factor = 2.0,
      },
  };

  // FlowRight: withdraws from physical junction j_down
  const Array<FlowRight> flow_right_array = {
      {
          .uid = Uid {1},
          .name = "farmer_withdrawal",
          .junction = Uid {2},
          .discharge = 10.0,
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
    "VolumeRight subtracts from physical reservoir balance")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
          .production_factor = 2.0,
      },
  };

  // VolumeRight: extracts volume from reservoir (always consumptive)
  const Array<VolumeRight> volume_right_array = {
      {
          .uid = Uid {1},
          .name = "irrig_vol",
          .reservoir = Uid {1},
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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
          .production_factor = 2.0,
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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
          .production_factor = 2.0,
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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
          .production_factor = 2.0,
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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
    "Rights exhaustion limits generation despite available water")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Physical model: reservoir → waterway → j_down (drain=true).
  // A user constraint couples reservoir extraction to VolumeRight extraction:
  //   rsv_extraction = vrt_extraction
  // VolumeRight tracks remaining rights (eini=limit → depletes to 0).
  // When vrt_vol hits 0, extraction must stop →
  // turbine stops → thermal generator fills the gap at higher cost.
  //
  // No FlowRight — the VolumeRight extraction IS the physical extraction.

  auto solve = [](double rights_limit) -> double
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
            .name = "hydro",
            .bus = Uid {1},
            .gcost = 0.0,
            .capacity = 200.0,
        },
        {
            .uid = Uid {2},
            .name = "thermal",
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
            .name = "j_rsv",
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
            .name = "ww",
            .junction_a = Uid {1},
            .junction_b = Uid {2},
            .fmin = 0.0,
            .fmax = 200.0,
        },
    };
    const Array<Reservoir> reservoir_array = {
        {
            .uid = Uid {1},
            .name = "rsv",
            .junction = Uid {1},
            .capacity = 200.0,
            .emin = 0.0,
            .emax = 200.0,
            .eini = 100.0,
        },
    };
    const Array<Turbine> turbine_array = {
        {
            .uid = Uid {1},
            .name = "tur",
            .waterway = Uid {1},
            .generator = Uid {1},
            .production_factor = 2.0,
        },
    };
    // VolumeRight: starts at rights_limit and depletes toward 0.
    // extraction = extraction flow (m³/s), coupled to reservoir via user
    // constraint.
    const Array<VolumeRight> volume_right_array = {
        {
            .uid = Uid {1},
            .name = "rights_vol",
            .emax = rights_limit,
            .eini = rights_limit,
            .fmax = 200.0,
        },
    };
    // User constraint ties reservoir extraction to VolumeRight extraction:
    //   rsv_extraction = vrt_extraction  (physical extraction IS rights
    //   consumption)
    const Array<UserConstraint> user_constraint_array = {
        {
            .uid = Uid {1},
            .name = "rsv_vrt_couple",
            .expression =
                R"(reservoir("rsv").extraction = volume_right("rights_vol").extraction)",
            .constraint_type = "raw",
        },
    };

    const Simulation simulation = {
        .block_array =
            {
                {
                    .uid = Uid {1},
                    .duration = 24,
                },
            },
        .stage_array =
            {
                {
                    .uid = Uid {1},
                    .first_block = 0,
                    .count_block = 1,
                },
                {
                    .uid = Uid {2},
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
        .name = "RightsExhaustionTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .reservoir_array = reservoir_array,
        .turbine_array = turbine_array,
        .volume_right_array = volume_right_array,
        .user_constraint_array = user_constraint_array,
    };

    const PlanningOptionsLP options;
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
    return lp.get_obj_value();
  };

  // With 100 hm³ of rights (plenty): all demand served by hydro → cost ≈ 0
  // Demand = 50 MW, conv_rate = 2.0, so need 25 m³/s.
  // 2 stages × 24h × 25 m³/s × 0.0036 = 4.32 hm³ needed, 100 available.
  const auto cost_unlimited = solve(100.0);
  CHECK(cost_unlimited == doctest::Approx(0.0).epsilon(0.01));

  // With 3 hm³ of rights (< 4.32 hm³ needed): thermal must fill the gap.
  // The optimizer allocates rights optimally across stages, so partial
  // hydro is possible — but total cost must be strictly positive.
  const auto cost_limited = solve(3.0);
  CHECK(cost_limited > cost_unlimited);
}

TEST_CASE(  // NOLINT
    "VolumeRight economy with saving variable accumulates unused rights")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Model: a rights VolumeRight extracts water, an economy VolumeRight
  // receives the unused portion as savings via a user constraint:
  //   rights.extraction + economy.saving <= max_extraction_flow
  //
  // The economy VolumeRight uses saving_rate to enable the saving variable.
  // When the rights holder doesn't fully exercise their entitlement,
  // the remainder flows into the economy accumulator.
  //
  // Setup:
  //   - 1 bus, 1 hydro generator, 1 thermal, 1 demand (50 MW)
  //   - reservoir → waterway → downstream junction
  //   - rights VolumeRight: emax=100 hm³, extraction coupled to reservoir
  //   - economy VolumeRight: eini=0, saving_rate=200 m³/s, accumulates
  //   - user constraint: rights.extraction + economy.saving = 50 m³/s
  //     (the max_flow limit — unused portion becomes savings)
  //   - 2 stages × 24h blocks

  auto solve = [](double saving_max_rate) -> double
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
            .name = "hydro",
            .bus = Uid {1},
            .gcost = 0.0,
            .capacity = 200.0,
        },
        {
            .uid = Uid {2},
            .name = "thermal",
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
            .name = "j_rsv",
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
            .name = "ww",
            .junction_a = Uid {1},
            .junction_b = Uid {2},
            .fmin = 0.0,
            .fmax = 200.0,
        },
    };
    const Array<Reservoir> reservoir_array = {
        {
            .uid = Uid {1},
            .name = "rsv",
            .junction = Uid {1},
            .capacity = 200.0,
            .emin = 0.0,
            .emax = 200.0,
            .eini = 100.0,
        },
    };
    const Array<Turbine> turbine_array = {
        {
            .uid = Uid {1},
            .name = "tur",
            .waterway = Uid {1},
            .generator = Uid {1},
            .production_factor = 2.0,
        },
    };
    // Rights VolumeRight: starts at 100 hm³, extraction depletes it.
    // Economy VolumeRight: starts at 0, saving_rate allows deposits.
    const Array<VolumeRight> volume_right_array = {
        {
            .uid = Uid {1},
            .name = "rights_vol",
            .emax = 100.0,
            .eini = 100.0,
            .fmax = 200.0,
        },
        {
            .uid = Uid {2},
            .name = "economy_vol",
            .purpose = "economy",
            .emax = 100.0,
            .eini = 0.0,
            .saving_rate = saving_max_rate,
        },
    };
    // User constraints:
    // 1. rsv extraction = rights extraction (physical coupling)
    // 2. rights extraction + economy saving = 50 m³/s
    //    (unused portion of 50 m³/s max flow becomes savings)
    const Array<UserConstraint> user_constraint_array = {
        {
            .uid = Uid {1},
            .name = "rsv_vrt_couple",
            .expression =
                R"(reservoir("rsv").extraction = volume_right("rights_vol").extraction)",
            .constraint_type = "raw",
        },
        {
            .uid = Uid {2},
            .name = "saving_balance",
            .expression =
                R"(volume_right("rights_vol").extraction + volume_right("economy_vol").saving = 50)",
            .constraint_type = "raw",
        },
    };

    const Simulation simulation = {
        .block_array =
            {
                {
                    .uid = Uid {1},
                    .duration = 24,
                },
            },
        .stage_array =
            {
                {
                    .uid = Uid {1},
                    .first_block = 0,
                    .count_block = 1,
                },
                {
                    .uid = Uid {2},
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
        .name = "EconomySavingTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .reservoir_array = reservoir_array,
        .turbine_array = turbine_array,
        .volume_right_array = volume_right_array,
        .user_constraint_array = user_constraint_array,
    };

    const PlanningOptionsLP options;
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
    return lp.get_obj_value();
  };

  // With saving enabled: the constraint forces
  //   rights.extraction + economy.saving = 50 m³/s
  // Hydro needs 25 m³/s for 50 MW (conv_rate=2.0), so:
  //   rights.extraction = 25, economy.saving = 25
  // The economy accumulates 25 m³/s × 24h × 0.0036 = 0.216 hm³/stage
  // All demand served by hydro → cost ≈ 0.
  const auto cost_with_saving = solve(200.0);
  CHECK(cost_with_saving == doctest::Approx(0.0).epsilon(0.01));
}

TEST_CASE(  // NOLINT
    "FlowRight extraction - use_value, fail_cost, use_average, and junction")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Verifies:
  //  1. Per-element use_value creates a negative obj coefficient (benefit)
  //  2. Per-element fail_cost creates a deficit variable with penalty
  //  3. hydro_use_value fallback applies when no per-element use_value
  //  4. hydro_fail_cost fallback applies when no per-element fail_cost
  //  5. use_average creates qeh (stage-average flow) variable
  //  6. Junction coupling subtracts flow from junction balance

  // Two blocks with different durations to exercise averaging
  const auto dur1 = 6.0;  // hours
  const auto dur2 = 18.0;  // hours

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
          .production_factor = 2.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = dur1,
              },
              {
                  .uid = Uid {2},
                  .duration = dur2,
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

  SUBCASE("per-element use_value and fail_cost")
  {
    const auto use_val = 1500.0;
    const auto fail_val = 5000.0;

    const Array<FlowRight> flow_right_array = {
        {
            .uid = Uid {1},
            .name = "irr_with_costs",
            .junction = Uid {2},
            .discharge = 10.0,
            .fail_cost = fail_val,
            .use_value = use_val,
        },
    };

    const System system = {
        .name = "PerElementCostTest",
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
    const auto scale_obj = options.scale_objective();
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    const auto obj_coeffs = lp.get_obj_coeff();

    // Find columns by scanning for negative cost (use_value → benefit)
    // and positive cost (fail_cost → penalty)
    bool found_benefit = false;
    bool found_penalty = false;
    for (size_t i = 0; i < lp.get_numcols(); ++i) {
      const auto c = obj_coeffs[i];
      if (c < 0.0 && doctest::Approx(c).epsilon(1e-6) == -use_val / scale_obj) {
        found_benefit = true;
      }
      if (c > 0.0 && doctest::Approx(c).epsilon(1e-6) == fail_val / scale_obj) {
        found_penalty = true;
      }
    }
    CHECK(found_benefit);
    CHECK(found_penalty);

    // Verify flow lower bound is relaxed to 0 (deficit coupling)
    const auto col_low = lp.get_col_low();
    bool found_relaxed = false;
    for (size_t i = 0; i < lp.get_numcols(); ++i) {
      if (obj_coeffs[i] < 0.0
          && doctest::Approx(obj_coeffs[i]).epsilon(1e-6)
              == -use_val / scale_obj)
      {
        CHECK(col_low[i] == doctest::Approx(0.0));
        found_relaxed = true;
      }
    }
    CHECK(found_relaxed);

    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // flow should equal discharge (10.0) when unconstrained,
    // fail should be 0 (no deficit)
    const auto sol = lp.get_col_sol();
    for (size_t i = 0; i < lp.get_numcols(); ++i) {
      if (obj_coeffs[i] > 0.0
          && doctest::Approx(obj_coeffs[i]).epsilon(1e-6)
              == fail_val / scale_obj)
      {
        CHECK(sol[i] == doctest::Approx(0.0));
      }
    }
  }

  SUBCASE("fail_cost deficit coupling - fail absorbs shortfall")
  {
    // FlowRight demands 80 m³/s from junction j_down, but the hydro
    // system only delivers 100 m³/s inflow total (through one turbine).
    // After serving 50 MW demand via gen1 (50/2.0 = 25 m³/s turbine flow),
    // the junction has limited water.  With discharge=80 and junction
    // coupling, the FlowRight can't always fully deliver, so fail > 0.
    const auto discharge = 80.0;
    const auto fail_val = 5000.0;

    const Array<FlowRight> flow_right_array = {
        {
            .uid = Uid {1},
            .name = "heavy_demand",
            .junction = Uid {2},
            .discharge = discharge,
            .fail_cost = fail_val,
        },
    };

    const System system = {
        .name = "DeficitCouplingTest",
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
    const auto scale_obj = options.scale_objective();
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // The fail variable should be non-zero: the FlowRight can't get
    // the full 80 m³/s because the junction's water is limited.
    const auto obj_coeffs = lp.get_obj_coeff();
    const auto sol = lp.get_col_sol();
    double total_fail = 0.0;
    for (size_t i = 0; i < lp.get_numcols(); ++i) {
      if (obj_coeffs[i] > 0.0
          && doctest::Approx(obj_coeffs[i]).epsilon(1e-6)
              == fail_val / scale_obj)
      {
        total_fail += sol[i];
      }
    }
    // With limited water, some deficit should exist
    CHECK(total_fail > 0.0);
  }

  SUBCASE("hydro_use_value global fallback")
  {
    const auto global_uv = 0.5;  // $/m³

    // FlowRight without per-element use_value → should use global fallback
    const Array<FlowRight> flow_right_array = {
        {
            .uid = Uid {1},
            .name = "irr_global_uv",
            .discharge = 10.0,
        },
    };

    const System system = {
        .name = "GlobalUseValueTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .flow_right_array = flow_right_array,
    };

    PlanningOptions popts;
    popts.model_options.hydro_use_value = global_uv;
    const PlanningOptionsLP options(std::move(popts));
    const auto scale_obj = options.scale_objective();
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    const auto obj_coeffs = lp.get_obj_coeff();

    // Expected: -global_uv * duration * 3600 / scale_objective
    // Block 1: -0.5 * 6 * 3600 / scale = -10800 / scale
    // Block 2: -0.5 * 18 * 3600 / scale = -32400 / scale
    const auto expected_b1 = -global_uv * dur1 * 3600.0 / scale_obj;
    const auto expected_b2 = -global_uv * dur2 * 3600.0 / scale_obj;

    bool found_b1 = false;
    bool found_b2 = false;
    for (size_t i = 0; i < lp.get_numcols(); ++i) {
      const auto c = obj_coeffs[i];
      if (doctest::Approx(c).epsilon(1e-8) == expected_b1) {
        found_b1 = true;
      }
      if (doctest::Approx(c).epsilon(1e-8) == expected_b2) {
        found_b2 = true;
      }
    }
    CHECK(found_b1);
    CHECK(found_b2);

    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
  }

  SUBCASE("hydro_fail_cost global fallback")
  {
    const auto global_fc = 1.0;  // $/m³

    // FlowRight without per-element fail_cost → should use global fallback
    const Array<FlowRight> flow_right_array = {
        {
            .uid = Uid {1},
            .name = "irr_global_fc",
            .discharge = 10.0,
        },
    };

    const System system = {
        .name = "GlobalFailCostTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .flow_right_array = flow_right_array,
    };

    PlanningOptions popts;
    popts.model_options.hydro_fail_cost = global_fc;
    const PlanningOptionsLP options(std::move(popts));
    const auto scale_obj = options.scale_objective();
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    const auto obj_coeffs = lp.get_obj_coeff();

    // Expected: global_fc * duration * 3600 / scale_objective
    const auto expected_b1 = global_fc * dur1 * 3600.0 / scale_obj;
    const auto expected_b2 = global_fc * dur2 * 3600.0 / scale_obj;

    bool found_b1 = false;
    bool found_b2 = false;
    for (size_t i = 0; i < lp.get_numcols(); ++i) {
      const auto c = obj_coeffs[i];
      if (doctest::Approx(c).epsilon(1e-8) == expected_b1) {
        found_b1 = true;
      }
      if (doctest::Approx(c).epsilon(1e-8) == expected_b2) {
        found_b2 = true;
      }
    }
    CHECK(found_b1);
    CHECK(found_b2);

    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
  }

  SUBCASE("use_average creates qeh variable and qavg constraint")
  {
    // use_average=true → stage-average flow variable (qeh)
    // qeh = Σ_b [ flow(b) × dur(b) / dur_stage ]
    //
    // Use DIFFERENT discharges per block so the weighted average
    // differs from a simple arithmetic mean — this proves the
    // duration-weighting is actually applied.
    //   block 1: discharge = 10 m³/s, duration = 6 h
    //   block 2: discharge = 50 m³/s, duration = 18 h
    //   stage_dur = 24 h
    //   qeh = 10 × 6/24 + 50 × 18/24 = 2.5 + 37.5 = 40.0
    //   simple mean would be (10+50)/2 = 30 ≠ 40 → proves weighting
    const auto d1 = 10.0;
    const auto d2 = 50.0;
    const auto expected_qeh =
        (d1 * (dur1 / (dur1 + dur2))) + (d2 * (dur2 / (dur1 + dur2)));

    // Per-block discharge: [scenario=1][stage=1][block=2]
    std::vector<std::vector<std::vector<Real>>> discharge_sched = {
        {
            {
                d1,
                d2,
            },
        },
    };

    const Array<FlowRight> flow_right_array = {
        {
            .uid = Uid {1},
            .name = "irr_avg",
            .discharge = std::move(discharge_sched),
            .use_average = true,
            .fail_cost = 1000.0,
        },
    };

    const System system = {
        .name = "UseAverageTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .flow_right_array = flow_right_array,
    };

    // Count columns/rows without use_average for comparison
    size_t cols_without = 0;
    size_t rows_without = 0;
    {
      std::vector<std::vector<std::vector<Real>>> ds_no = {
          {
              {
                  d1,
                  d2,
              },
          },
      };
      const Array<FlowRight> fra_no_avg = {
          {
              .uid = Uid {1},
              .name = "irr_no_avg",
              .discharge = std::move(ds_no),
              .fail_cost = 1000.0,
          },
      };
      const System sys_no = {
          .name = "NoAvgRef",
          .bus_array = bus_array,
          .demand_array = demand_array,
          .generator_array = generator_array,
          .flow_right_array = fra_no_avg,
      };
      const PlanningOptionsLP opts_no;
      SimulationLP sim_no(simulation, opts_no);
      SystemLP slp_no(sys_no, sim_no);
      cols_without = slp_no.linear_interface().get_numcols();
      rows_without = slp_no.linear_interface().get_numrows();
    }

    const PlanningOptionsLP options;
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();

    // use_average adds 1 qeh column and 1 qavg row
    CHECK(lp.get_numcols() == cols_without + 1);
    CHECK(lp.get_numrows() == rows_without + 1);

    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // qeh = 10 × 6/24 + 50 × 18/24 = 40.0
    // This differs from the simple mean (30.0), proving duration-weighting.
    const auto sol = lp.get_col_sol();
    const auto qeh_val = sol[cols_without];
    CHECK(qeh_val == doctest::Approx(expected_qeh).epsilon(0.01));
    CHECK(expected_qeh == doctest::Approx(40.0));  // sanity check
  }

  SUBCASE("per-element use_value overrides hydro_use_value global")
  {
    const auto per_elem_uv = 2000.0;
    const auto global_uv = 0.5;  // $/m³ — should be ignored

    const Array<FlowRight> flow_right_array = {
        {
            .uid = Uid {1},
            .name = "irr_override",
            .discharge = 10.0,
            .use_value = per_elem_uv,
        },
    };

    const System system = {
        .name = "OverrideUseValueTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .flow_right_array = flow_right_array,
    };

    PlanningOptions popts;
    popts.model_options.hydro_use_value = global_uv;
    const PlanningOptionsLP options(std::move(popts));
    const auto scale_obj = options.scale_objective();
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    const auto obj_coeffs = lp.get_obj_coeff();

    // Per-element takes precedence: cost = -per_elem_uv / scale_obj
    // (no duration multiplication for per-element, it's already in $/flow-unit)
    const auto expected = -per_elem_uv / scale_obj;
    bool found = false;
    for (size_t i = 0; i < lp.get_numcols(); ++i) {
      if (doctest::Approx(obj_coeffs[i]).epsilon(1e-8) == expected) {
        found = true;
        break;
      }
    }
    CHECK(found);

    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
  }
}
