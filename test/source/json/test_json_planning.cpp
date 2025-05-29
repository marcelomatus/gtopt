/**
 * @file      test_json_planning.cpp
 * @brief     Unit tests for JSON serialization/deserialization of Planning
 * class
 * @date      Wed May 28 12:00:00 2025
 * @author    System Tests
 * @copyright BSD-3-Clause
 *
 * This module tests JSON serialization and deserialization of the Planning
 * class including its components (Options, Simulation, System).
 */

#include <doctest/doctest.h>
#include <gtopt/json/json_planning.hpp>

TEST_CASE("Planning daw json test 1 - basic parsing")
{
  const std::string_view json_data = R"({
    "options": {
      "input_directory": "data",
      "use_kirchhoff": true
    },
    "simulation": {
      "block_array": [{"uid": 1, "duration": 2}],
      "stage_array": [],
      "scenario_array": []
    },
    "system": {
      "name": "TEST",
      "bus_array": [{"uid": 5, "name": "BUS1"}],
      "generator_array": []
    }
  })";

  gtopt::Planning plan = daw::json::from_json<gtopt::Planning>(json_data);

  REQUIRE(plan.options.input_directory.value() == "data");
  REQUIRE(plan.options.use_kirchhoff.value() == true);

  REQUIRE(plan.simulation.block_array.size() == 1);
  REQUIRE(plan.simulation.block_array[0].uid == 1);
  REQUIRE(plan.simulation.block_array[0].duration == 2);
  REQUIRE(plan.simulation.stage_array.empty());
  REQUIRE(plan.simulation.scenario_array.empty());

  REQUIRE(plan.system.name == "TEST");
  REQUIRE(plan.system.bus_array.size() == 1);
  REQUIRE(plan.system.bus_array[0].uid == 5);
  REQUIRE(plan.system.bus_array[0].name == "BUS1");
  REQUIRE(plan.system.generator_array.empty());
}

TEST_CASE("Planning daw json test 2 - large scale")
{
  const size_t size = 1000;
  std::vector<gtopt::Bus> bus_array(size);
  std::vector<gtopt::Generator> generator_array(size);

  {
    gtopt::Uid uid = 0;
    for (size_t i = 0; i < size; ++i) {
      const gtopt::SingleId bus {uid};
      bus_array[i] = {.uid = uid, .name = "bus"};
      generator_array[i] = {.uid = uid,
                            .name = "gen",
                            .bus = bus,
                            .pmin = 0.0,
                            .pmax = 300.0,
                            .gcost = 50.0,
                            .capacity = 300.0};
      ++uid;
    }
  }

  const gtopt::Planning planning {
      .options = gtopt::Options {.input_directory = "large_test"},
      .simulation =
          gtopt::Simulation {
              .block_array = {}, .stage_array = {}, .scenario_array = {}},
      .system = gtopt::System {.name = "large_system",
                               .bus_array = bus_array,
                               .demand_array = {},
                               .generator_array = generator_array,
                               .line_array = {}}};

  auto json_data = daw::json::to_json(planning);
  gtopt::Planning parsed_plan =
      daw::json::from_json<gtopt::Planning>(json_data);

  REQUIRE(parsed_plan.options.input_directory.value() == "large_test");
  REQUIRE(parsed_plan.system.name == "large_system");
  REQUIRE(parsed_plan.system.bus_array.size() == size);
  REQUIRE(parsed_plan.system.generator_array.size() == size);

  // Verify bus and generator data
  gtopt::Uid uid = 0;
  for (size_t i = 0; i < size; ++i) {
    gtopt::SingleId bus {uid};
    REQUIRE(parsed_plan.system.bus_array[i].uid == uid);
    REQUIRE(parsed_plan.system.generator_array[i].uid == uid);
    REQUIRE(parsed_plan.system.generator_array[i].bus == bus);
    REQUIRE(std::get<double>(parsed_plan.system.generator_array[i].pmax.value())
            == doctest::Approx(300.0));
    ++uid;
  }
}
