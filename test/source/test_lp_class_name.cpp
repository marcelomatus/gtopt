// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_lp_class_name.cpp
 * @brief     Unit tests for LPClassName – PascalCase → snake_case conversion
 * @date      2026-04-23
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <format>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/lp_class_name.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("LPClassName single-word class")  // NOLINT
{
  constexpr LPClassName cn {"Generator"};
  CHECK(cn.full_name() == "Generator");
  CHECK(cn.short_name() == "generator");
  CHECK(cn.snake_case() == "generator");
}

TEST_CASE("LPClassName two-word class")  // NOLINT
{
  constexpr LPClassName cn {"GeneratorProfile"};
  CHECK(cn.full_name() == "GeneratorProfile");
  CHECK(cn.short_name() == "generator_profile");
}

TEST_CASE("LPClassName three-word class")  // NOLINT
{
  constexpr LPClassName cn {"ReserveProvision"};
  CHECK(cn.short_name() == "reserve_provision");
}

TEST_CASE("LPClassName longest known class name")  // NOLINT
{
  constexpr LPClassName cn {"ReservoirProductionFactor"};
  CHECK(cn.short_name() == "reservoir_production_factor");
}

TEST_CASE("LPClassName lowercase stays lowercase")  // NOLINT
{
  constexpr LPClassName cn {"bus"};
  CHECK(cn.short_name() == "bus");
}

TEST_CASE("LPClassName conversion to string_view yields full name")  // NOLINT
{
  constexpr LPClassName cn {"Battery"};
  const std::string_view sv = cn;
  CHECK(sv == "Battery");
}

TEST_CASE("LPClassName std::format uses full name")  // NOLINT
{
  constexpr LPClassName cn {"Demand"};
  const std::string s = std::format("{}", cn);
  CHECK(s == "Demand");
}

TEST_CASE("LPClassName short_name and snake_case are identical")  // NOLINT
{
  constexpr LPClassName cn {"FlowRight"};
  CHECK(cn.short_name() == cn.snake_case());
}

TEST_CASE("LPClassName various project class names")  // NOLINT
{
  struct Case
  {
    std::string_view full;
    std::string_view snake;
  };
  constexpr Case cases[] = {
      {"Bus", "bus"},
      {"Line", "line"},
      {"Stage", "stage"},
      {"Block", "block"},
      {"Scenario", "scenario"},
      {"Phase", "phase"},
      {"Battery", "battery"},
      {"Converter", "converter"},
      {"Junction", "junction"},
      {"Waterway", "waterway"},
      {"Reservoir", "reservoir"},
      {"Turbine", "turbine"},
  };
  for (const auto& [full, snake] : cases) {
    const LPClassName cn {full};
    CHECK(cn.short_name() == snake);
  }
}
