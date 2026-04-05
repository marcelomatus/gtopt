/**
 * @file      test_scenario_json.hpp
 * @brief     JSON serialization tests for Scenario
 * @date      2026-03-18
 * @copyright BSD-3-Clause
 */

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_scenario.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Scenario JSON basic parsing")
{
  const std::string_view json_data = R"({
    "uid": 1,
    "name": "dry_year",
    "probability_factor": 0.4
  })";

  const Scenario scenario = daw::json::from_json<Scenario>(json_data);

  CHECK(scenario.uid == 1);
  REQUIRE(scenario.name.has_value());
  CHECK(scenario.name.value_or("") == "dry_year");
  REQUIRE(scenario.probability_factor.has_value());
  CHECK(scenario.probability_factor.value_or(0.0) == doctest::Approx(0.4));
}

TEST_CASE("Scenario JSON minimal fields")
{
  const std::string_view json_data = R"({
    "uid": 2
  })";

  const Scenario scenario = daw::json::from_json<Scenario>(json_data);

  CHECK(scenario.uid == 2);
  CHECK_FALSE(scenario.name.has_value());
  CHECK_FALSE(scenario.active.has_value());
  CHECK(scenario.is_active());
  // probability_factor is nullopt when absent from JSON
  CHECK_FALSE(scenario.probability_factor.has_value());
}

TEST_CASE("Scenario JSON with active flag")
{
  const std::string_view json_data = R"({
    "uid": 3,
    "name": "inactive_scenario",
    "active": 0,
    "probability_factor": 0.5
  })";

  const Scenario scenario = daw::json::from_json<Scenario>(json_data);

  REQUIRE(scenario.active.has_value());
  CHECK(scenario.active.value_or(true) == false);
  CHECK_FALSE(scenario.is_active());
}

TEST_CASE("Scenario JSON array parsing")
{
  const std::string_view json_data = R"([
    {"uid": 1, "name": "wet", "probability_factor": 0.3},
    {"uid": 2, "name": "avg", "probability_factor": 0.4},
    {"uid": 3, "name": "dry", "probability_factor": 0.3}
  ])";

  const auto scenarios = daw::json::from_json_array<Scenario>(json_data);

  REQUIRE(scenarios.size() == 3);
  CHECK(scenarios[0].name.value_or("") == "wet");
  CHECK(scenarios[1].probability_factor.value_or(0.0) == doctest::Approx(0.4));
  CHECK(scenarios[2].name.value_or("") == "dry");
}

TEST_CASE("Scenario JSON round-trip serialization")
{
  Scenario original;
  original.uid = 7;
  original.name = "roundtrip_sc";
  original.active = true;
  original.probability_factor = 0.6;

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const Scenario roundtrip = daw::json::from_json<Scenario>(json);
  CHECK(roundtrip.uid == 7);
  CHECK(roundtrip.name.value_or("") == "roundtrip_sc");
  CHECK(roundtrip.probability_factor.value_or(0.0) == doctest::Approx(0.6));
}
