/**
 * @file      test_unified_battery_planning.hpp
 * @brief     LP solve tests for the unified battery definition
 * @date      2026-03-06
 * @copyright BSD-3-Clause
 *
 * Verifies that the unified battery definition (single Battery element
 * with `bus` field set) produces a valid LP solve.  The test uses a
 * single-bus 2-block system where:
 *   - A cheap generator (g1, $10/MWh, 200 MW) provides bulk power.
 *   - A unified battery (60 MW charge/discharge) is connected at the
 *     same bus.
 *   - Demand of 100 MW is served across both blocks.
 *   - The battery can charge from g1 in block 1 and discharge in block 2,
 *     but at $10/MWh and a 100 MW flat demand, the solver prefers direct
 *     supply from g1 (no round-trip loss).
 *
 * The expected behaviour is a feasible LP (status = 0, 1 scene processed).
 */

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_lp.hpp>

using namespace gtopt;

// clang-format off
static constexpr std::string_view unified_battery_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": true,
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 1},
      {"uid": 2, "duration": 1}
    ],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 2}
    ],
    "scenario_array": [
      {"uid": 1, "probability_factor": 1}
    ]
  },
  "system": {
    "name": "unified_battery_test",
    "bus_array": [
      {"uid": 1, "name": "b1"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": 1, "gcost": 10, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 1, "lmax": [[100, 100]]}
    ],
    "battery_array": [
      {
        "uid": 1, "name": "bess1",
        "bus": 1,
        "input_efficiency": 0.95,
        "output_efficiency": 0.95,
        "emin": 0, "emax": 200,
        "pmax_charge": 60,
        "pmax_discharge": 60,
        "gcost": 0,
        "capacity": 200
      }
    ]
  }
})";
// clang-format on

TEST_CASE("Unified battery JSON parse")  // NOLINT
{
  auto planning = daw::json::from_json<Planning>(unified_battery_json);

  CHECK(planning.system.name == "unified_battery_test");

  // Battery has the new bus field
  REQUIRE(planning.system.battery_array.size() == 1);
  const auto& bat = planning.system.battery_array[0];
  REQUIRE(bat.bus.has_value());
  CHECK(std::get<Uid>(*bat.bus) == Uid {1});

  // No converter_array in JSON — will be auto-generated
  CHECK(planning.system.converter_array.empty());

  // Only the user-defined generator and demand
  CHECK(planning.system.generator_array.size() == 1);
  CHECK(planning.system.demand_array.size() == 1);
}

TEST_CASE("Unified battery LP solve")  // NOLINT
{
  Planning base;
  base.merge(daw::json::from_json<Planning>(unified_battery_json));

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
  CHECK(result.value() == 1);  // 1 scene successfully processed
}

TEST_CASE("Unified battery solution correctness")  // NOLINT
{
  // With a single bus, 200 MW generator at $10, and 100 MW demand:
  //   G1 supplies 100 MW in each block directly.
  //   Battery is available but not needed (direct supply is cheaper
  //   than charging + round-trip loss + discharging).
  //   Objective = 100 MW × $10/MWh × 1 h × 2 blocks / 1000 = 2.0
  Planning base;
  base.merge(daw::json::from_json<Planning>(unified_battery_json));

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 1);

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& lp = systems.front().front().linear_interface();

  // Objective: 100 MW × $10/MWh × 1 h × 2 blocks ÷ 1000 = 2.0
  CHECK(lp.get_obj_value() == doctest::Approx(2.0));
}
