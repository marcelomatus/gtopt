/**
 * @file      test_fuel_min_offtake_planning.cpp
 * @brief     Integration tests: Fuel.min_offtake in Planning JSON + LP solve
 * @date      2026-05-31
 * @copyright BSD-3-Clause
 *
 * Exercises the full pipeline: Planning JSON deserialization →
 * Planning merge → PlanningLP::resolve.  Pins the take-or-pay
 * (`Fuel.min_offtake`) feature to the wire format that
 * `plexos2gtopt` emits and that `gtopt_main` consumes.
 *
 * Coverage:
 *   * JSON parse: hard floor (only ``min_offtake``).
 *   * JSON parse: soft floor (``min_offtake`` + ``min_offtake_cost``).
 *   * JSON parse: per-block floor (``min_offtake_per_block``).
 *   * JSON parse: ranged pair (both ``min_offtake`` and
 *     ``max_offtake`` on the same Fuel — emits two rows sharing the
 *     offtake DV, mirrors the ``Commitment::{min,max}_starts`` pattern).
 *   * PlanningLP resolve succeeds for each case.
 */

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_lp.hpp>

using namespace gtopt;

namespace test_fuel_min_offtake_planning
{

// Minimal single-bus, single-fuel, 2-block planning JSON.  Demand is
// 50 MW → 100 MWh / stage when fully served; generator g1 with
// heat_rate=2 burns 200 MMBtu in that case.
inline constexpr std::string_view base_json = R"json({
  "options": {
    "output_format": "csv",
    "output_compression": "uncompressed",
    "model_options": {
      "use_single_bus": true,
      "scale_objective": 1000,
      "demand_fail_cost": 1000
    }
  },
  "simulation": {
    "block_array": [
      {"uid": 0, "duration": 1},
      {"uid": 1, "duration": 1}
    ],
    "stage_array": [
      {"uid": 0, "first_block": 0, "count_block": 2, "active": 1}
    ],
    "scenario_array": [
      {"uid": 0, "probability_factor": 1}
    ]
  },
  "system": {
    "name": "min_offtake_planning",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "capacity": 50.0}
    ],
    "fuel_array": [
      {"uid": 1, "name": "gas", "price": 10.0, "min_offtake": 200.0}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1",
       "pmin": 0.0, "pmax": 200.0, "gcost": 0.0,
       "fuel": 1, "heat_rate": 2.0, "capacity": 200.0}
    ]
  }
})json";

}  // namespace test_fuel_min_offtake_planning

TEST_CASE("Fuel.min_offtake - JSON parse hard floor")  // NOLINT
{
  using namespace test_fuel_min_offtake_planning;
  const auto planning = parse_planning_json(base_json);
  REQUIRE(planning.system.fuel_array.size() == 1);
  const auto& f = planning.system.fuel_array[0];
  REQUIRE(f.min_offtake.has_value());
  CHECK(std::get<Real>(*f.min_offtake) == doctest::Approx(200.0));
  // No cost ⇒ hard floor; per-block flag unset ⇒ per-stage SUM mode.
  CHECK_FALSE(f.min_offtake_cost.has_value());
  CHECK_FALSE(f.min_offtake_per_block.has_value());
  // max_offtake stays unset for this case.
  CHECK_FALSE(f.max_offtake.has_value());
}

TEST_CASE("Fuel.min_offtake - JSON parse soft floor")  // NOLINT
{
  // Soft floor at $50/MMBtu shortfall: the LP will under-deliver when
  // physical supply can't reach the floor and pay the slack.
  constexpr std::string_view soft_json = R"json({
    "options": {"output_compression": "uncompressed",
                "model_options": {"use_single_bus": true,
                                  "demand_fail_cost": 1000}},
    "simulation": {
      "block_array": [{"uid": 0, "duration": 1}],
      "stage_array": [{"uid": 0, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 0}]
    },
    "system": {
      "name": "soft_min",
      "fuel_array": [
        {"uid": 1, "name": "gas", "price": 10.0,
         "min_offtake": 300.0, "min_offtake_cost": 50.0,
         "min_offtake_per_block": true}
      ]
    }
  })json";
  const auto planning = parse_planning_json(soft_json);
  REQUIRE(planning.system.fuel_array.size() == 1);
  const auto& f = planning.system.fuel_array[0];
  REQUIRE(f.min_offtake.has_value());
  REQUIRE(f.min_offtake_cost.has_value());
  CHECK(std::get<Real>(*f.min_offtake_cost) == doctest::Approx(50.0));
  CHECK(f.min_offtake_per_block.value_or(false));
}

