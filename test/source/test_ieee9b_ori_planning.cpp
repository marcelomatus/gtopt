/**
 * @file      test_ieee9b_ori_planning.cpp
 * @brief     Integration test for IEEE 9-bus original base case
 * @date      2026-02-22
 * @copyright BSD-3-Clause
 *
 * Integration test for the IEEE 9-bus original base case.  The system has:
 *   - 9 buses, 3 simple thermal generators (no solar profile), 3 demand buses,
 *     and 9 transmission lines.
 *   - A single 1-hour block (1 stage, 1 scenario).
 *   - Original IEEE 9-bus loads: 125 MW at b5, 100 MW at b7, 90 MW at b9.
 *   - All generators are thermal with costs 20, 35, 30 $/MWh respectively.
 *   - No generator_profile_array (no solar/renewable profiles).
 */

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_lp.hpp>

using namespace gtopt;

// clang-format off
static constexpr std::string_view ieee9b_ori_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": true,
    "output_format": "csv",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "use_kirchhoff": true
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "ieee_9b_ori",
    "bus_array": [
      {"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"}, {"uid": 3, "name": "b3"},
      {"uid": 4, "name": "b4"}, {"uid": 5, "name": "b5"}, {"uid": 6, "name": "b6"},
      {"uid": 7, "name": "b7"}, {"uid": 8, "name": "b8"}, {"uid": 9, "name": "b9"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 10, "pmax": 250, "gcost": 20, "capacity": 250},
      {"uid": 2, "name": "g2", "bus": "b2", "pmin": 10, "pmax": 300, "gcost": 35, "capacity": 300},
      {"uid": 3, "name": "g3", "bus": "b3", "pmin": 0,  "pmax": 270, "gcost": 30, "capacity": 270}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b5", "lmax": [[125.0]]},
      {"uid": 2, "name": "d2", "bus": "b7", "lmax": [[100.0]]},
      {"uid": 3, "name": "d3", "bus": "b9", "lmax":  [[90.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l1_4", "bus_a": "b1", "bus_b": "b4", "reactance": 0.0576,  "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 2, "name": "l2_7", "bus_a": "b2", "bus_b": "b7", "reactance": 0.0625,  "tmax_ab": 300, "tmax_ba": 300},
      {"uid": 3, "name": "l3_9", "bus_a": "b3", "bus_b": "b9", "reactance": 0.0586,  "tmax_ab": 270, "tmax_ba": 270},
      {"uid": 4, "name": "l4_5", "bus_a": "b4", "bus_b": "b5", "reactance": 0.085,   "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 5, "name": "l4_6", "bus_a": "b4", "bus_b": "b6", "reactance": 0.092,   "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 6, "name": "l5_7", "bus_a": "b5", "bus_b": "b7", "reactance": 0.161,   "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 7, "name": "l6_9", "bus_a": "b6", "bus_b": "b9", "reactance": 0.17,    "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 8, "name": "l7_8", "bus_a": "b7", "bus_b": "b8", "reactance": 0.072,   "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 9, "name": "l8_9", "bus_a": "b8", "bus_b": "b9", "reactance": 0.1008,  "tmax_ab": 250, "tmax_ba": 250}
    ]
  }
})";
// clang-format on

TEST_CASE("IEEE 9-bus original - JSON parse and structure check")
{
  auto planning = daw::json::from_json<Planning>(ieee9b_ori_json);

  CHECK(planning.system.name == "ieee_9b_ori");
  CHECK(planning.system.bus_array.size() == 9);
  CHECK(planning.system.generator_array.size() == 3);
  CHECK(planning.system.demand_array.size() == 3);
  CHECK(planning.system.line_array.size() == 9);
  CHECK(planning.system.generator_profile_array.empty());

  // Single block, 1 stage, 1 scenario
  CHECK(planning.simulation.block_array.size() == 1);
  CHECK(planning.simulation.stage_array.size() == 1);
  CHECK(planning.simulation.scenario_array.size() == 1);
}

TEST_CASE("IEEE 9-bus original - LP solve")
{
  auto planning = daw::json::from_json<Planning>(ieee9b_ori_json);

  PlanningLP planning_lp(planning);
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}
