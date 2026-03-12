/**
 * @file      test_user_constraint_json.hpp
 * @brief     JSON serialization tests for UserConstraint
 * @date      Wed Mar 12 03:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_system.hpp>
#include <gtopt/json/json_user_constraint.hpp>
#include <gtopt/user_constraint.hpp>

TEST_CASE("UserConstraint JSON deserialization")
{
  using namespace gtopt;

  std::string_view json_data = R"json({
    "uid": 1,
    "name": "gen_pair_limit",
    "expression": "generator(\"TORO\").generation + generator(\"uid:23\").generation <= 300, for(stage in {4,5,6}, block in 1..30)"
  })json";

  const auto uc = daw::json::from_json<UserConstraint>(json_data);

  CHECK(uc.uid == 1);
  CHECK(uc.name == "gen_pair_limit");
  CHECK_FALSE(uc.active.has_value());
  CHECK(uc.expression.find("TORO") != std::string::npos);
}

TEST_CASE("UserConstraint JSON array deserialization")
{
  using namespace gtopt;

  std::string_view json_data = R"json([{
    "uid": 1,
    "name": "limit1",
    "active": true,
    "expression": "generator(\"G1\").generation <= 100"
  },{
    "uid": 2,
    "name": "limit2",
    "expression": "demand(\"D1\").load >= 50"
  }])json";

  auto constraints = daw::json::from_json_array<UserConstraint>(json_data);

  REQUIRE(constraints.size() == 2);
  CHECK(constraints[0].uid == 1);
  CHECK(constraints[0].name == "limit1");
  CHECK(constraints[0].active.value_or(false) == true);
  CHECK(constraints[1].uid == 2);
  CHECK(constraints[1].name == "limit2");
}

TEST_CASE("UserConstraint JSON round-trip")
{
  using namespace gtopt;

  const UserConstraint uc {
      .uid = 42,
      .name = "flow_limit",
      .active = true,
      .expression = R"(line("L1").flow <= 200)",
      .description = "Maximum flow on line L1",
  };

  auto json = daw::json::to_json(uc);
  const auto roundtrip = daw::json::from_json<UserConstraint>(json);

  CHECK(roundtrip.uid == 42);
  CHECK(roundtrip.name == "flow_limit");
  CHECK(roundtrip.active.value_or(false) == true);
  CHECK(roundtrip.expression == R"(line("L1").flow <= 200)");
  CHECK(roundtrip.description.value_or("") == "Maximum flow on line L1");
  CHECK_FALSE(roundtrip.constraint_type.has_value());
}

TEST_CASE("UserConstraint constraint_type field — power")
{
  using namespace gtopt;

  std::string_view json_data = R"({
    "uid": 10,
    "name": "gen_limit",
    "expression": "generator(\"G1\").generation <= 100",
    "constraint_type": "power"
  })";

  const auto uc = daw::json::from_json<UserConstraint>(json_data);
  REQUIRE(uc.constraint_type.has_value());
  CHECK(uc.constraint_type.value_or("") == "power");
}

TEST_CASE("UserConstraint constraint_type field — energy")
{
  using namespace gtopt;

  std::string_view json_data = R"({
    "uid": 11,
    "name": "energy_limit",
    "expression": "battery(\"B1\").energy <= 100",
    "constraint_type": "energy"
  })";

  const auto uc = daw::json::from_json<UserConstraint>(json_data);
  REQUIRE(uc.constraint_type.has_value());
  CHECK(uc.constraint_type.value_or("") == "energy");
}

TEST_CASE("UserConstraint constraint_type round-trip")
{
  using namespace gtopt;

  const UserConstraint uc {
      .uid = 20,
      .name = "energy_cap",
      .expression = "battery(\"bat\").energy <= 500",
      .constraint_type = "energy",
  };

  auto json = daw::json::to_json(uc);
  const auto rt = daw::json::from_json<UserConstraint>(json);

  REQUIRE(rt.constraint_type.has_value());
  CHECK(rt.constraint_type.value_or("") == "energy");
}

TEST_CASE("UserConstraint description field — absent is nullopt")
{
  using namespace gtopt;

  std::string_view json_data = R"({
    "uid": 7,
    "name": "no_desc",
    "expression": "generator(\"G1\").generation <= 100"
  })";

  const auto uc = daw::json::from_json<UserConstraint>(json_data);
  CHECK_FALSE(uc.description.has_value());
  CHECK_FALSE(uc.constraint_type.has_value());
}

TEST_CASE("System JSON with user_constraint_array")
{
  using namespace gtopt;

  std::string_view json_data = R"json({
    "name": "test_system",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": 1, "capacity": 200}
    ],
    "user_constraint_array": [
      {
        "uid": 1,
        "name": "gen_limit",
        "expression": "generator(\"g1\").generation <= 100"
      }
    ]
  })json";

  const auto sys = daw::json::from_json<System>(json_data);

  CHECK(sys.name == "test_system");
  CHECK(sys.bus_array.size() == 1);
  CHECK(sys.generator_array.size() == 1);
  REQUIRE(sys.user_constraint_array.size() == 1);
  CHECK(sys.user_constraint_array[0].uid == 1);
  CHECK(sys.user_constraint_array[0].name == "gen_limit");
  CHECK_FALSE(sys.user_constraint_file.has_value());
}

TEST_CASE("System JSON with user_constraint_file")
{
  using namespace gtopt;

  std::string_view json_data = R"({
    "name": "test_system",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "user_constraint_file": "constraints.json"
  })";

  const auto sys = daw::json::from_json<System>(json_data);

  CHECK(sys.user_constraint_array.empty());
  REQUIRE(sys.user_constraint_file.has_value());
  CHECK(sys.user_constraint_file.value_or("") == "constraints.json");
}

TEST_CASE("System JSON round-trip preserves user constraints")
{
  using namespace gtopt;

  System sys;
  sys.name = "roundtrip_test";
  sys.bus_array = {
      {
          .uid = 1,
          .name = "b1",
      },
  };
  sys.user_constraint_array = {
      UserConstraint {
          .uid = 1,
          .name = "test_constraint",
          .active = true,
          .expression = R"(generator("g1").generation <= 100)",
          .description = "Test description",
      },
  };
  sys.user_constraint_file = "external.json";

  auto json = daw::json::to_json(sys);
  const auto roundtrip = daw::json::from_json<System>(json);

  REQUIRE(roundtrip.user_constraint_array.size() == 1);
  CHECK(roundtrip.user_constraint_array[0].uid == 1);
  CHECK(roundtrip.user_constraint_array[0].name == "test_constraint");
  CHECK(roundtrip.user_constraint_array[0].description.value_or("")
        == "Test description");
  REQUIRE(roundtrip.user_constraint_file.has_value());
  CHECK(roundtrip.user_constraint_file.value_or("") == "external.json");
}

TEST_CASE("UserConstraint constraint_type field — raw")
{
  using namespace gtopt;

  std::string_view json_data = R"({
    "uid": 30,
    "name": "raw_limit",
    "expression": "generator(\"G1\").generation <= 100",
    "constraint_type": "raw"
  })";

  const auto uc = daw::json::from_json<UserConstraint>(json_data);
  REQUIRE(uc.constraint_type.has_value());
  CHECK(*uc.constraint_type == "raw");
  CHECK(parse_constraint_scale_type(*uc.constraint_type)
        == ConstraintScaleType::Raw);
}

TEST_CASE("UserConstraint constraint_type field — unitless")
{
  using namespace gtopt;

  std::string_view json_data = R"({
    "uid": 31,
    "name": "unitless_limit",
    "expression": "generator(\"G1\").generation <= 100",
    "constraint_type": "unitless"
  })";

  const auto uc = daw::json::from_json<UserConstraint>(json_data);
  REQUIRE(uc.constraint_type.has_value());
  CHECK(*uc.constraint_type == "unitless");
  CHECK(parse_constraint_scale_type(*uc.constraint_type)
        == ConstraintScaleType::Raw);
}

TEST_CASE("parse_constraint_scale_type — all valid values")
{
  using namespace gtopt;

  // Default / empty → Power
  CHECK(parse_constraint_scale_type("") == ConstraintScaleType::Power);
  CHECK(parse_constraint_scale_type("power") == ConstraintScaleType::Power);
  CHECK(parse_constraint_scale_type("unknown") == ConstraintScaleType::Power);

  // Energy
  CHECK(parse_constraint_scale_type("energy") == ConstraintScaleType::Energy);

  // Raw (both accepted aliases)
  CHECK(parse_constraint_scale_type("raw") == ConstraintScaleType::Raw);
  CHECK(parse_constraint_scale_type("unitless") == ConstraintScaleType::Raw);
}
