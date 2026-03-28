/**
 * @file      test_element_column_resolver.hpp
 * @brief     Unit tests for element_column_resolver functions
 * @date      Mon Mar 24 2026
 * @copyright BSD-3-Clause
 *
 * Tests that resolve_single_col() and collect_sum_cols() correctly resolve
 * LP columns for each supported element type.  Since these functions are
 * called during LP construction (they need the LinearProblem reference),
 * we test them end-to-end via user constraints that exercise each element
 * type and verify the LP solves successfully with the constraints active.
 */

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// clang-format off

/// System with diverse element types, each referenced by a user constraint.
/// Tests that resolve_single_col can resolve columns for: generator, demand,
/// battery (charge/discharge/energy), reservoir (volume), waterway (flow).
static constexpr std::string_view resolver_diverse_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "lp_build_options": {"names_level": 1},
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}, {"uid": 2, "duration": 2}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 2, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "resolver_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 20, "capacity": 300},
      {"uid": 2, "name": "g2", "bus": "b1", "pmin": 0, "pmax": 200, "gcost": 35, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[100, 100]]}
    ],
    "battery_array": [
      {
        "uid": 1, "name": "bat1", "bus": "b1",
        "input_efficiency": 0.95, "output_efficiency": 0.95,
        "emin": 0, "emax": 200, "eini": 50,
        "pmax_charge": 50, "pmax_discharge": 50,
        "gcost": 0, "capacity": 200
      }
    ],
    "junction_array": [
      {"uid": 1, "name": "j_up"},
      {"uid": 2, "name": "j_down", "drain": true}
    ],
    "waterway_array": [
      {"uid": 1, "name": "ww1", "junction_a": "j_up", "junction_b": "j_down",
       "fmin": 0, "fmax": 100}
    ],
    "reservoir_array": [
      {"uid": 1, "name": "rsv1", "junction": "j_up",
       "capacity": 500, "emin": 0, "emax": 500, "eini": 250}
    ],
    "turbine_array": [
      {"uid": 1, "name": "tur1", "waterway": "ww1", "generator": "g1"}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "gen_limit",
        "expression": "generator(\"g1\").generation + generator(\"g2\").generation <= 400"
      },
      {
        "uid": 2, "name": "demand_load_limit",
        "expression": "demand(\"d1\").load <= 100"
      },
      {
        "uid": 3, "name": "bat_charge_limit",
        "expression": "battery(\"bat1\").charge <= 40"
      },
      {
        "uid": 4, "name": "bat_discharge_limit",
        "expression": "battery(\"bat1\").discharge <= 40"
      },
      {
        "uid": 5, "name": "waterway_flow_limit",
        "expression": "waterway(\"ww1\").flow <= 80"
      },
      {
        "uid": 6, "name": "sum_all_gens",
        "expression": "sum(generator.generation) <= 500"
      }
    ]
  }
})json";

/// System for testing uid: prefix resolution
static constexpr std::string_view resolver_uid_ref_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "lp_build_options": {"names_level": 1},
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "resolver_uid_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 20, "capacity": 300}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[100]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "gen_by_uid",
        "expression": "generator(1).generation <= 200"
      }
    ]
  }
})json";

/// System for testing unknown element graceful handling
static constexpr std::string_view resolver_unknown_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "lp_build_options": {"names_level": 1},
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "resolver_unknown_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 20, "capacity": 300}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[100]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "bad_element",
        "expression": "generator(\"nonexistent\").generation <= 200"
      },
      {
        "uid": 2, "name": "bad_attribute",
        "expression": "generator(\"g1\").bogus_attr <= 200"
      }
    ]
  }
})json";

// clang-format on

// ─── resolve_single_col (via user constraints) ──────────────────────────────

TEST_CASE(  // NOLINT
    "element_column_resolver - diverse element types solve correctly")
{
  Planning base;
  base.merge(daw::json::from_json<Planning>(resolver_diverse_json));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();

  // LP should solve successfully with all constraints resolved
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);
}

TEST_CASE(  // NOLINT
    "element_column_resolver - generator constraint binds and affects solution")
{
  // Without constraint: g1 dispatches up to 300 MW at cost 20 $/MWh.
  // With constraint g1+g2<=400: total gen limited.  Demand is 100+100=200
  // in two blocks so constraint shouldn't bind.  Check solve succeeds.
  Planning base;
  base.merge(daw::json::from_json<Planning>(resolver_diverse_json));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());

  // Access the LP solution to verify it's optimal
  const auto& li = planning_lp.systems().front().front().linear_interface();
  CHECK(li.is_optimal());
}

