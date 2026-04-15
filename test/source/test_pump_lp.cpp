/**
 * @file      test_pump_lp.cpp
 * @brief     Unit tests for PumpLP covering conversion, capacity, and
 *            pumped-storage round-trip economics
 * @date      2026-04-13
 * @copyright BSD-3-Clause
 *
 * Tests cover:
 *  - Basic pump conversion constraint (pump_factor × flow − load ≤ 0)
 *  - Pump with capacity constraint limiting flow
 *  - Two-reservoir pumped-storage: turbine + pump with round-trip
 *    efficiency < 1 ensures no simultaneous gen/pump in LP
 *  - Pump with custom efficiency
 */

#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// -----------------------------------------------------------------------
// Basic pump: waterway + demand, verify LP builds and solves
// -----------------------------------------------------------------------

TEST_CASE("PumpLP — basic conversion constraint builds and solves")  // NOLINT
{
  // Single reservoir draining through a waterway.  The pump pushes water
  // from the drain junction back upstream.  A cheap thermal generator
  // provides electrical power for the pump load.

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load_d1",
          .bus = Uid {1},
          .fcost = 1000.0,
          .capacity = 50.0,
      },
      {
          .uid = Uid {2},
          .name = "pump_demand",
          .bus = Uid {1},
          .capacity = 200.0,
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
          .name = "ww_pump",
          .junction_a = Uid {2},
          .junction_b = Uid {1},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv_up",
          .junction = Uid {1},
          .capacity = 5000.0,
          .emin = 0.0,
          .emax = 5000.0,
          .eini = 2000.0,
      },
      {
          .uid = Uid {2},
          .name = "rsv_down",
          .junction = Uid {2},
          .capacity = 5000.0,
          .emin = 0.0,
          .emax = 5000.0,
          .eini = 3000.0,
      },
  };

  // Pump with pump_factor=2.0 MW/(m³/s)
  const Array<Pump> pump_array = {
      {
          .uid = Uid {1},
          .name = "pump1",
          .waterway = Uid {1},
          .demand = Uid {2},
          .pump_factor = 2.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
              },
              {
                  .uid = Uid {2},
                  .duration = 2,
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
      .name = "BasicPumpTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .pump_array = pump_array,
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

// -----------------------------------------------------------------------
// Pump with capacity constraint
// -----------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "PumpLP — capacity constraint limits pump flow")
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
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load_d1",
          .bus = Uid {1},
          .fcost = 1000.0,
          .capacity = 50.0,
      },
      {
          .uid = Uid {2},
          .name = "pump_demand",
          .bus = Uid {1},
          .capacity = 500.0,
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
          .name = "ww_pump",
          .junction_a = Uid {2},
          .junction_b = Uid {1},
          .fmin = 0.0,
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv_up",
          .junction = Uid {1},
          .capacity = 10000.0,
          .emin = 0.0,
          .emax = 10000.0,
          .eini = 3000.0,
      },
      {
          .uid = Uid {2},
          .name = "rsv_down",
          .junction = Uid {2},
          .capacity = 10000.0,
          .emin = 0.0,
          .emax = 10000.0,
          .eini = 5000.0,
      },
  };

  // Pump with capacity=25 m³/s (limits flow despite ww_pump fmax=500)
  const Array<Pump> pump_array = {
      {
          .uid = Uid {1},
          .name = "pump_capped",
          .waterway = Uid {1},
          .demand = Uid {2},
          .pump_factor = 2.0,
          .capacity = 25.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
              },
              {
                  .uid = Uid {2},
                  .duration = 2,
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
      .name = "PumpCapacityTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .pump_array = pump_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

// -----------------------------------------------------------------------
// Pumped-storage round-trip: turbine + pump between two reservoirs
// -----------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "PumpLP — two-reservoir pumped storage with turbine and pump")
{
  // Simplified Colbún-Machicura model:
  //
  //   Colbún (upper)  ──ww_gen──▶  Machicura (lower)
  //       junction j_up              junction j_down (drain)
  //       reservoir rsv_up           reservoir rsv_down
  //
  //   Machicura (lower) ──ww_pump──▶ Colbún (upper)
  //
  //   Turbine on ww_gen:  production_factor=1.44 MW/(m³/s)
  //   Pump on ww_pump:    pump_factor=1.88 MW/(m³/s), efficiency=0.85
  //
  //   Round-trip efficiency: (1.44 / 1.88) × 0.85 ≈ 0.65 < 1.0
  //   → LP will never run turbine + pump simultaneously
  //
  //   Block 1 (off-peak, 4 h): low demand, cheap thermal → pump fills upper
  //   Block 2 (peak, 2 h): high demand → turbine generates from upper

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  // Hydro generator (turbine output) and thermal backup
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
          .capacity = 300.0,
      },
  };

  // System load + pump electrical demand
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "system_load",
          .bus = Uid {1},
          .fcost = 1000.0,
          .capacity = 150.0,
      },
      {
          .uid = Uid {2},
          .name = "pump_load",
          .bus = Uid {1},
          .capacity = 200.0,
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

  // Generation waterway: upper → lower
  // Pump waterway: lower → upper
  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww_gen",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "ww_pump",
          .junction_a = Uid {2},
          .junction_b = Uid {1},
          .fmin = 0.0,
          .fmax = 60.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv_colbun",
          .junction = Uid {1},
          .capacity = 10000.0,
          .emin = 1000.0,
          .emax = 10000.0,
          .eini = 5000.0,
      },
      {
          .uid = Uid {2},
          .name = "rsv_machicura",
          .junction = Uid {2},
          .capacity = 3000.0,
          .emin = 500.0,
          .emax = 3000.0,
          .eini = 1500.0,
      },
  };

  // Inflow to upper reservoir
  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "natural_inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 20.0,
      },
  };

  // Turbine: generation direction (upper → lower)
  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur_gen",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.44,
      },
  };

  // Pump: pumping direction (lower → upper)
  const Array<Pump> pump_array = {
      {
          .uid = Uid {1},
          .name = "pump_hb",
          .waterway = Uid {2},
          .demand = Uid {2},
          .pump_factor = 1.88,
          .efficiency = 0.85,
          .capacity = 40.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 4,
              },
              {
                  .uid = Uid {2},
                  .duration = 2,
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
      .name = "PumpedStorageTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .pump_array = pump_array,
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

  // Objective must be positive (demand × cost × duration)
  CHECK(lp.get_obj_value() > 0.0);
}

// -----------------------------------------------------------------------
// Pump with custom efficiency
// -----------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "PumpLP — efficiency modifies effective conversion rate")
{
  // Two identical pumps except efficiency: one at 1.0 (default), one at 0.5.
  // The lower-efficiency pump needs twice the electrical power per m³/s.
  // With limited generator capacity, the system must choose the cheaper one.

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load_d1",
          .bus = Uid {1},
          .fcost = 1000.0,
          .capacity = 30.0,
      },
      {
          .uid = Uid {2},
          .name = "pump_eff_demand",
          .bus = Uid {1},
          .capacity = 500.0,
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
          .name = "ww_pump_eff",
          .junction_a = Uid {2},
          .junction_b = Uid {1},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv_up",
          .junction = Uid {1},
          .capacity = 5000.0,
          .emin = 0.0,
          .emax = 5000.0,
          .eini = 2000.0,
      },
      {
          .uid = Uid {2},
          .name = "rsv_down",
          .junction = Uid {2},
          .capacity = 5000.0,
          .emin = 0.0,
          .emax = 5000.0,
          .eini = 3000.0,
      },
  };

  // Pump with efficiency=0.5: effective rate = 2.0/0.5 = 4.0 MW/(m³/s)
  const Array<Pump> pump_array = {
      {
          .uid = Uid {1},
          .name = "pump_low_eff",
          .waterway = Uid {1},
          .demand = Uid {2},
          .pump_factor = 2.0,
          .efficiency = 0.5,
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
      .name = "PumpEfficiencyTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .pump_array = pump_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  CHECK(lp.get_obj_value() >= 0.0);
}

// -----------------------------------------------------------------------
// PumpLP::add_to_output via write_out
// -----------------------------------------------------------------------

TEST_CASE("PumpLP — add_to_output via write_out")  // NOLINT
{
  // Exercises PumpLP::add_to_output (conversion_rows + capacity_rows dual)
  // by calling system_lp.write_out() after the solve.

  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load_d1",
          .bus = Uid {1},
          .fcost = 1000.0,
          .capacity = 50.0,
      },
      {
          .uid = Uid {2},
          .name = "pump_demand",
          .bus = Uid {1},
          .capacity = 200.0,
      },
  };

  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j_up"},
      {.uid = Uid {2}, .name = "j_down", .drain = true},
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww_pump",
          .junction_a = Uid {2},
          .junction_b = Uid {1},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv_up",
          .junction = Uid {1},
          .capacity = 5000.0,
          .emin = 0.0,
          .emax = 5000.0,
          .eini = 2000.0,
      },
      {
          .uid = Uid {2},
          .name = "rsv_down",
          .junction = Uid {2},
          .capacity = 5000.0,
          .emin = 0.0,
          .emax = 5000.0,
          .eini = 3000.0,
      },
  };

  // Pump with capacity constraint so capacity_rows are also populated
  const Array<Pump> pump_array = {
      {
          .uid = Uid {1},
          .name = "pump1",
          .waterway = Uid {1},
          .demand = Uid {2},
          .pump_factor = 2.0,
          .capacity = 80.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
          },
      .stage_array =
          {
              {.uid = Uid {1}, .first_block = 0, .count_block = 1},
          },
      .scenario_array =
          {
              {.uid = Uid {0}},
          },
  };

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_pump_out";
  std::filesystem::create_directories(tmpdir);

  const System system = {
      .name = "PumpOutputTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .pump_array = pump_array,
  };

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Exercises PumpLP::add_to_output
  CHECK_NOTHROW(system_lp.write_out());

  std::filesystem::remove_all(tmpdir);
}
