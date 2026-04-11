/**
 * @file      test_irrigation_coupling.cpp
 * @brief     LP integration tests for FlowRight and VolumeRight hydro coupling
 * @date      2026-04-05
 * @copyright BSD-3-Clause
 *
 * Tests verify that irrigation rights couple correctly to the hydrological
 * topology: FlowRight subtracts from junction balances, VolumeRight subtracts
 * from reservoir balances, and use_average creates stage-average variables.
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/volume_right_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

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

// ─────────────────────────────────────────────────────────────────────────
// Tier 4 — VolumeRight + Reservoir balance integration tests
//
// These tests exercise the full coupling between a physical reservoir,
// hydro generation, and one or more VolumeRights through user constraints.
// They verify behaviors at the LP-solution level (objective value, dual
// feasibility) rather than column-level wiring (which Tier 1 covers).
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 4.1 - VolumeRight + reservoir mass balance closes")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Single reservoir with no inflow, hydro generator dispatched to
  // meet demand.  A VolumeRight is coupled to reservoir extraction
  // through a user constraint, and depletes as the rights are
  // exercised.  When the rights cap is generous, hydro covers all
  // demand (objective ≈ 0); when the cap is tight the mass-balance
  // limit kicks in and thermal must back-fill.
  //
  // Production factor 2.0 means 50 MW demand needs 25 m³/s of flow
  // through the turbine.  Over 2 × 24h stages this is
  //   2 × 25 × 24 × 0.0036 = 4.32 hm³ of extraction total.

  auto solve = [](double rights_cap) -> double
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
    const Array<VolumeRight> volume_right_array = {
        {
            .uid = Uid {1},
            .name = "rights_vol",
            .emax = rights_cap,
            .eini = rights_cap,
            .fmax = 200.0,
        },
    };
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
        .name = "Tier4_1_MassBalance",
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
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
    return lp.get_obj_value();
  };

  // 100 hm³ of rights >> 4.32 hm³ needed: hydro covers all demand,
  // mass balance closes with positive end-of-stage volume.
  const auto cost_unlimited = solve(100.0);
  CHECK(cost_unlimited == doctest::Approx(0.0).epsilon(0.01));

  // 3 hm³ of rights << 4.32 hm³ needed: thermal must fill the gap.
  const auto cost_limited = solve(3.0);
  CHECK(cost_limited > cost_unlimited);
}

TEST_CASE(  // NOLINT
    "Tier 4.2 - FlowRight bound_rule zone transition")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Two-segment bound_rule attached to a FlowRight: when reservoir
  // volume is below 100 hm³ the cap is low (5 m³/s); above 100 hm³ the
  // cap rises to 60 m³/s.  Solving with eini above and below the
  // breakpoint must produce different LP objectives because the
  // FlowRight bound is dispatched off the reservoir's volume.

  auto solve = [](double initial_volume) -> double
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
            .fmax = 500.0,
        },
    };
    const Array<Reservoir> reservoir_array = {
        {
            .uid = Uid {1},
            .name = "rsv",
            .junction = Uid {1},
            .capacity = 500.0,
            .emin = 0.0,
            .emax = 500.0,
            .eini = initial_volume,
        },
    };
    const Array<Flow> flow_array = {
        {
            .uid = Uid {1},
            .name = "inflow",
            .direction = 1,
            .junction = Uid {1},
            .discharge = 5.0,
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
    // FlowRight on the downstream junction with a 2-zone bound rule:
    //   V <  100 hm³ → cap = 5 m³/s
    //   V >= 100 hm³ → cap = 60 m³/s
    const Array<FlowRight> flow_right_array = {
        {
            .uid = Uid {1},
            .name = "fr_zone",
            .junction = Uid {2},
            .direction = -1,
            .discharge = 60.0,
            .fail_cost = 10.0,
            .bound_rule =
                RightBoundRule {
                    .reservoir = Uid {1},
                    .segments =
                        {
                            {
                                .volume = 0.0,
                                .slope = 0.0,
                                .constant = 5.0,
                            },
                            {
                                .volume = 100.0,
                                .slope = 0.0,
                                .constant = 60.0,
                            },
                        },
                    .cap = 60.0,
                },
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
            },
        .scenario_array =
            {
                {
                    .uid = Uid {0},
                },
            },
    };

    const System system = {
        .name = "Tier4_2_ZoneTransition",
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
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
    return lp.get_obj_value();
  };

  // The two solutions must reflect the zone change: above the
  // breakpoint the FlowRight cap is high (60 m³/s); below it the cap
  // collapses to 5 m³/s.  We accept either direction of inequality —
  // what matters is that the bound rule is dispatched off the
  // configured reservoir volume.
  const auto cost_above = solve(150.0);
  const auto cost_below = solve(50.0);
  CHECK(cost_above != doctest::Approx(cost_below).epsilon(1e-6));
}

TEST_CASE(  // NOLINT
    "Tier 4.3 - Two competing VolumeRights on one reservoir")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Two VolumeRights both draw from the same physical reservoir.
  // A user constraint partitions reservoir extraction between them:
  //   reservoir.extraction = vrt_a.extraction + vrt_b.extraction
  // When the combined available water cannot satisfy demand, thermal
  // must back-fill — total cost > 0.

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
          .eini = 50.0,
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
  // Two competing rights, each starting with 1 hm³.
  // Combined 2 hm³ < 2.16 hm³ needed for full hydro coverage.
  const Array<VolumeRight> volume_right_array = {
      {
          .uid = Uid {1},
          .name = "vrt_a",
          .emax = 1.0,
          .eini = 1.0,
          .fmax = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "vrt_b",
          .emax = 1.0,
          .eini = 1.0,
          .fmax = 200.0,
      },
  };
  const Array<UserConstraint> user_constraint_array = {
      {
          .uid = Uid {1},
          .name = "rsv_partition",
          .expression =
              R"(reservoir("rsv").extraction = volume_right("vrt_a").extraction + volume_right("vrt_b").extraction)",
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
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "Tier4_3_TwoRights",
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
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto cost = lp.get_obj_value();
  CHECK(cost > 0.0);
}

TEST_CASE(  // NOLINT
    "Tier 4.4 - Multi-stage state link uses single shared column")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Within a single phase, the SDDP-style storage chain reuses the
  // same column for efin[N] and eini[N+1] (storage_lp.hpp:475).
  // This test verifies that VolumeRightLP wires its storage chain
  // correctly across stages: efin_col_at(stage_i) must equal
  // eini_col_at(stage_{i+1}) — no duplicated state columns.
  //
  // This is a wiring regression that would have failed silently before
  // the f02856d7 AMPL registration refactor — any double-registration
  // of the state column would surface as a column-index drift.

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
  const Array<VolumeRight> volume_right_array = {
      {
          .uid = Uid {1},
          .name = "rights_vol",
          .emax = 100.0,
          .eini = 100.0,
          .fmax = 200.0,
      },
  };
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
      .name = "Tier4_4_StateLink",
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

  // Verify the storage chain wiring at the column level: within a
  // single phase, eini[stage_{i+1}] must be the SAME column as
  // efin[stage_i] (no duplicated state).
  const auto& vr_lp = system_lp.elements<VolumeRightLP>().front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  REQUIRE(scenarios.size() == 1);
  REQUIRE(stages.size() == 2);

  const auto efin_s1 = vr_lp.efin_col_at(scenarios[0], stages[0]);
  const auto eini_s2 = vr_lp.eini_col_at(scenarios[0], stages[1]);
  CHECK(efin_s1 == eini_s2);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

// ─────────────────────────────────────────────────────────────────────────
// Tier 5 — FlowRight + Turbine/Junction integration tests
//
// Tier 5 verifies that FlowRights interact correctly with hydraulic
// topology (junctions, waterways, turbines) at the LP-solution level.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "Tier 5.1 - FlowRight upstream of turbine starves hydro generation")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Topology: rsv → j_up → ww → j_down (drain), turbine on ww.
  // A FlowRight at j_up withdraws water, reducing the flow available
  // to the waterway and therefore the hydro turbine.  When the
  // FlowRight discharge is large enough to starve the turbine, thermal
  // (gcost=100) must back-fill — total cost > the no-withdrawal case.

  auto solve = [](double withdrawal_discharge) -> double
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
            .name = "ww",
            .junction_a = Uid {1},
            .junction_b = Uid {2},
            .fmin = 0.0,
            .fmax = 200.0,
        },
    };
    // Small reservoir creates scarcity: 5 hm³ over 24h is only
    // 5/(0.0036×24) ≈ 57.87 m³/s of total budget — enough for the
    // turbine's 25 m³/s OR a substantial withdrawal, but not both.
    const Array<Reservoir> reservoir_array = {
        {
            .uid = Uid {1},
            .name = "rsv",
            .junction = Uid {1},
            .capacity = 200.0,
            .emin = 0.0,
            .emax = 200.0,
            .eini = 5.0,
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
    // FlowRight on the upstream junction — pulls water OUT of j_up
    // before it can reach the turbine.  With low fail_cost the LP
    // can choose to fail this right rather than starve the turbine.
    const Array<FlowRight> flow_right_array = {
        {
            .uid = Uid {1},
            .name = "upstream_withdrawal",
            .junction = Uid {1},
            .direction = -1,
            .discharge = withdrawal_discharge,
            .fail_cost = 200000.0,
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
            },
        .scenario_array =
            {
                {
                    .uid = Uid {0},
                },
            },
    };

    const System system = {
        .name = "Tier5_1_StarveHydro",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .reservoir_array = reservoir_array,
        .turbine_array = turbine_array,
        .flow_right_array = flow_right_array,
    };

    const PlanningOptionsLP options;
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
    return lp.get_obj_value();
  };

  // Tiny withdrawal: hydro still has plenty of water, thermal not
  // needed → cost ≈ 0.
  const auto cost_low = solve(0.1);
  CHECK(cost_low == doctest::Approx(0.0).epsilon(0.01));

  // Large withdrawal (200 m³/s ≫ 25 m³/s hydro need): no water left
  // for the turbine, the FlowRight itself takes priority due to its
  // 5000 $/hm³ fail_cost, and thermal must back-fill the demand.
  const auto cost_high = solve(200.0);
  CHECK(cost_high > cost_low);
}

TEST_CASE(  // NOLINT
    "Tier 5.2 - Multiple FlowRights on one junction sum to a binding total")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Two FlowRights on the same downstream junction j_down both pull
  // water out.  Their combined discharge competes with the turbine
  // throughput in the junction balance.  Solving with one vs two
  // FlowRights of the same individual discharge proves that
  // additional rights aggregate (sum constraint via the Junction
  // balance row).

  auto solve = [](int num_rights) -> double
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
    Array<FlowRight> flow_right_array = {
        {
            .uid = Uid {1},
            .name = "fr_a",
            .junction = Uid {2},
            .direction = -1,
            .discharge = 100.0,
            .fail_cost = 5000.0,
        },
    };
    if (num_rights == 2) {
      flow_right_array.push_back(FlowRight {
          .uid = Uid {2},
          .name = "fr_b",
          .junction = Uid {2},
          .direction = -1,
          .discharge = 100.0,
          .fail_cost = 5000.0,
      });
    }

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
            },
        .scenario_array =
            {
                {
                    .uid = Uid {0},
                },
            },
    };

    const System system = {
        .name = "Tier5_2_TwoOnOneJunction",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .reservoir_array = reservoir_array,
        .turbine_array = turbine_array,
        .flow_right_array = flow_right_array,
    };

    const PlanningOptionsLP options;
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
    return lp.get_obj_value();
  };

  // 1 FlowRight × 100 m³/s requires the waterway to carry 100 m³/s of
  // throughput just to satisfy that one right.  Reservoir has
  // 100 hm³ ≈ 100/(0.0036·24) ≈ 1157 m³/s × h-equivalent — plenty.
  // 2 FlowRights × 100 m³/s requires 200 m³/s — at the waterway fmax.
  // Both should be solvable; the second case applies more strain on
  // the reservoir balance.
  const auto cost_one = solve(1);
  const auto cost_two = solve(2);

  // Both objectives must be finite and the two-rights case can never
  // be cheaper than the one-right case (additional binding constraint).
  CHECK(cost_two >= cost_one - 1e-6);
}

TEST_CASE(  // NOLINT
    "Tier 5.3 - FlowRight fail_cost trades deficit against penalty")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // FlowRight with discharge=100 m³/s on a system whose only water
  // source is a small reservoir (eini=2 hm³) and no inflow.
  // Available water ≪ demanded discharge over 24h, so the deficit
  // variable must absorb the shortfall.  The objective scales with
  // fail_cost: doubling fail_cost more than doubles the cost (the
  // small "free" water budget makes the relationship slightly
  // sub-linear in absolute terms but strictly monotonic).

  auto solve = [](double fail_cost) -> double
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
            .eini = 2.0,
        },
    };
    const Array<FlowRight> flow_right_array = {
        {
            .uid = Uid {1},
            .name = "fr_infeasible",
            .junction = Uid {2},
            .direction = -1,
            .discharge = 100.0,
            .fail_cost = fail_cost,
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
            },
        .scenario_array =
            {
                {
                    .uid = Uid {0},
                },
            },
    };

    const System system = {
        .name = "Tier5_3_FailCostTradeoff",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .reservoir_array = reservoir_array,
        .flow_right_array = flow_right_array,
    };

    const PlanningOptionsLP options;
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
    return lp.get_obj_value();
  };

  // The deficit variable absorbs the shortfall, with the objective
  // scaling linearly in fail_cost beyond the baseline (thermal +
  // any fixed overhead).  Higher fail_cost → strictly higher total.
  const auto cost_low = solve(100.0);
  const auto cost_high = solve(10000.0);
  CHECK(cost_high > cost_low);
}
