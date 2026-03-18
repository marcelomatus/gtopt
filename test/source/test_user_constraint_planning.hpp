/**
 * @file      test_user_constraint_planning.hpp
 * @brief     Integration tests: user constraints in Planning JSON + LP solve
 * @date      Wed Mar 12 03:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Tests that user constraints survive JSON deserialization, merge, and that
 * the LP solves successfully with constraints present in the system.
 * Also tests that dual values for user constraint rows are correctly stored
 * and written to the output context after solving.
 */

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/constraint_parser.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

// clang-format off

/// IEEE 4-bus case with user constraints added to the system
static constexpr std::string_view ieee4b_with_constraints_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 2,
    "output_format": "csv",
    "output_compression": "uncompressed",
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
    "name": "ieee_4b_constraints",
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
    ],
    "user_constraint_array": [
      {
        "uid": 1,
        "name": "gen_pair_limit",
        "expression": "generator(\"g1\").generation + generator(\"g2\").generation <= 300"
      },
      {
        "uid": 2,
        "name": "flow_bound",
        "expression": "line(\"l1_2\").flow <= 200, for(stage in all, block in all)"
      },
      {
        "uid": 3,
        "name": "inactive_constraint",
        "active": false,
        "expression": "generator(\"g1\").generation <= 10"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - JSON parse in Planning context")
{
  using namespace gtopt;
  auto planning = daw::json::from_json<Planning>(ieee4b_with_constraints_json);

  CHECK(planning.system.name == "ieee_4b_constraints");
  REQUIRE(planning.system.user_constraint_array.size() == 3);

  const auto& uc1 = planning.system.user_constraint_array[0];
  CHECK(uc1.uid == 1);
  CHECK(uc1.name == "gen_pair_limit");

  const auto& uc3 = planning.system.user_constraint_array[2];
  CHECK(uc3.active.value_or(true) == false);
}

TEST_CASE("User constraint - parse expressions from Planning JSON")
{
  using namespace gtopt;
  auto planning = daw::json::from_json<Planning>(ieee4b_with_constraints_json);

  REQUIRE(planning.system.user_constraint_array.size() == 3);

  SUBCASE("Parse gen_pair_limit expression")
  {
    const auto& uc = planning.system.user_constraint_array[0];
    auto expr = ConstraintParser::parse(uc.name, uc.expression);

    CHECK(expr.name == "gen_pair_limit");
    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(expr.rhs == doctest::Approx(300.0));
    REQUIRE(expr.terms.size() == 2);
    REQUIRE(expr.terms[0].element.has_value());
    CHECK(expr.terms[0].element.value_or(ElementRef {}).element_id == "g1");
    REQUIRE(expr.terms[1].element.has_value());
    CHECK(expr.terms[1].element.value_or(ElementRef {}).element_id == "g2");
  }

  SUBCASE("Parse flow_bound expression")
  {
    const auto& uc = planning.system.user_constraint_array[1];
    auto expr = ConstraintParser::parse(uc.name, uc.expression);

    CHECK(expr.name == "flow_bound");
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    CHECK(expr.terms[0].element.value_or(ElementRef {}).element_type == "line");
    CHECK(expr.domain.stages.is_all);
    CHECK(expr.domain.blocks.is_all);
  }
}

TEST_CASE("User constraint - merge preserves constraints")
{
  using namespace gtopt;

  Planning base;
  base.merge(daw::json::from_json<Planning>(ieee4b_with_constraints_json));

  CHECK(base.system.user_constraint_array.size() == 3);

  // Merge additional constraints from a second JSON
  constexpr std::string_view additional_json = R"({
    "system": {
      "user_constraint_array": [
        {
          "uid": 10,
          "name": "extra_limit",
          "expression": "generator(\"g1\").generation >= 50"
        }
      ]
    }
  })";

  base.merge(daw::json::from_json<Planning>(additional_json));

  CHECK(base.system.user_constraint_array.size() == 4);
  CHECK(base.system.user_constraint_array[3].name == "extra_limit");
}

TEST_CASE("User constraint - LP solve with constraints in JSON")
{
  // Verify that having user_constraint_array in the JSON does not break
  // the LP solve.  The constraints are wired into the LP assembly and
  // their row indices are stored for dual-value output.
  using namespace gtopt;

  Planning base;
  base.merge(daw::json::from_json<Planning>(ieee4b_with_constraints_json));

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
  CHECK(result.value() == 1);  // 1 scene successfully processed
}

TEST_CASE("User constraint - user_constraint_file in Planning JSON")
{
  using namespace gtopt;

  constexpr std::string_view json_with_file = R"({
    "options": {
      "demand_fail_cost": 1000,
      "output_compression": "uncompressed"
    },
    "simulation": {
      "block_array": [{"uid": 1, "duration": 1}],
      "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
      "scenario_array": [{"uid": 1}]
    },
    "system": {
      "name": "file_ref_test",
      "bus_array": [{"uid": 1, "name": "b1"}],
      "generator_array": [
        {"uid": 1, "name": "g1", "bus": 1, "capacity": 200}
      ],
      "demand_array": [
        {"uid": 1, "name": "d1", "bus": 1, "capacity": 50}
      ],
      "user_constraint_file": "my_constraints.json"
    }
  })";

  auto planning = daw::json::from_json<Planning>(json_with_file);

  REQUIRE(planning.system.user_constraint_file.has_value());
  CHECK(planning.system.user_constraint_file.value_or("")
        == "my_constraints.json");
  CHECK(planning.system.user_constraint_array.empty());
}

