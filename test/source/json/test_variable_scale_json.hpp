/**
 * @file      test_variable_scale_json.hpp
 * @brief     JSON serialization tests for VariableScale
 * @date      2026-03-18
 * @copyright BSD-3-Clause
 */

#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_variable_scale.hpp>

TEST_CASE("VariableScale JSON basic parsing")
{
  const std::string_view json_data = R"({
    "class_name": "Reservoir",
    "variable": "energy",
    "uid": 0,
    "scale": 1000.0
  })";

  const VariableScale vs = daw::json::from_json<VariableScale>(json_data);

  CHECK(vs.class_name == "Reservoir");
  CHECK(vs.variable == "energy");
  CHECK(vs.uid == 0);
  CHECK(vs.scale == doctest::Approx(1000.0));
}

TEST_CASE("VariableScale JSON with specific uid")
{
  const std::string_view json_data = R"({
    "class_name": "Battery",
    "variable": "energy",
    "uid": 42,
    "scale": 500.0
  })";

  const VariableScale vs = daw::json::from_json<VariableScale>(json_data);

  CHECK(vs.class_name == "Battery");
  CHECK(vs.variable == "energy");
  CHECK(vs.uid == 42);
  CHECK(vs.scale == doctest::Approx(500.0));
}

TEST_CASE("VariableScale JSON array parsing")
{
  const std::string_view json_data = R"([
    {"class_name": "Bus", "variable": "theta", "uid": 0, "scale": 1000.0},
    {"class_name": "Reservoir", "variable": "energy", "uid": 5, "scale": 100.0}
  ])";

  const auto scales = daw::json::from_json_array<VariableScale>(json_data);

  REQUIRE(scales.size() == 2);
  CHECK(scales[0].class_name == "Bus");
  CHECK(scales[0].scale == doctest::Approx(1000.0));
  CHECK(scales[1].class_name == "Reservoir");
  CHECK(scales[1].uid == 5);
}

TEST_CASE("VariableScale JSON round-trip serialization")
{
  VariableScale original;
  original.class_name = "Line";
  original.variable = "flow";
  original.uid = 99;
  original.scale = 0.001;

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const VariableScale roundtrip = daw::json::from_json<VariableScale>(json);
  CHECK(roundtrip.class_name == "Line");
  CHECK(roundtrip.variable == "flow");
  CHECK(roundtrip.uid == 99);
  CHECK(roundtrip.scale == doctest::Approx(0.001));
}
