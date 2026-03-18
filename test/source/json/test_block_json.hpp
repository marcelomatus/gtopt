/**
 * @file      test_block_json.hpp
 * @brief     JSON serialization tests for Block
 * @date      2026-03-18
 * @copyright BSD-3-Clause
 */

#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_block.hpp>

TEST_CASE("Block JSON basic parsing")
{
  const std::string_view json_data = R"({
    "uid": 1,
    "duration": 1.0
  })";

  const Block block = daw::json::from_json<Block>(json_data);

  CHECK(block.uid == 1);
  CHECK(block.duration == doctest::Approx(1.0));
  CHECK_FALSE(block.name.has_value());
}

TEST_CASE("Block JSON with name")
{
  const std::string_view json_data = R"({
    "uid": 2,
    "name": "peak_hour",
    "duration": 4.5
  })";

  const Block block = daw::json::from_json<Block>(json_data);

  CHECK(block.uid == 2);
  CHECK(block.name.value_or("") == "peak_hour");
  CHECK(block.duration == doctest::Approx(4.5));
}

TEST_CASE("Block JSON array parsing")
{
  const std::string_view json_data = R"([
    {"uid": 1, "duration": 1.0},
    {"uid": 2, "duration": 2.0},
    {"uid": 3, "name": "off_peak", "duration": 8.0}
  ])";

  const auto blocks = daw::json::from_json_array<Block>(json_data);

  REQUIRE(blocks.size() == 3);
  CHECK(blocks[0].uid == 1);
  CHECK(blocks[0].duration == doctest::Approx(1.0));
  CHECK(blocks[1].duration == doctest::Approx(2.0));
  CHECK(blocks[2].name.value_or("") == "off_peak");
  CHECK(blocks[2].duration == doctest::Approx(8.0));
}

TEST_CASE("Block JSON round-trip serialization")
{
  Block original;
  original.uid = 5;
  original.name = "roundtrip_block";
  original.duration = 3.14;

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const Block roundtrip = daw::json::from_json<Block>(json);
  CHECK(roundtrip.uid == 5);
  CHECK(roundtrip.name.value_or("") == "roundtrip_block");
  CHECK(roundtrip.duration == doctest::Approx(3.14));
}