// clang-format off

/// Single-bus case with a tight generator capacity constraint to produce a
/// non-zero dual on the user constraint row.
static constexpr std::string_view single_bus_uc_dual_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 2,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "uc_dual_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 20, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[90.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1,
        "name": "gen_upper",
        "expression": "generator(\"g1\").generation <= 80",
        "constraint_type": "power"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - dual values written to output (CSV)")
{
  using namespace gtopt;

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_uc_dual_test";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  // Write the JSON to a temp file and run via gtopt_main-style direct API
  auto planning = daw::json::from_json<Planning>(single_bus_uc_dual_json);
  planning.options.output_directory = tmpdir.string();

  // Directly construct SimulationLP + SystemLP and run solve + write_out.
  const OptionsLP options(planning.options);
  SimulationLP sim_lp(planning.simulation, options);

  SystemLP sys_lp(planning.system, sim_lp);

  // Solve the LP
  auto res = sys_lp.linear_interface().resolve();
  REQUIRE(res.has_value());

  // Write output
  sys_lp.write_out();

  // The user constraint "gen_upper" limits generation to 80 MW.
  // With demand at 90 MW and fail_cost = 1000, the constraint is binding,
  // so the dual should be non-zero.
  const auto dual_file = tmpdir / "UserConstraint" / "constraint_dual.csv";
  CHECK(std::filesystem::exists(dual_file));

  if (std::filesystem::exists(dual_file)) {
    std::ifstream f(dual_file);
    const std::string content((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    // CSV has a header and at least one data row
    CHECK_FALSE(content.empty());
    // The header should mention scenario/stage/block columns
    CHECK(content.find("scenario") != std::string::npos);
    CHECK(content.find("stage") != std::string::npos);
    CHECK(content.find("block") != std::string::npos);
  }

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("User constraint - constraint_type field preserved")
{
  using namespace gtopt;

  auto planning = daw::json::from_json<Planning>(single_bus_uc_dual_json);

  REQUIRE(planning.system.user_constraint_array.size() == 1);
  const auto& uc = planning.system.user_constraint_array[0];
  REQUIRE(uc.constraint_type.has_value());
  CHECK(*uc.constraint_type == "power");  // NOLINT
}

// clang-format off

/// Same single-bus case but with constraint_type = "raw" to test
/// discount-only dual scaling.
static constexpr std::string_view single_bus_uc_raw_json = R"json({
  "options": {
    "annual_discount_rate": 0.1,
    "use_lp_names": 2,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "uc_raw_test",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 20, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[90.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 2,
        "name": "gen_upper_raw",
        "expression": "generator(\"g1\").generation <= 80",
        "constraint_type": "raw"
      },
      {
        "uid": 3,
        "name": "gen_upper_unitless",
        "expression": "generator(\"g1\").generation <= 80",
        "constraint_type": "unitless"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - raw/unitless type produces output CSV")
{
  using namespace gtopt;

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_uc_raw_test";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  auto planning = daw::json::from_json<Planning>(single_bus_uc_raw_json);
  planning.options.output_directory = tmpdir.string();

  const OptionsLP options(planning.options);
  SimulationLP sim_lp(planning.simulation, options);
  SystemLP sys_lp(planning.system, sim_lp);

  auto res = sys_lp.linear_interface().resolve();
  REQUIRE(res.has_value());

  sys_lp.write_out();

  // Both "raw" and "unitless" constraints should produce the dual output file.
  const auto dual_file = tmpdir / "UserConstraint" / "constraint_dual.csv";
  CHECK(std::filesystem::exists(dual_file));

  if (std::filesystem::exists(dual_file)) {
    std::ifstream f(dual_file);
    const std::string content((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    CHECK_FALSE(content.empty());
    CHECK(content.find("scenario") != std::string::npos);
    // Both constraints (uid 2 and uid 3) should have their own column
    CHECK(content.find("uid:2") != std::string::npos);
    CHECK(content.find("uid:3") != std::string::npos);
  }

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("User constraint - parse_constraint_scale_type from constraint_type")
{
  using namespace gtopt;

  // "raw" → Raw
  CHECK(parse_constraint_scale_type("raw") == ConstraintScaleType::Raw);
  // "unitless" → Raw
  CHECK(parse_constraint_scale_type("unitless") == ConstraintScaleType::Raw);
  // "power" → Power (default)
  CHECK(parse_constraint_scale_type("power") == ConstraintScaleType::Power);
  // absent/empty → Power
  CHECK(parse_constraint_scale_type("") == ConstraintScaleType::Power);
  // "energy" → Energy
  CHECK(parse_constraint_scale_type("energy") == ConstraintScaleType::Energy);
}