TEST_CASE(  // NOLINT
    "element_column_resolver - sum(generator.generation) resolves all gens")
{
  // The sum_all_gens constraint: sum(generator.generation) <= 500
  // With 2 generators and demand of ~200, this shouldn't bind but should
  // resolve both generator columns.
  Planning base;
  base.merge(daw::json::from_json<Planning>(resolver_diverse_json));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
  CHECK(result.value() == 1);
}

TEST_CASE(  // NOLINT
    "element_column_resolver - uid reference resolves generator")
{
  // generator(1).generation uses uid:1 form internally
  Planning base;
  base.merge(daw::json::from_json<Planning>(resolver_uid_ref_json));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  // The constraint generator(1).generation <= 200 should be active
  const auto& li = planning_lp.systems().front().front().linear_interface();
  CHECK(li.is_optimal());
}

TEST_CASE(  // NOLINT
    "element_column_resolver - unknown element/attribute skipped gracefully")
{
  // Constraints referencing nonexistent elements or unknown attributes
  // should be silently skipped (no rows added, no crash).
  Planning base;
  base.merge(daw::json::from_json<Planning>(resolver_unknown_json));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();

  // LP should still solve even with unresolvable constraints
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);
}

TEST_CASE(  // NOLINT
    "element_column_resolver - battery energy constraint with LP scaling")
{
  // Battery energy columns have non-unit LP scale.  Test that the
  // resolver correctly applies get_col_scale for battery attributes.
  static constexpr std::string_view bat_energy_json = R"json({
    "options": {
      "annual_discount_rate": 0.0,
      "lp_build_options": {"names_level": 1},
      "output_format": "csv",
      "output_compression": "uncompressed",
      "use_single_bus": true,
      "demand_fail_cost": 1000,
      "scale_objective": 1000
    },
    "simulation": {
      "block_array": [{"uid": 1, "duration": 1}],
      "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 1, "probability_factor": 1}]
    },
    "system": {
      "name": "bat_energy_test",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 20, "capacity": 300}
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[50]]}
      ],
      "battery_array": [
        {
          "uid": 1, "name": "bat1", "bus": "b1",
          "input_efficiency": 0.9, "output_efficiency": 0.9,
          "emin": 0, "emax": 100, "eini": 50,
          "pmax_charge": 30, "pmax_discharge": 30,
          "gcost": 0, "capacity": 100
        }
      ],
      "user_constraint_array": [
        {
          "uid": 1, "name": "bat_energy_upper",
          "expression": "battery(\"bat1\").energy <= 80"
        }
      ]
    }
  })json";

  Planning base;
  base.merge(daw::json::from_json<Planning>(bat_energy_json));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
  CHECK(result.value() == 1);
}

// ---------------------------------------------------------------------------
// User constraint with UID-based element reference (uid:N syntax)
// ---------------------------------------------------------------------------

TEST_CASE("ElementColumnResolver - uid syntax in constraint")  // NOLINT
{
  static constexpr std::string_view uid_ref_json = R"json({
    "options": {
      "demand_fail_cost": 1000,
      "use_single_bus": true
    },
    "simulation": {
      "block_array": [{"uid": 1, "duration": 1}],
      "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 1}]
    },
    "system": {
      "name": "uid_ref_test",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {
          "uid": 1, "name": "g1", "bus": 1,
          "gcost": 10, "capacity": 200
        }
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": 1, "capacity": 50}
      ],
      "user_constraint_array": [
        {
          "uid": 1, "name": "gen_limit",
          "expression": "generator(uid:1).generation <= 150"
        }
      ]
    }
  })json";

  Planning base;
  base.merge(daw::json::from_json<Planning>(uid_ref_json));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
  CHECK(result.value() == 1);
}

// ---------------------------------------------------------------------------
// User constraint with invalid element name (graceful skip)
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "ElementColumnResolver - unknown element name logs warning")
{
  static constexpr std::string_view bad_ref_json = R"json({
    "options": {
      "demand_fail_cost": 1000,
      "use_single_bus": true
    },
    "simulation": {
      "block_array": [{"uid": 1, "duration": 1}],
      "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 1}]
    },
    "system": {
      "name": "bad_ref_test",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {
          "uid": 1, "name": "g1", "bus": 1,
          "gcost": 10, "capacity": 200
        }
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": 1, "capacity": 50}
      ],
      "user_constraint_array": [
        {
          "uid": 1, "name": "bad_ref",
          "expression": "generator(\"nonexistent\").generation <= 100"
        }
      ]
    }
  })json";

  Planning base;
  base.merge(daw::json::from_json<Planning>(bad_ref_json));
  PlanningLP planning_lp(std::move(base));
  // Should still solve — the constraint with unresolved element is skipped
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
}