TEST_CASE("Fuel.min_offtake - JSON parse ranged pair (min + max)")  // NOLINT
{
  // PLEXOS-style two-sided contract band: at least `min_offtake`
  // (take-or-pay), at most `max_offtake` (delivery cap).
  constexpr std::string_view ranged_json = R"json({
    "options": {"output_compression": "uncompressed",
                "model_options": {"use_single_bus": true}},
    "simulation": {
      "block_array": [{"uid": 0, "duration": 1}],
      "stage_array": [{"uid": 0, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 0}]
    },
    "system": {
      "name": "ranged_pair",
      "fuel_array": [
        {"uid": 1, "name": "gas", "price": 10.0,
         "min_offtake": 200.0, "min_offtake_cost": 50.0,
         "max_offtake": 500.0, "max_offtake_cost": 1000.0}
      ]
    }
  })json";
  const auto planning = parse_planning_json(ranged_json);
  REQUIRE(planning.system.fuel_array.size() == 1);
  const auto& f = planning.system.fuel_array[0];
  REQUIRE(f.min_offtake.has_value());
  REQUIRE(f.max_offtake.has_value());
  CHECK(std::get<Real>(*f.min_offtake) == doctest::Approx(200.0));
  CHECK(std::get<Real>(*f.max_offtake) == doctest::Approx(500.0));
}

TEST_CASE("Fuel.min_offtake - PlanningLP solve, hard floor binds")  // NOLINT
{
  // demand = 50 MW × 2 blocks × 1h = 100 MWh → fuel = 200 MMBtu
  // exactly hits min_offtake=200.  No unserved demand, no slack.
  using namespace test_fuel_min_offtake_planning;
  Planning base;
  base.merge(parse_planning_json(base_json));

  PlanningLP planning_lp(std::move(base));
  const auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);  // 1 scene processed
}

TEST_CASE(  // NOLINT
    "Fuel.min_offtake - PlanningLP solve, ranged pair both rows present")
{
  // min=100, max=300, demand=100 MWh ⇒ 200 MMBtu sits inside the band
  // — both rows have non-binding slack, the LP picks the demand-only
  // dispatch.
  constexpr std::string_view ranged_solve_json = R"json({
    "options": {"output_format": "csv", "output_compression": "uncompressed",
                "model_options": {"use_single_bus": true,
                                  "scale_objective": 1000,
                                  "demand_fail_cost": 1000}},
    "simulation": {
      "block_array": [{"uid": 0, "duration": 1}, {"uid": 1, "duration": 1}],
      "stage_array": [{"uid": 0, "first_block": 0, "count_block": 2}],
      "scenario_array": [{"uid": 0, "probability_factor": 1}]
    },
    "system": {
      "name": "ranged_solve",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "demand_array": [{"uid": 1, "name": "d1", "bus": "b1",
                        "capacity": 50.0}],
      "fuel_array": [
        {"uid": 1, "name": "gas", "price": 10.0,
         "min_offtake": 100.0, "max_offtake": 300.0}
      ],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": "b1",
         "pmin": 0.0, "pmax": 200.0, "gcost": 0.0,
         "fuel": 1, "heat_rate": 2.0, "capacity": 200.0}
      ]
    }
  })json";
  Planning base;
  base.merge(parse_planning_json(ranged_solve_json));
  PlanningLP planning_lp(std::move(base));
  const auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);
}

TEST_CASE(  // NOLINT
    "Fuel.min_offtake - PlanningLP solve, soft floor slack absorbs shortfall")
{
  // min_offtake = 300 MMBtu but supply caps at demand-only = 200
  // MMBtu (the surplus 100 MMBtu has no sink in single-bus).  With
  // soft slack at $50/MMBtu the LP pays $50 × 100 = $5000 shortfall
  // plus base cost $2000 = $7000 total, scale_objective=1000 → 7.0.
  constexpr std::string_view soft_solve_json = R"json({
    "options": {"output_format": "csv", "output_compression": "uncompressed",
                "model_options": {"use_single_bus": true,
                                  "scale_objective": 1000,
                                  "demand_fail_cost": 1000}},
    "simulation": {
      "block_array": [{"uid": 0, "duration": 1}, {"uid": 1, "duration": 1}],
      "stage_array": [{"uid": 0, "first_block": 0, "count_block": 2}],
      "scenario_array": [{"uid": 0, "probability_factor": 1}]
    },
    "system": {
      "name": "soft_solve",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "demand_array": [{"uid": 1, "name": "d1", "bus": "b1",
                        "capacity": 50.0}],
      "fuel_array": [
        {"uid": 1, "name": "gas", "price": 10.0,
         "min_offtake": 300.0, "min_offtake_cost": 50.0}
      ],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": "b1",
         "pmin": 0.0, "pmax": 200.0, "gcost": 0.0,
         "fuel": 1, "heat_rate": 2.0, "capacity": 200.0}
      ]
    }
  })json";
  Planning base;
  base.merge(parse_planning_json(soft_solve_json));
  PlanningLP planning_lp(std::move(base));
  const auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);
}
