/**
 * @file      test_simulation.cpp
 * @brief     Tests for the Simulation class
 * @date      Sat Apr 19 12:30:00 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

TEST_CASE("Simulation - Constructor initialization")
{
  using Uid = gtopt::Uid;

  // Create arrays for system components
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0}};
  const Array<Generator> generator_array = {{.uid = Uid {1},
                                             .name = "g1",
                                             .bus = Uid {1},
                                             .gcost = 50.0,
                                             .capacity = 100.0}};

  // Create minimal system
  const Options options = {};

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system {.name = "TestSys",
                       .bus_array = bus_array,
                       .demand_array = demand_array,
                       .generator_array = generator_array,
                       .line_array = {}};

  // Test constructor
  const OptionsLP options_lp(options);
  SimulationLP simulation_lp(simulation, options_lp);

  SystemLP system_lp(system, simulation_lp);
  const SystemContext system_context(simulation_lp, system_lp);
  const InputContext ic(system_context);

  CHECK(ic.system_context().system().elements<BusLP>().size()
        == system.bus_array.size());
}

TEST_CASE("Simulation - Basic LP run without solving")
{
  using namespace gtopt;
  using Uid = gtopt::Uid;

  // Create arrays for system components
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0}};
  const Array<Generator> generator_array = {{.uid = Uid {1},
                                             .name = "g1",
                                             .bus = Uid {1},
                                             .gcost = 50.0,
                                             .capacity = 100.0}};

  const Options options = {};

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system {.name = "TestSys",
                       .bus_array = bus_array,
                       .demand_array = demand_array,
                       .generator_array = generator_array,
                       .line_array = {}};

  // Test constructor
  const OptionsLP options_lp(options);
  SimulationLP simulation_lp(simulation, options_lp);

  SystemLP system_lp(system, simulation_lp);
  const SystemContext system_context(simulation_lp, system_lp);
  const InputContext ic(system_context);

  // Create minimal system

  // Test just creating the LP without solving
  // simulation_lp.create_lp(system_lp);
}

#ifdef NONE

TEST_CASE("Simulation - Full LP run with solving")
{
  using namespace gtopt;
  using Uid = gtopt::Uid;

  // Create arrays for system components
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0}};
  const Array<Generator> generator_array = {{.uid = Uid {1},
                                             .name = "g1",
                                             .bus = Uid {1},
                                             .gcost = 50.0,
                                             .capacity = 100.0}};

  // Create minimal system
  System system {
      .name = "TestSys",
      .options = {},
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = {}};

  // Test creating and solving the LP
  auto result = Simulation::resolve(system,
                                    std::nullopt,  // no LP file
                                    1,  // use names
                                    0.0,  // no matrix filtering
                                    false);  // solve the problem

  // Check result is successful
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);
}

TEST_CASE("Simulation - Run with LP file output")
{
  using namespace gtopt;
  using Uid = gtopt::Uid;

  // Create arrays for system components
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0}};
  const Array<Generator> generator_array = {{.uid = Uid {1},
                                             .name = "g1",
                                             .bus = Uid {1},
                                             .gcost = 50.0,
                                             .capacity = 100.0}};

  // Create minimal system
  System system {
      .name = "TestSys",
      .options = {},
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = {}};

  // Generate a temporary file path
  std::string temp_file = "test_lp_output";

  // Test creating LP model and writing to file
  auto result = Simulation::resolve(system,
                                    temp_file,  // write LP to this file
                                    2,  // use names with maps
                                    0.0,  // no matrix filtering
                                    true);  // just create, don't solve

  // Check result is successful
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  // Check if the file was created
  // Note: we don't check file contents as that's implementation-dependent
  // and might change, but we can at least verify file creation
  // (optionally cleanup after test if desired)
  std::string lp_file = temp_file + ".lp";
  bool file_exists = std::filesystem::exists(lp_file);
  if (file_exists) {
    std::filesystem::remove(lp_file);
  }
  REQUIRE(file_exists);
}

TEST_CASE("Simulation - create_lp method")
{
  using namespace gtopt;
  using Uid = gtopt::Uid;

  // Create arrays for system components
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0}};
  const Array<Generator> generator_array = {{.uid = Uid {1},
                                             .name = "g1",
                                             .bus = Uid {1},
                                             .gcost = 50.0,
                                             .capacity = 100.0}};

  // Create phase and scene arrays with proper structure
  const Array<Phase> phase_array = {
      {.uid = Uid {1}, .first_stage = 0, .count_stage = 1},
      {.uid = Uid {2}, .first_stage = 0, .count_stage = 1}};

  const Array<Scene> scene_array = {
      {.uid = Uid {1}, .first_scenario = 0, .count_scenario = 2}};

  // Create test system with multiple stages/scenarios
  System system {
      .name = "MultiStageTest",
      .options = {},
      .block_array = {{.uid = Uid {1}, .duration = 1},
                      {.uid = Uid {2}, .duration = 2}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1},
                      {.uid = Uid {2}, .first_block = 1, .count_block = 1}},
      .scenario_array = {{.uid = Uid {1}}, {.uid = Uid {2}}},
      .phase_array = phase_array,
      .scene_array = scene_array,
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = {}};

  // Create simulation instance
  Simulation simulation(system);

  // Call the create_lp method - should create matrix
  // This mainly tests that no exceptions are thrown
  simulation.create_lp();
}

#endif
