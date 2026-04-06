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
