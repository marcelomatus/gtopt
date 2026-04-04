/**
 * @file      test_annual_discount_rate.hpp
 * @brief     Tests for annual_discount_rate in Simulation and fallback chain
 * @date      2026-03-30
 * @copyright BSD-3-Clause
 */

#pragma once

#include <doctest/doctest.h>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation.hpp>

// --- Struct-level tests ---

TEST_CASE("Simulation annual_discount_rate - struct defaults")
{  // NOLINT
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("default-constructed Simulation has nullopt")
  {
    const Simulation sim {};
    CHECK_FALSE(sim.annual_discount_rate.has_value());
  }

  SUBCASE("designated initializer sets value")
  {
    const Simulation sim {
        .annual_discount_rate = 0.12,
    };
    CHECK(sim.annual_discount_rate.value_or(-1.0) == doctest::Approx(0.12));
  }
}

// --- Merge tests ---

TEST_CASE("Simulation annual_discount_rate - merge behavior")
{  // NOLINT
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("merge overwrites nullopt with value")
  {
    Simulation base {};
    Simulation incoming {
        .annual_discount_rate = 0.05,
    };

    base.merge(std::move(incoming));

    CHECK(base.annual_discount_rate.value_or(-1.0) == doctest::Approx(0.05));
  }

  SUBCASE("merge does not overwrite existing value with nullopt")
  {
    Simulation base {
        .annual_discount_rate = 0.07,
    };
    Simulation incoming {};

    base.merge(std::move(incoming));

    CHECK(base.annual_discount_rate.value_or(-1.0) == doctest::Approx(0.07));
  }

  SUBCASE("merge overwrites existing value with new value")
  {
    Simulation base {
        .annual_discount_rate = 0.03,
    };
    Simulation incoming {
        .annual_discount_rate = 0.09,
    };

    base.merge(std::move(incoming));

    CHECK(base.annual_discount_rate.value_or(-1.0) == doctest::Approx(0.09));
  }
}

// --- PlanningOptionsLP fallback chain tests ---

TEST_CASE("annual_discount_rate - PlanningOptionsLP fallback chain")
{  // NOLINT
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("default PlanningOptionsLP returns 0.0")
  {
    const PlanningOptionsLP opts {};
    CHECK(opts.annual_discount_rate() == doctest::Approx(0.0));
  }

  SUBCASE("PlanningOptions top-level field takes precedence")
  {
    const PlanningOptions po {
        .annual_discount_rate = 0.1,
    };
    const PlanningOptionsLP opts(po);
    CHECK(opts.annual_discount_rate() == doctest::Approx(0.1));
  }
}
