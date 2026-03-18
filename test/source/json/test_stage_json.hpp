/**
 * @file      test_stage_json.hpp
 * @brief     JSON serialization tests for Stage
 * @date      2026-03-18
 * @copyright BSD-3-Clause
 */

#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_stage.hpp>

TEST_CASE("Stage JSON basic parsing")
{
  const std::string_view json_data = R"({
    "uid": 1,
    "name": "year_2025",
    "first_block": 0,
    "count_block": 24,
    "discount_factor": 0.9
  })";

  const Stage stage = daw::json::from_json<Stage>(json_data);

  CHECK(stage.uid == 1);
  REQUIRE(stage.name.has_value());
  CHECK(stage.name.value_or("") == "year_2025");
  CHECK(stage.first_block == 0);
  CHECK(stage.count_block == 24);
  REQUIRE(stage.discount_factor.has_value());
  CHECK(stage.discount_factor.value_or(0.0) == doctest::Approx(0.9));
}

TEST_CASE("Stage JSON minimal fields")
{
  const std::string_view json_data = R"({
    "uid": 2,
    "first_block": 5,
    "count_block": 10
  })";

  const Stage stage = daw::json::from_json<Stage>(json_data);

  CHECK(stage.uid == 2);
  CHECK_FALSE(stage.name.has_value());
  CHECK(stage.first_block == 5);
  CHECK(stage.count_block == 10);
  // discount_factor is nullopt when absent from JSON
  CHECK_FALSE(stage.discount_factor.has_value());
}

TEST_CASE("Stage JSON with active flag")
{
  const std::string_view json_data = R"({
    "uid": 3,
    "active": 0,
    "first_block": 0,
    "count_block": 1
  })";

  const Stage stage = daw::json::from_json<Stage>(json_data);

  REQUIRE(stage.active.has_value());
  CHECK(stage.active.value_or(true) == false);
  CHECK_FALSE(stage.is_active());
}

TEST_CASE("Stage JSON array parsing")
{
  const std::string_view json_data = R"([
    {"uid": 1, "first_block": 0, "count_block": 12},
    {"uid": 2, "first_block": 12, "count_block": 12, "discount_factor": 0.9}
  ])";

  const auto stages = daw::json::from_json_array<Stage>(json_data);

  REQUIRE(stages.size() == 2);
  CHECK(stages[0].first_block == 0);
  CHECK(stages[0].count_block == 12);
  CHECK(stages[1].first_block == 12);
  CHECK(stages[1].discount_factor.value_or(0.0) == doctest::Approx(0.9));
}

TEST_CASE("Stage JSON round-trip serialization")
{
  Stage original;
  original.uid = 5;
  original.name = "roundtrip_stage";
  original.first_block = 10;
  original.count_block = 8;
  original.discount_factor = 0.85;

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const Stage roundtrip = daw::json::from_json<Stage>(json);
  CHECK(roundtrip.uid == 5);
  CHECK(roundtrip.name.value_or("") == "roundtrip_stage");
  CHECK(roundtrip.first_block == 10);
  CHECK(roundtrip.count_block == 8);
  CHECK(roundtrip.discount_factor.value_or(0.0) == doctest::Approx(0.85));
}
