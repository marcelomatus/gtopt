/**
 * @file      test_aperture_json.hpp
 * @brief     JSON serialization tests for Aperture
 * @date      2026-03-18
 * @copyright BSD-3-Clause
 */

#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_aperture.hpp>

TEST_CASE("Aperture JSON basic parsing")
{
  const std::string_view json_data = R"({
    "uid": 1,
    "name": "dry_year",
    "source_scenario": 5,
    "probability_factor": 0.3
  })";

  const Aperture ap = daw::json::from_json<Aperture>(json_data);

  CHECK(ap.uid == 1);
  REQUIRE(ap.name.has_value());
  CHECK(ap.name.value_or("") == "dry_year");
  CHECK(ap.source_scenario == 5);
  REQUIRE(ap.probability_factor.has_value());
  CHECK(ap.probability_factor.value_or(0.0) == doctest::Approx(0.3));
}

TEST_CASE("Aperture JSON minimal fields")
{
  const std::string_view json_data = R"({
    "uid": 2,
    "source_scenario": 10
  })";

  const Aperture ap = daw::json::from_json<Aperture>(json_data);

  CHECK(ap.uid == 2);
  CHECK_FALSE(ap.name.has_value());
  CHECK(ap.source_scenario == 10);
  // probability_factor is nullopt when absent from JSON
  CHECK_FALSE(ap.probability_factor.has_value());
}

TEST_CASE("Aperture JSON with active flag")
{
  const std::string_view json_data = R"({
    "uid": 3,
    "source_scenario": 1,
    "active": 0
  })";

  const Aperture ap = daw::json::from_json<Aperture>(json_data);

  REQUIRE(ap.active.has_value());
  CHECK(ap.active.value_or(true) == false);
  CHECK_FALSE(ap.is_active());
}

TEST_CASE("Aperture JSON array parsing")
{
  const std::string_view json_data = R"([
    {"uid": 1, "source_scenario": 1, "probability_factor": 0.5},
    {"uid": 2, "source_scenario": 5, "probability_factor": 0.3},
    {"uid": 3, "source_scenario": 10, "probability_factor": 0.2}
  ])";

  const auto apertures = daw::json::from_json_array<Aperture>(json_data);

  REQUIRE(apertures.size() == 3);
  CHECK(apertures[0].source_scenario == 1);
  CHECK(apertures[0].probability_factor.value_or(0.0) == doctest::Approx(0.5));
  CHECK(apertures[1].source_scenario == 5);
  CHECK(apertures[2].source_scenario == 10);
}

TEST_CASE("Aperture JSON round-trip serialization")
{
  Aperture original;
  original.uid = 7;
  original.name = "roundtrip_ap";
  original.source_scenario = 42;
  original.probability_factor = 0.75;

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const Aperture roundtrip = daw::json::from_json<Aperture>(json);
  CHECK(roundtrip.uid == 7);
  CHECK(roundtrip.name.value_or("") == "roundtrip_ap");
  CHECK(roundtrip.source_scenario == 42);
  CHECK(roundtrip.probability_factor.value_or(0.0) == doctest::Approx(0.75));
}
