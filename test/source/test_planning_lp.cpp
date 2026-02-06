/**
 * @file      test_planning_lp.cpp
 * @brief     Unit tests for the PlanningLP class
 * @date      Sun May  5 10:42:00 2025
 * @author    Claude
 * @copyright BSD-3-Clause
 *
 * This module contains the unit tests for the PlanningLP class.
 */

#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/options_lp.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

TEST_CASE("PlanningLP - Default construction base")
{
  // Create minimal components
  const Options options {};
  const Simulation simulation {};
  const Planning planning {};

  // Test constructor
  const PlanningLP planning_lp(planning);

  // Verify construction was successful

  CHECK(planning_lp.planning().system.name == "");
  REQUIRE(planning_lp.planning().system.bus_array.size() == 0);
}

TEST_CASE("PlanningLP - Default construction")
{
  // Create minimal components
  const Options options {};
  const Simulation simulation {};

  // Create minimal system with one bus
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const System system {.name = "TestSystem", .bus_array = bus_array};

  // Create planning with components
  const Planning planning {
      .options = options,
      .simulation = simulation,
      .system = system,
  };

  // Convert options to flat options
  const FlatOptions flat_options;

  // Test constructor
  const PlanningLP planning_lp(planning, flat_options);

  // Verify construction was successful

  CHECK(planning_lp.planning().system.name == "TestSystem");
  REQUIRE(planning_lp.planning().system.bus_array.size() == 1);
  CHECK(planning_lp.planning().system.bus_array[0].name == "b1");
}

TEST_CASE("PlanningLP - Create simulations")
{
  // Create a simulation with blocks, stages, and scenarios
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  // Create minimal system with one bus, one generator, and one demand
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  const System system {
      .name = "TestSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  // Create planning with components
  const Planning planning {
      .options = {},  // NOLINT
      .simulation = simulation,
      .system = system,
  };

  // Create flat options
  const FlatOptions flat_options;

  // Create planning_lp
  const PlanningLP planning_lp(planning, flat_options);

  // Verify systems were created as expected (indirect test)
  // Further tests would depend on PlanningLP internal implementation
}

TEST_CASE("PlanningLP - Write LP file")
{
  // Create a simulation with blocks, stages, and scenarios
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  // Create minimal system with one bus, one generator, and one demand
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  const System system {
      .name = "TestSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  // Create planning with components
  const Planning planning {
      .options = {},  // NOLINT
      .simulation = simulation,
      .system = system,
  };

  // Create flat options for LP file writing
  const FlatOptions flat_options;

  // Create planning_lp
  const PlanningLP planning_lp(planning, flat_options);

  // Create systems first

  // Test writing LP file
  planning_lp.write_lp("test_planning");

  // Check if the file was created
  const std::string lp_file = "test_planning_0_0.lp";
  const bool file_exists = std::filesystem::exists(lp_file);

  // Clean up the file if it exists
  if (file_exists) {
    std::filesystem::remove(lp_file);
  }

  // Verify the file was created
  CHECK(file_exists);
}

TEST_CASE("PlanningLP - Run LP")
{
  // Create a simulation with blocks, stages, and scenarios
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  // Create minimal system with one bus, one generator, and one demand
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 80.0},
  };

  const System system {
      .name = "TestSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  // Create planning with components
  const Planning planning {
      .options = {},  // NOLINT
      .simulation = simulation,
      .system = system,
  };

  // Create flat options
  const FlatOptions flat_options;

  // Create planning_lp
  PlanningLP planning_lp(planning, flat_options);

  // Run the LP
  auto result = planning_lp.resolve();

  REQUIRE(result);
}

TEST_CASE("PlanningLP - Run with write_only flag")
{
  // Create a simulation with blocks, stages, and scenarios
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  // Create minimal system with one bus, one generator, and one demand
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 80.0},
  };

  const System system {
      .name = "TestSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  // Create planning with components
  const Planning planning {
      .options = {},  // NOLINT
      .simulation = simulation,
      .system = system,
  };

  // Create flat options
  const FlatOptions flat_options;

  // Create planning_lp
  PlanningLP planning_lp(planning, flat_options);

  planning_lp.write_lp("test_planning_lp_write_only");
  // Run the LP (should only create LP model, not solve)

  auto result = planning_lp.resolve();

  // Check that we got a successful result
  REQUIRE(result.has_value());

  // Check if the file was created
  const std::string lp_file = "test_planning_lp_write_only_0_0.lp";
  const bool file_exists = std::filesystem::exists(lp_file);

  // Clean up the file if it exists
  if (file_exists) {
    std::filesystem::remove(lp_file);
  }

  // Verify the file was created
  CHECK(file_exists);
}

TEST_CASE("PlanningLP - Error handling")
{
  // Setup test with invalid data that should cause a solver error
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  // Create system with conflicting constraints
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 200.0},
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };

  const System system = {
      .name = "TestSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  const Planning planning = {
      .options = {},  // NOLINT
      .simulation = simulation,
      .system = system,
  };

  PlanningLP planning_lp(planning);

  // Test error handling
  auto result = planning_lp.resolve();
  REQUIRE(!result);
  CHECK(result.error().code == ErrorCode::SolverError);
  CHECK(result.error().message.find("Failed to resolve") != std::string::npos);
}

TEST_CASE("PlanningLP - Solver test")
{
  using Uid = Uid;
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "b1", .bus = Uid {1}, .capacity = 100.0},
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 1000.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {3}, .duration = 1},
              {.uid = Uid {4}, .duration = 2},
              {.uid = Uid {5}, .duration = 3},
          },
      .stage_array =
          {
              {.uid = Uid {1}, .first_block = 0, .count_block = 1},
              {.uid = Uid {2}, .first_block = 1, .count_block = 2},
          },
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "SEN",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  REQUIRE(simulation.scenario_array.size() == 1);
  REQUIRE(simulation.stage_array.size() == 2);
  REQUIRE(simulation.block_array.size() == 3);
  REQUIRE(system.bus_array.size() == 1);
  REQUIRE(system.bus_array.size() == 1);
  REQUIRE(system.demand_array.size() == 1);
  REQUIRE(system.generator_array.size() == 1);
  REQUIRE(!system.line_array.empty() == false);

  // Create planning with components
  const Planning planning {
      .options = {},  // NOLINT
      .simulation = simulation,
      .system = system,
  };

  // Create planning_lp
  PlanningLP planning_lp(planning);

  // Run the LP - should result in an error
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());

  auto&& systems = planning_lp.systems();
  CHECK(systems.size() == 1);

  auto&& system_lp = systems.front().front();
  auto&& lp_interface = system_lp.linear_interface();

  const auto sol = lp_interface.get_col_sol();
  REQUIRE(sol[0] == doctest::Approx(100));  // demand
  REQUIRE(sol[1] == doctest::Approx(100));  // generation

  const auto dual = lp_interface.get_row_dual();
  REQUIRE(dual[0] * system_lp.options().scale_objective()
          == doctest::Approx(50));
}
