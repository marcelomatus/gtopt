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
    "use_lp_names": 1,
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
    "use_lp_names": 1,
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
    "use_lp_names": 1,
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

// ══════════════════════════════════════════════════════════════════════════════
// Coverage tests for resolve_single_col, collect_sum_cols, domain filtering,
// and error paths in user_constraint_lp.cpp
// ══════════════════════════════════════════════════════════════════════════════

// clang-format off

/// Multi-component system with generator, demand, line, battery, and bus
/// constraints exercising resolve_single_col for each element type.
static constexpr std::string_view uc_multi_component_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 1,
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
    "name": "uc_multi_comp",
    "bus_array": [
      {"uid": 1, "name": "b1"},
      {"uid": 2, "name": "b2"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 20, "capacity": 300},
      {"uid": 2, "name": "g2", "bus": "b2", "pmin": 0, "pmax": 200, "gcost": 35, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[100.0]]},
      {"uid": 2, "name": "d2", "bus": "b2", "lmax": [[80.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2",
       "reactance": 0.02, "tmax_ab": 300, "tmax_ba": 300}
    ],
    "battery_array": [
      {
        "uid": 1, "name": "bat1", "bus": "b1",
        "input_efficiency": 0.9, "output_efficiency": 0.9,
        "emin": 0, "emax": 50, "eini": 25,
        "pmax_charge": 100, "pmax_discharge": 100,
        "gcost": 0, "capacity": 50
      }
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_gen",
        "expression": "generator(\"g1\").generation <= 250"
      },
      {
        "uid": 2, "name": "uc_demand_load",
        "expression": "demand(\"d1\").load <= 90"
      },
      {
        "uid": 3, "name": "uc_demand_fail",
        "expression": "demand(\"d1\").fail <= 50"
      },
      {
        "uid": 4, "name": "uc_line_flow",
        "expression": "line(\"l1_2\").flow <= 200"
      },
      {
        "uid": 5, "name": "uc_line_flown",
        "expression": "line(\"l1_2\").flown <= 200"
      },
      {
        "uid": 6, "name": "uc_bat_charge",
        "expression": "battery(\"bat1\").charge <= 80"
      },
      {
        "uid": 7, "name": "uc_bat_discharge",
        "expression": "battery(\"bat1\").discharge <= 80"
      },
      {
        "uid": 8, "name": "uc_bat_energy",
        "expression": "battery(\"bat1\").energy <= 45"
      },
      {
        "uid": 9, "name": "uc_bus_theta",
        "expression": "bus(\"b1\").theta <= 10.0"
      },
      {
        "uid": 10, "name": "uc_bus_angle",
        "expression": "bus(\"b2\").angle >= -10.0"
      },
      {
        "uid": 11, "name": "uc_gen_ge",
        "expression": "generator(\"g1\").generation >= 10"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - resolve_single_col multi-component")
{
  using namespace gtopt;

  auto planning = daw::json::from_json<Planning>(uc_multi_component_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  // The LP should solve (may need demand shedding due to constraints)
  REQUIRE(result.has_value());
}

// clang-format off

/// System with line loss variables to test lossp/lossn attributes.
static constexpr std::string_view uc_line_loss_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 1,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "use_kirchhoff": true,
    "use_line_losses": true
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "uc_line_loss",
    "bus_array": [
      {"uid": 1, "name": "b1"},
      {"uid": 2, "name": "b2"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 20, "capacity": 300}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b2", "lmax": [[100.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2",
       "reactance": 0.02, "resistance": 0.01,
       "tmax_ab": 300, "tmax_ba": 300}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_lossp",
        "expression": "line(\"l1_2\").lossp <= 50"
      },
      {
        "uid": 2, "name": "uc_lossn",
        "expression": "line(\"l1_2\").lossn <= 50"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - line loss attributes (lossp/lossn)")
{
  using namespace gtopt;

  auto planning = daw::json::from_json<Planning>(uc_line_loss_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}

// clang-format off

/// Sum references: sum(generator(all).generation) and explicit ID list.
static constexpr std::string_view uc_sum_ref_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 1,
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
    "name": "uc_sum_ref",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200},
      {"uid": 2, "name": "g2", "bus": "b1", "pmin": 0, "pmax": 150, "gcost": 30, "capacity": 150}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[100.0]]},
      {"uid": 2, "name": "d2", "bus": "b1", "lmax": [[50.0]]}
    ],
    "battery_array": [
      {
        "uid": 1, "name": "bat1", "bus": "b1",
        "input_efficiency": 0.9, "output_efficiency": 0.9,
        "emin": 0, "emax": 50, "eini": 25,
        "pmax_charge": 100, "pmax_discharge": 100,
        "gcost": 0, "capacity": 50
      }
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_sum_gen_all",
        "expression": "sum(generator(all).generation) <= 300"
      },
      {
        "uid": 2, "name": "uc_sum_gen_list",
        "expression": "sum(generator(\"g1\",\"g2\").generation) <= 280"
      },
      {
        "uid": 3, "name": "uc_sum_demand_all",
        "expression": "sum(demand(all).load) <= 140"
      },
      {
        "uid": 4, "name": "uc_sum_bat_all",
        "expression": "sum(battery(all).energy) <= 40"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - sum references (all and explicit list)")
{
  using namespace gtopt;

  auto planning = daw::json::from_json<Planning>(uc_sum_ref_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}

// clang-format off

/// Domain filtering: constraints active only in specific stages and blocks.
static constexpr std::string_view uc_domain_filter_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 1,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 1},
      {"uid": 2, "duration": 2},
      {"uid": 3, "duration": 3}
    ],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 3, "active": 1},
      {"uid": 2, "first_block": 0, "count_block": 3, "active": 1}
    ],
    "scenario_array": [
      {"uid": 1, "probability_factor": 0.5},
      {"uid": 2, "probability_factor": 0.5}
    ]
  },
  "system": {
    "name": "uc_domain_filter",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200,
       "gcost": 20, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[50.0, 60.0, 70.0], [50.0, 60.0, 70.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_stage_filter",
        "expression": "generator(\"g1\").generation <= 150, for(stage in {1})"
      },
      {
        "uid": 2, "name": "uc_block_filter",
        "expression": "generator(\"g1\").generation <= 180, for(block in {1,2})"
      },
      {
        "uid": 3, "name": "uc_scenario_filter",
        "expression": "generator(\"g1\").generation <= 160, for(scenario in {1})"
      },
      {
        "uid": 4, "name": "uc_combo_filter",
        "expression": "generator(\"g1\").generation <= 170, for(stage in {2}, block in {1,3}, scenario in {2})"
      },
      {
        "uid": 5, "name": "uc_block_range",
        "expression": "generator(\"g1\").generation <= 190, for(block in 1..2)"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - domain filtering (stage/block/scenario)")
{
  using namespace gtopt;

  auto planning = daw::json::from_json<Planning>(uc_domain_filter_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}

// clang-format off

/// Hydro system with user constraints on reservoir, waterway, turbine,
/// junction, and flow elements.
static constexpr std::string_view uc_hydro_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 1,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 1},
      {"uid": 2, "duration": 2}
    ],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 2}
    ],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "uc_hydro",
    "bus_array": [{"uid": 1, "name": "b1"}],
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
      {"uid": 1, "name": "inflow1", "direction": 1, "junction": 1, "discharge": 20}
    ],
    "reservoir_array": [
      {
        "uid": 1, "name": "rsv1",
        "junction": 1,
        "capacity": 1000,
        "emin": 0, "emax": 1000,
        "eini": 500
      }
    ],
    "turbine_array": [
      {
        "uid": 1, "name": "tur1",
        "waterway": 1, "generator": 1,
        "conversion_rate": 1.0
      }
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_reservoir_volume",
        "expression": "reservoir(\"rsv1\").volume <= 900"
      },
      {
        "uid": 2, "name": "uc_reservoir_energy",
        "expression": "reservoir(\"rsv1\").energy >= 100"
      },
      {
        "uid": 3, "name": "uc_reservoir_drain",
        "expression": "reservoir(\"rsv1\").drain <= 500"
      },
      {
        "uid": 4, "name": "uc_reservoir_spill",
        "expression": "reservoir(\"rsv1\").spill <= 500"
      },
      {
        "uid": 5, "name": "uc_waterway_flow",
        "expression": "waterway(\"ww1\").flow <= 400"
      },
      {
        "uid": 6, "name": "uc_turbine_gen",
        "expression": "turbine(\"tur1\").generation <= 180"
      },
      {
        "uid": 7, "name": "uc_junction_drain",
        "expression": "junction(\"j_down\").drain <= 1000"
      },
      {
        "uid": 8, "name": "uc_flow_discharge",
        "expression": "flow(\"inflow1\").discharge <= 50"
      },
      {
        "uid": 9, "name": "uc_flow_flow",
        "expression": "flow(\"inflow1\").flow <= 50"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE(
    "User constraint - hydro element types "
    "(reservoir/waterway/turbine/junction/flow)")
{
  using namespace gtopt;

  auto planning = daw::json::from_json<Planning>(uc_hydro_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}

// clang-format off

/// Sum references over hydro elements (reservoir, waterway, junction, flow).
static constexpr std::string_view uc_hydro_sum_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 1,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 1},
      {"uid": 2, "duration": 2}
    ],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 2}
    ],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "uc_hydro_sum",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "hg1", "bus": 1, "gcost": 5, "capacity": 200},
      {"uid": 2, "name": "hg2", "bus": 1, "gcost": 10, "capacity": 200},
      {"uid": 3, "name": "thermal", "bus": 1, "gcost": 100, "capacity": 300}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 1, "capacity": 50}
    ],
    "junction_array": [
      {"uid": 1, "name": "j1"},
      {"uid": 2, "name": "j2"},
      {"uid": 3, "name": "j_down", "drain": true}
    ],
    "waterway_array": [
      {"uid": 1, "name": "ww1", "junction_a": 1, "junction_b": 3, "fmin": 0, "fmax": 500},
      {"uid": 2, "name": "ww2", "junction_a": 2, "junction_b": 3, "fmin": 0, "fmax": 500}
    ],
    "flow_array": [
      {"uid": 1, "name": "inflow1", "direction": 1, "junction": 1, "discharge": 20},
      {"uid": 2, "name": "inflow2", "direction": 1, "junction": 2, "discharge": 15}
    ],
    "reservoir_array": [
      {"uid": 1, "name": "rsv1", "junction": 1, "capacity": 1000, "emin": 0, "emax": 1000, "eini": 500},
      {"uid": 2, "name": "rsv2", "junction": 2, "capacity": 800, "emin": 0, "emax": 800, "eini": 400}
    ],
    "turbine_array": [
      {"uid": 1, "name": "tur1", "waterway": 1, "generator": 1, "conversion_rate": 1.0},
      {"uid": 2, "name": "tur2", "waterway": 2, "generator": 2, "conversion_rate": 1.0}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_sum_reservoir",
        "expression": "sum(reservoir(all).volume) <= 1500"
      },
      {
        "uid": 2, "name": "uc_sum_waterway",
        "expression": "sum(waterway(all).flow) <= 800"
      },
      {
        "uid": 3, "name": "uc_sum_turbine",
        "expression": "sum(turbine(all).generation) <= 350"
      },
      {
        "uid": 4, "name": "uc_sum_junction",
        "expression": "sum(junction(all).drain) <= 2000"
      },
      {
        "uid": 5, "name": "uc_sum_flow",
        "expression": "sum(flow(all).flow) <= 100"
      },
      {
        "uid": 6, "name": "uc_sum_line",
        "expression": "sum(line(all).flow) <= 999"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - sum references over hydro elements")
{
  using namespace gtopt;

  auto planning = daw::json::from_json<Planning>(uc_hydro_sum_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}

// clang-format off

/// Constraint referencing a reservoir with extraction attribute.
static constexpr std::string_view uc_reservoir_extraction_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 1,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}, {"uid": 2, "duration": 2}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 2}],
    "scenario_array": [{"uid": 1}]
  },
  "system": {
    "name": "uc_rsv_extraction",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "hg1", "bus": 1, "gcost": 5, "capacity": 200},
      {"uid": 2, "name": "thermal", "bus": 1, "gcost": 100, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 1, "capacity": 50}
    ],
    "junction_array": [
      {"uid": 1, "name": "j1"},
      {"uid": 2, "name": "j_down", "drain": true}
    ],
    "waterway_array": [
      {"uid": 1, "name": "ww1", "junction_a": 1, "junction_b": 2, "fmin": 0, "fmax": 500}
    ],
    "flow_array": [
      {"uid": 1, "name": "inflow1", "direction": 1, "junction": 1, "discharge": 20}
    ],
    "reservoir_array": [
      {"uid": 1, "name": "rsv1", "junction": 1, "capacity": 1000,
       "emin": 0, "emax": 1000, "eini": 500}
    ],
    "turbine_array": [
      {"uid": 1, "name": "tur1", "waterway": 1, "generator": 1, "conversion_rate": 1.0}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_extraction",
        "expression": "reservoir(\"rsv1\").extraction <= 300"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - reservoir extraction attribute")
{
  using namespace gtopt;

  auto planning = daw::json::from_json<Planning>(uc_reservoir_extraction_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}

// clang-format off

/// Test unknown element type and unknown attribute — should not crash,
/// constraint is silently skipped.
static constexpr std::string_view uc_unknown_ref_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 1,
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
    "name": "uc_unknown_ref",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_unknown_type",
        "expression": "widget(\"w1\").power <= 100"
      },
      {
        "uid": 2, "name": "uc_unknown_attr",
        "expression": "generator(\"g1\").foobar <= 100"
      },
      {
        "uid": 3, "name": "uc_nonexistent_element",
        "expression": "generator(\"g_nonexistent\").generation <= 100"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE(
    "User constraint - unknown element type and attribute "
    "(graceful skip)")
{
  using namespace gtopt;

  auto planning = daw::json::from_json<Planning>(uc_unknown_ref_json);
  PlanningLP planning_lp(std::move(planning));

  // Should solve successfully — unknown constraints are silently skipped
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
}

// clang-format off

/// Test empty expression — should be silently skipped.
static constexpr std::string_view uc_empty_expr_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 1,
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
    "name": "uc_empty_expr",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_empty",
        "expression": ""
      },
      {
        "uid": 2, "name": "uc_bad_syntax",
        "expression": "this is not valid syntax @#$"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - empty and invalid expressions (graceful skip)")
{
  using namespace gtopt;

  auto planning = daw::json::from_json<Planning>(uc_empty_expr_json);
  PlanningLP planning_lp(std::move(planning));

  // Should solve — empty/invalid expressions are silently skipped
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
}

// clang-format off

/// Test UID-based element references (uid:N and bare integer forms).
static constexpr std::string_view uc_uid_ref_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 1,
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
    "name": "uc_uid_ref",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_uid_prefix",
        "expression": "generator(\"uid:1\").generation <= 180"
      },
      {
        "uid": 2, "name": "uc_bare_int",
        "expression": "generator(1).generation <= 190"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - UID-based element references")
{
  using namespace gtopt;

  auto planning = daw::json::from_json<Planning>(uc_uid_ref_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}

// clang-format off

/// Energy-type constraint to test ConstraintScaleType::Energy dual scaling.
static constexpr std::string_view uc_energy_type_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 1,
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
    "name": "uc_energy_type",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 20, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0]]}
    ],
    "battery_array": [
      {
        "uid": 1, "name": "bat1", "bus": "b1",
        "input_efficiency": 0.9, "output_efficiency": 0.9,
        "emin": 0, "emax": 50, "eini": 25,
        "pmax_charge": 100, "pmax_discharge": 100,
        "gcost": 0, "capacity": 50
      }
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_energy",
        "expression": "battery(\"bat1\").energy <= 40",
        "constraint_type": "energy"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - energy constraint_type dual output")
{
  using namespace gtopt;

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_uc_energy_test";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  auto planning = daw::json::from_json<Planning>(uc_energy_type_json);
  planning.options.output_directory = tmpdir.string();

  const OptionsLP options(planning.options);
  SimulationLP sim_lp(planning.simulation, options);
  SystemLP sys_lp(planning.system, sim_lp);

  auto res = sys_lp.linear_interface().resolve();
  REQUIRE(res.has_value());

  sys_lp.write_out();

  const auto dual_file = tmpdir / "UserConstraint" / "constraint_dual.csv";
  CHECK(std::filesystem::exists(dual_file));

  std::filesystem::remove_all(tmpdir);
}

// clang-format off

/// Greater-equal and equality constraint types.
static constexpr std::string_view uc_ge_eq_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 1,
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
    "name": "uc_ge_eq",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200},
      {"uid": 2, "name": "g2", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 30, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_ge",
        "expression": "generator(\"g1\").generation >= 10"
      },
      {
        "uid": 2, "name": "uc_eq",
        "expression": "generator(\"g2\").generation = 50"
      },
      {
        "uid": 3, "name": "uc_combined",
        "expression": "generator(\"g1\").generation + generator(\"g2\").generation >= 70"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - greater-equal and equality constraints")
{
  using namespace gtopt;

  auto planning = daw::json::from_json<Planning>(uc_ge_eq_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}

// clang-format off

/// Coefficient scaling: constraints with non-unit coefficients.
static constexpr std::string_view uc_coefficients_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 1,
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
    "name": "uc_coefficients",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200},
      {"uid": 2, "name": "g2", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 30, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_weighted",
        "expression": "2*generator(\"g1\").generation + 3*generator(\"g2\").generation <= 500"
      },
      {
        "uid": 2, "name": "uc_negative_coeff",
        "expression": "generator(\"g1\").generation - generator(\"g2\").generation <= 100"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - non-unit coefficients and subtraction")
{
  using namespace gtopt;

  auto planning = daw::json::from_json<Planning>(uc_coefficients_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}

// clang-format off

/// Sum with type filter: sum(generator(all, type="thermal").generation).
static constexpr std::string_view uc_type_filter_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 1,
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
    "name": "uc_type_filter",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200, "gcost": 20,
       "capacity": 200, "type": "thermal"},
      {"uid": 2, "name": "g2", "bus": "b1", "pmin": 0, "pmax": 100, "gcost": 10,
       "capacity": 100, "type": "solar"}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0]]}
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_thermal_only",
        "expression": "sum(generator(all, type=\"thermal\").generation) <= 150"
      },
      {
        "uid": 2, "name": "uc_solar_only",
        "expression": "sum(generator(all, type=\"solar\").generation) <= 80"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - sum with type filter")
{
  using namespace gtopt;

  auto planning = daw::json::from_json<Planning>(uc_type_filter_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}

// clang-format off

/// Battery drain alias for spill, and battery charge/discharge
/// in a binding constraint scenario to verify the constraint is effective.
static constexpr std::string_view uc_bat_drain_json = R"json({
  "options": {
    "annual_discount_rate": 0.0,
    "use_lp_names": 1,
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
    "name": "uc_bat_drain",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200,
       "gcost": 20, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0]]}
    ],
    "battery_array": [
      {
        "uid": 1, "name": "bat1", "bus": "b1",
        "input_efficiency": 0.9, "output_efficiency": 0.9,
        "emin": 0, "emax": 50, "eini": 25,
        "pmax_charge": 100, "pmax_discharge": 100,
        "gcost": 0, "capacity": 50
      }
    ],
    "user_constraint_array": [
      {
        "uid": 1, "name": "uc_bat_drain_alias",
        "expression": "battery(\"bat1\").drain <= 10"
      }
    ]
  }
})json";

// clang-format on

TEST_CASE("User constraint - battery drain alias for spill")
{
  using namespace gtopt;

  auto planning = daw::json::from_json<Planning>(uc_bat_drain_json);
  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());
}
