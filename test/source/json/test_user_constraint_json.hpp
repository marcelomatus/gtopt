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
  };

  auto json = daw::json::to_json(uc);
  const auto roundtrip = daw::json::from_json<UserConstraint>(json);

  CHECK(roundtrip.uid == 42);
  CHECK(roundtrip.name == "flow_limit");
  CHECK(roundtrip.active.value_or(false) == true);
  CHECK(roundtrip.expression == R"(line("L1").flow <= 200)");
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
      },
  };
  sys.user_constraint_file = "external.json";

  auto json = daw::json::to_json(sys);
  const auto roundtrip = daw::json::from_json<System>(json);

  REQUIRE(roundtrip.user_constraint_array.size() == 1);
  CHECK(roundtrip.user_constraint_array[0].uid == 1);
  CHECK(roundtrip.user_constraint_array[0].name == "test_constraint");
  REQUIRE(roundtrip.user_constraint_file.has_value());
  CHECK(roundtrip.user_constraint_file.value_or("") == "external.json");
}
