/**
 * @file      test_ieee4b_ori_planning.cpp
 * @brief     Unit and solution-correctness tests for IEEE 4-bus original case
 * @date      2026-02-22
 * @copyright BSD-3-Clause
 *
 * Tests for the IEEE 4-bus original base case.  The system has:
 *   - 4 buses, 2 simple thermal generators (no solar profile), 2 demand buses,
 *     and 5 transmission lines.
 *   - A single 1-hour block (1 stage, 1 scenario).
 *   - Loads: 150 MW at b3, 100 MW at b4 (total 250 MW).
 *   - G1 at b1 (pmax=300, gcost=20 $/MWh), G2 at b2 (pmax=200, gcost=35 $/MWh).
 *   - No generator_profile_array (no solar/renewable profiles).
 *   - All demand is fully served (status=0, no load shedding).
 *   - Optimal dispatch: G1 produces 250 MW (cheaper), G2 is idle.
 *   - Objective value = 250 × 20 / scale_objective(1000) = 5.0.
 *   - Solution validated via e2e integration test comparing output CSVs.
 */

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_lp.hpp>

using namespace gtopt;

// clang-format off
static constexpr std::string_view ieee4b_ori_json = R"({
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
    "name": "ieee_4b_ori",
    "bus_array": [
      {"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"},
      {"uid": 3, "name": "b3"}, {"uid": 4, "name": "b4"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 20, "capacity": 300},
      {"uid": 2, "name": "g2", "bus": "b2", "pmin": 0, "pmax": 200, "gcost": 35, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d3", "bus": "b3", "lmax": [[150.0]]},
      {"uid": 2, "name": "d4", "bus": "b4", "lmax": [[100.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2", "reactance": 0.02, "tmax_ab": 300, "tmax_ba": 300},
      {"uid": 2, "name": "l1_3", "bus_a": "b1", "bus_b": "b3", "reactance": 0.02, "tmax_ab": 300, "tmax_ba": 300},
      {"uid": 3, "name": "l2_3", "bus_a": "b2", "bus_b": "b3", "reactance": 0.03, "tmax_ab": 200, "tmax_ba": 200},
      {"uid": 4, "name": "l2_4", "bus_a": "b2", "bus_b": "b4", "reactance": 0.02, "tmax_ab": 200, "tmax_ba": 200},
      {"uid": 5, "name": "l3_4", "bus_a": "b3", "bus_b": "b4", "reactance": 0.03, "tmax_ab": 150, "tmax_ba": 150}
    ]
  }
})";
// clang-format on

TEST_CASE("IEEE 4-bus original - JSON parse and structure check")
{
  auto planning = daw::json::from_json<Planning>(ieee4b_ori_json);

  CHECK(planning.system.name == "ieee_4b_ori");
  CHECK(planning.system.bus_array.size() == 4);
  CHECK(planning.system.generator_array.size() == 2);
  CHECK(planning.system.demand_array.size() == 2);
  CHECK(planning.system.line_array.size() == 5);
  CHECK(planning.system.generator_profile_array.empty());

  // Single block, 1 stage, 1 scenario
  CHECK(planning.simulation.block_array.size() == 1);
  CHECK(planning.simulation.stage_array.size() == 1);
  CHECK(planning.simulation.scenario_array.size() == 1);
}

TEST_CASE("IEEE 4-bus original - LP solve")
{
  // Use Planning::merge so scene_array keeps its default {Scene{}} –
  // the same pattern gtopt_main uses when reading JSON files.
  Planning base;
  base.merge(daw::json::from_json<Planning>(ieee4b_ori_json));

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
  CHECK(result.value() == 1);  // 1 scene successfully processed
}

TEST_CASE("IEEE 4-bus original - solution correctness")
{
  // Known analytical solution:
  //   G1 (gcost=20) is cheaper → dispatches all 250 MW of demand.
  //   G2 (gcost=35) is idle.
  //   No load shedding.
  //   Objective = 250 × 20 / scale_objective(1000) = 5.0.
  Planning base;
  base.merge(daw::json::from_json<Planning>(ieee4b_ori_json));

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 1);  // 1 scene processed

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& lp_interface = systems.front().front().linear_interface();

  // Objective value: 250 MW × $20/MWh ÷ 1000 (scale_objective) = 5.0
  CHECK(lp_interface.get_obj_value() == doctest::Approx(5.0));
}
