#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_planning.hpp>

using namespace gtopt;

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

  if (plan.options.input_directory) {
    CHECK(plan.options.input_directory.value() == "data");
  }
  if (plan.options.use_kirchhoff) {
    CHECK(plan.options.use_kirchhoff.value() == true);
  }

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
      generator_array[i] = {
          .uid = uid,
          .name = "gen",
          .bus = bus,
          .pmin = 0.0,
          .pmax = 300.0,
          .gcost = 50.0,
          .capacity = 300.0,
      };
      ++uid;
    }
  }

  const Planning planning {
      .options = Options {.input_directory = "large_test"},
      .simulation = Simulation(),
      .system =
          System {
              .name = "large_system",
              .bus_array = bus_array,
              .generator_array = generator_array,
          },
  };

  auto json_data = daw::json::to_json(planning);
  gtopt::Planning parsed_plan =
      daw::json::from_json<gtopt::Planning>(json_data);

  if (parsed_plan.options.input_directory) {
    REQUIRE(parsed_plan.options.input_directory.value() == "large_test");
  }
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
    if (parsed_plan.system.generator_array[i].pmax) {
      REQUIRE(std::get<double>(
                  parsed_plan.system.generator_array[i].pmax.value_or(-1.0))
              == doctest::Approx(300.0));
    }
    ++uid;
  }
}

static constexpr std::string_view planning_json = R"({
  "options": {
    "annual_discount_rate": 0.1,
    "use_lp_names": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 1},
      {"uid": 2, "duration": 2}
    ],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1},
      {"uid": 2, "first_block": 1, "count_block": 1}
    ],
    "scenario_array": [
      {"uid": 1, "probability_factor": 1}
    ]
  },
  "system": {
    "name": "json_test_system",
    "bus_array": [
      {"uid": 1, "name": "b1"},
      {"uid": 2, "name": "b2"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": 1, "gcost": 50, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 2, "capacity": 80}
    ],
    "line_array": [
      {
        "uid": 1, "name": "l1",
        "bus_a": 1, "bus_b": 2,
        "reactance": 0.1,
        "tmax_ba": 200, "tmax_ab": 200,
        "capacity": 200
      }
    ],
    "battery_array": [
      {
        "uid": 1, "name": "bat1",
        "input_efficiency": 0.9,
        "output_efficiency": 0.9,
        "vmin": 0, "vmax": 50,
        "capacity": 50
      }
    ],
    "converter_array": [
      {
        "uid": 1, "name": "conv1",
        "battery": 1, "generator": 1, "demand": 1,
        "capacity": 100
      }
    ]
  }
})";

TEST_CASE("Planning JSON round-trip serialization")
{
  auto planning = daw::json::from_json<Planning>(planning_json);

  // Serialize back to JSON
  auto json_output = daw::json::to_json(planning);
  CHECK(!json_output.empty());

  // Parse the output back
  auto planning2 = daw::json::from_json<Planning>(json_output);
  CHECK(planning2.system.name == planning.system.name);
  CHECK(planning2.system.bus_array.size() == planning.system.bus_array.size());
  CHECK(planning2.system.generator_array.size()
        == planning.system.generator_array.size());
}
