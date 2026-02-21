/**
 * @file      test_planning_json.cpp
 * @brief     Tests for Planning JSON parsing and solve pipeline
 * @date      2026-02-18
 * @copyright BSD-3-Clause
 *
 * Tests JSON round-trip parsing of Planning objects including all
 * component types, and exercises the full planning LP solve pipeline.
 */

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_lp.hpp>

using namespace gtopt;

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

TEST_CASE("Planning JSON parse and solve")
{
  auto planning = daw::json::from_json<Planning>(planning_json);

  CHECK(planning.system.name == "json_test_system");
  CHECK(planning.system.bus_array.size() == 2);
  CHECK(planning.system.generator_array.size() == 1);
  CHECK(planning.system.demand_array.size() == 1);
  CHECK(planning.system.line_array.size() == 1);
  CHECK(planning.system.battery_array.size() == 1);
  CHECK(planning.system.converter_array.size() == 1);

  PlanningLP planning_lp(planning);
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
}

static constexpr std::string_view hydro_planning_json = R"({
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 1},
      {"uid": 2, "duration": 2}
    ],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 2}
    ],
    "scenario_array": [
      {"uid": 1}
    ]
  },
  "system": {
    "name": "hydro_json_test",
    "bus_array": [
      {"uid": 1, "name": "b1"}
    ],
    "generator_array": [
      {"uid": 1, "name": "hydro_gen", "bus": 1, "gcost": 5, "capacity": 200},
      {"uid": 2, "name": "thermal_gen", "bus": 1, "gcost": 100, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 1, "capacity": 50}
    ],
    "junction_array": [
      {"uid": 1, "name": "j_up"},
      {"uid": 2, "name": "j_down", "drain": true}
    ],
    "waterway_array": [
      {
        "uid": 1, "name": "ww1",
        "junction_a": 1, "junction_b": 2,
        "fmin": 0, "fmax": 500
      }
    ],
    "flow_array": [
      {"uid": 1, "name": "inflow", "direction": 1, "junction": 1, "discharge": 20}
    ],
    "reservoir_array": [
      {
        "uid": 1, "name": "rsv1",
        "junction": 1,
        "capacity": 1000,
        "vmin": 0, "vmax": 1000,
        "vini": 500
      }
    ],
    "turbine_array": [
      {
        "uid": 1, "name": "tur1",
        "waterway": 1, "generator": 1,
        "conversion_rate": 1.0
      }
    ]
  }
})";

TEST_CASE("Planning JSON parse and solve - hydro system")
{
  auto planning = daw::json::from_json<Planning>(hydro_planning_json);

  CHECK(planning.system.name == "hydro_json_test");
  CHECK(planning.system.junction_array.size() == 2);
  CHECK(planning.system.waterway_array.size() == 1);
  CHECK(planning.system.flow_array.size() == 1);
  CHECK(planning.system.reservoir_array.size() == 1);
  CHECK(planning.system.turbine_array.size() == 1);

  PlanningLP planning_lp(planning);
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
}

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
