/**
 * @file      test_variable_scale_map.hpp
 * @brief     Unit tests for VariableScaleMap lookup
 * @date      2026-03-25
 * @copyright BSD-3-Clause
 */

#pragma once

#include <vector>

#include <doctest/doctest.h>
#include <gtopt/variable_scale.hpp>

TEST_CASE("VariableScaleMap - Empty map returns default 1.0")
{
  const VariableScaleMap map {};

  CHECK(map.empty());
  CHECK(map.lookup("Bus", "theta") == doctest::Approx(1.0));
  CHECK(map.lookup("Reservoir", "energy", 42) == doctest::Approx(1.0));
}

TEST_CASE("VariableScaleMap - Per-class lookup")
{
  const std::vector<VariableScale> scales {
      {
          .class_name = "Bus",
          .variable = "theta",
          .uid = unknown_uid,
          .scale = 1000.0,
      },
      {
          .class_name = "Reservoir",
          .variable = "energy",
          .uid = unknown_uid,
          .scale = 0.001,
      },
  };

  const VariableScaleMap map {scales};

  CHECK_FALSE(map.empty());
  CHECK(map.lookup("Bus", "theta") == doctest::Approx(1000.0));
  CHECK(map.lookup("Reservoir", "energy") == doctest::Approx(0.001));
  // Any UID should match the per-class entry
  CHECK(map.lookup("Bus", "theta", 5) == doctest::Approx(1000.0));
  CHECK(map.lookup("Reservoir", "energy", 99) == doctest::Approx(0.001));
}

TEST_CASE("VariableScaleMap - Per-element lookup takes priority")
{
  const std::vector<VariableScale> scales {
      {
          .class_name = "Battery",
          .variable = "energy",
          .uid = unknown_uid,
          .scale = 100.0,
      },
      {
          .class_name = "Battery",
          .variable = "energy",
          .uid = 42,
          .scale = 999.0,
      },
  };

  const VariableScaleMap map {scales};

  // UID 42 gets the per-element override
  CHECK(map.lookup("Battery", "energy", 42) == doctest::Approx(999.0));
  // Other UIDs fall back to per-class
  CHECK(map.lookup("Battery", "energy", 1) == doctest::Approx(100.0));
  CHECK(map.lookup("Battery", "energy", 99) == doctest::Approx(100.0));
  // No UID falls back to per-class
  CHECK(map.lookup("Battery", "energy") == doctest::Approx(100.0));
}

TEST_CASE("VariableScaleMap - Unknown class/variable returns 1.0")
{
  const std::vector<VariableScale> scales {
      {
          .class_name = "Bus",
          .variable = "theta",
          .uid = unknown_uid,
          .scale = 1000.0,
      },
  };

  const VariableScaleMap map {scales};

  CHECK(map.lookup("Bus", "flow") == doctest::Approx(1.0));
  CHECK(map.lookup("Line", "theta") == doctest::Approx(1.0));
  CHECK(map.lookup("Generator", "power") == doctest::Approx(1.0));
}

TEST_CASE("VariableScaleMap - Multiple per-element entries")
{
  const std::vector<VariableScale> scales {
      {
          .class_name = "Reservoir",
          .variable = "volume",
          .uid = 1,
          .scale = 10.0,
      },
      {
          .class_name = "Reservoir",
          .variable = "volume",
          .uid = 2,
          .scale = 20.0,
      },
      {
          .class_name = "Reservoir",
          .variable = "volume",
          .uid = 3,
          .scale = 30.0,
      },
  };

  const VariableScaleMap map {scales};

  CHECK(map.lookup("Reservoir", "volume", 1) == doctest::Approx(10.0));
  CHECK(map.lookup("Reservoir", "volume", 2) == doctest::Approx(20.0));
  CHECK(map.lookup("Reservoir", "volume", 3) == doctest::Approx(30.0));
  // No per-class entry, so fallback to 1.0
  CHECK(map.lookup("Reservoir", "volume", 99) == doctest::Approx(1.0));
  CHECK(map.lookup("Reservoir", "volume") == doctest::Approx(1.0));
}
