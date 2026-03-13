/**
 * @file      test_phase_json.hpp
 * @brief     JSON round-trip tests for Phase with aperture_set
 */

#pragma once

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_phase.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Phase JSON round-trip")  // NOLINT
{
  SUBCASE("without aperture_set")
  {
    constexpr std::string_view json = R"({
      "uid": 1,
      "first_stage": 0,
      "count_stage": 5
    })";
    const auto phase = daw::json::from_json<Phase>(json);
    CHECK(phase.uid == 1);
    CHECK(phase.first_stage == 0);
    CHECK(phase.count_stage == 5);
    CHECK(phase.aperture_set.empty());

    const auto out = daw::json::to_json(phase);
    const auto phase2 = daw::json::from_json<Phase>(out);
    CHECK(phase2.uid == 1);
    CHECK(phase2.first_stage == 0);
    CHECK(phase2.count_stage == 5);
    CHECK(phase2.aperture_set.empty());
  }

  SUBCASE("with aperture_set")
  {
    constexpr std::string_view json = R"({
      "uid": 3,
      "first_stage": 2,
      "count_stage": 1,
      "aperture_set": [1, 5, 10, 15]
    })";
    const auto phase = daw::json::from_json<Phase>(json);
    CHECK(phase.uid == 3);
    CHECK(phase.first_stage == 2);
    CHECK(phase.count_stage == 1);
    REQUIRE(phase.aperture_set.size() == 4);
    CHECK(phase.aperture_set[0] == 1);
    CHECK(phase.aperture_set[1] == 5);
    CHECK(phase.aperture_set[2] == 10);
    CHECK(phase.aperture_set[3] == 15);

    // Round-trip
    const auto out = daw::json::to_json(phase);
    const auto phase2 = daw::json::from_json<Phase>(out);
    CHECK(phase2.uid == 3);
    CHECK(phase2.aperture_set.size() == 4);
    CHECK(phase2.aperture_set[0] == 1);
    CHECK(phase2.aperture_set[3] == 15);
  }

  SUBCASE("with empty aperture_set array")
  {
    constexpr std::string_view json = R"({
      "uid": 2,
      "first_stage": 0,
      "count_stage": 1,
      "aperture_set": []
    })";
    const auto phase = daw::json::from_json<Phase>(json);
    CHECK(phase.uid == 2);
    CHECK(phase.aperture_set.empty());
  }

  SUBCASE("with null aperture_set")
  {
    constexpr std::string_view json = R"({
      "uid": 4,
      "first_stage": 0,
      "count_stage": 1,
      "aperture_set": null
    })";
    const auto phase = daw::json::from_json<Phase>(json);
    CHECK(phase.uid == 4);
    CHECK(phase.aperture_set.empty());
  }
}
