/**
 * @file      test_model_options.hpp
 * @brief     Unit tests for ModelOptions struct
 * @date      2026-03-25
 * @copyright BSD-3-Clause
 */

#pragma once

#include <doctest/doctest.h>
#include <gtopt/model_options.hpp>

TEST_CASE("ModelOptions - Default construction")
{
  const ModelOptions opts {};

  CHECK_FALSE(opts.use_single_bus.has_value());
  CHECK_FALSE(opts.use_kirchhoff.has_value());
  CHECK_FALSE(opts.use_line_losses.has_value());
  CHECK_FALSE(opts.kirchhoff_threshold.has_value());
  CHECK_FALSE(opts.loss_segments.has_value());
  CHECK_FALSE(opts.scale_objective.has_value());
  CHECK_FALSE(opts.scale_theta.has_value());
  CHECK_FALSE(opts.demand_fail_cost.has_value());
  CHECK_FALSE(opts.reserve_fail_cost.has_value());
  CHECK_FALSE(opts.annual_discount_rate.has_value());

  CHECK_FALSE(opts.has_any());
}

TEST_CASE("ModelOptions - Construction with all values")
{
  const ModelOptions opts {
      .use_single_bus = true,
      .use_kirchhoff = false,
      .use_line_losses = true,
      .kirchhoff_threshold = 110.0,
      .loss_segments = 3,
      .scale_objective = 500.0,
      .scale_theta = 2000.0,
      .demand_fail_cost = 5000.0,
      .reserve_fail_cost = 3000.0,
      .annual_discount_rate = 0.08,
  };

  REQUIRE(opts.use_single_bus.has_value());
  CHECK(*opts.use_single_bus == true);

  REQUIRE(opts.use_kirchhoff.has_value());
  CHECK(*opts.use_kirchhoff == false);

  REQUIRE(opts.use_line_losses.has_value());
  CHECK(*opts.use_line_losses == true);

  REQUIRE(opts.kirchhoff_threshold.has_value());
  CHECK(*opts.kirchhoff_threshold == doctest::Approx(110.0));

  REQUIRE(opts.loss_segments.has_value());
  CHECK(*opts.loss_segments == 3);

  REQUIRE(opts.scale_objective.has_value());
  CHECK(*opts.scale_objective == doctest::Approx(500.0));

  REQUIRE(opts.scale_theta.has_value());
  CHECK(*opts.scale_theta == doctest::Approx(2000.0));

  REQUIRE(opts.demand_fail_cost.has_value());
  CHECK(*opts.demand_fail_cost == doctest::Approx(5000.0));

  REQUIRE(opts.reserve_fail_cost.has_value());
  CHECK(*opts.reserve_fail_cost == doctest::Approx(3000.0));

  REQUIRE(opts.annual_discount_rate.has_value());
  CHECK(*opts.annual_discount_rate == doctest::Approx(0.08));

  CHECK(opts.has_any());
}

TEST_CASE("ModelOptions - has_any with single field set")
{
  SUBCASE("use_single_bus")
  {
    const ModelOptions opts {
        .use_single_bus = false,
    };
    CHECK(opts.has_any());
  }

  SUBCASE("demand_fail_cost")
  {
    const ModelOptions opts {
        .demand_fail_cost = 1000.0,
    };
    CHECK(opts.has_any());
  }

  SUBCASE("scale_theta")
  {
    const ModelOptions opts {
        .scale_theta = 500.0,
    };
    CHECK(opts.has_any());
  }
}

TEST_CASE("ModelOptions - Merge fills missing fields")
{
  ModelOptions base {
      .use_single_bus = true,
      .scale_objective = 1000.0,
  };

  const ModelOptions overlay {
      .use_kirchhoff = false,
      .demand_fail_cost = 5000.0,
      .annual_discount_rate = 0.1,
  };

  base.merge(overlay);

  // Base fields preserved
  REQUIRE(base.use_single_bus.has_value());
  CHECK(*base.use_single_bus == true);
  REQUIRE(base.scale_objective.has_value());
  CHECK(*base.scale_objective == doctest::Approx(1000.0));

  // New fields from overlay
  REQUIRE(base.use_kirchhoff.has_value());
  CHECK(*base.use_kirchhoff == false);
  REQUIRE(base.demand_fail_cost.has_value());
  CHECK(*base.demand_fail_cost == doctest::Approx(5000.0));
  REQUIRE(base.annual_discount_rate.has_value());
  CHECK(*base.annual_discount_rate == doctest::Approx(0.1));

  // Still unset fields
  CHECK_FALSE(base.use_line_losses.has_value());
  CHECK_FALSE(base.kirchhoff_threshold.has_value());
  CHECK_FALSE(base.loss_segments.has_value());
}

TEST_CASE("ModelOptions - Merge overwrites existing values (overlay wins)")
{
  ModelOptions base {
      .use_single_bus = true,
      .scale_objective = 1000.0,
  };

  const ModelOptions overlay {
      .use_single_bus = false,
      .scale_objective = 999.0,
  };

  base.merge(overlay);

  // Overlay wins: set fields overwrite base
  REQUIRE(base.use_single_bus.has_value());
  CHECK(*base.use_single_bus == false);
  REQUIRE(base.scale_objective.has_value());
  CHECK(*base.scale_objective == doctest::Approx(999.0));
}

TEST_CASE("ModelOptions - Merge empty into filled does nothing")
{
  ModelOptions filled {
      .use_single_bus = true,
      .use_kirchhoff = false,
      .demand_fail_cost = 2000.0,
  };

  const ModelOptions empty {};
  filled.merge(empty);

  REQUIRE(filled.use_single_bus.has_value());
  CHECK(*filled.use_single_bus == true);
  REQUIRE(filled.use_kirchhoff.has_value());
  CHECK(*filled.use_kirchhoff == false);
  REQUIRE(filled.demand_fail_cost.has_value());
  CHECK(*filled.demand_fail_cost == doctest::Approx(2000.0));
}

TEST_CASE("ModelOptions - Merge filled into empty copies all")
{
  ModelOptions empty {};

  const ModelOptions filled {
      .use_single_bus = true,
      .use_kirchhoff = false,
      .use_line_losses = true,
      .kirchhoff_threshold = 110.0,
      .loss_segments = 3,
      .scale_objective = 500.0,
      .scale_theta = 2000.0,
      .demand_fail_cost = 5000.0,
      .reserve_fail_cost = 3000.0,
      .annual_discount_rate = 0.08,
  };

  empty.merge(filled);

  REQUIRE(empty.use_single_bus.has_value());
  CHECK(*empty.use_single_bus == true);
  REQUIRE(empty.loss_segments.has_value());
  CHECK(*empty.loss_segments == 3);
  REQUIRE(empty.annual_discount_rate.has_value());
  CHECK(*empty.annual_discount_rate == doctest::Approx(0.08));
  CHECK(empty.has_any());
}
