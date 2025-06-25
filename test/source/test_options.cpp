/**
 * @file      test_options.cpp
 * @brief     Unit tests for Options class
 * @date      Fri May  3 10:15:00 2025
 * @author    Claude
 * @copyright BSD-3-Clause
 *
 * This module contains unit tests for the Options class.
 */

#include <doctest/doctest.h>
#include <gtopt/options.hpp>

TEST_CASE("Options - Default construction")
{
  using namespace gtopt;

  const Options options {};

  // Check that all optional fields are empty
  CHECK_FALSE(options.input_directory.has_value());
  CHECK_FALSE(options.input_format.has_value());
  CHECK_FALSE(options.demand_fail_cost.has_value());
  CHECK_FALSE(options.reserve_fail_cost.has_value());
  CHECK_FALSE(options.use_line_losses.has_value());
  CHECK_FALSE(options.use_kirchhoff.has_value());
  CHECK_FALSE(options.use_single_bus.has_value());
  CHECK_FALSE(options.kirchhoff_threshold.has_value());
  CHECK_FALSE(options.scale_objective.has_value());
  CHECK_FALSE(options.scale_theta.has_value());
  CHECK_FALSE(options.output_directory.has_value());
  CHECK_FALSE(options.output_format.has_value());
  CHECK_FALSE(options.compression_format.has_value());
  CHECK_FALSE(options.use_lp_names.has_value());
  CHECK_FALSE(options.use_uid_fname.has_value());
  CHECK_FALSE(options.annual_discount_rate.has_value());
}

TEST_CASE("Options - Construction with values")
{
  using namespace gtopt;

  Options options {.input_directory = "input_dir",
                   .input_format = "json",
                   .demand_fail_cost = 1000.0,
                   .reserve_fail_cost = 500.0,
                   .use_line_losses = true,
                   .use_kirchhoff = false,
                   .use_single_bus = true,
                   .kirchhoff_threshold = 0.01,
                   .scale_objective = 1.0,
                   .scale_theta = 10.0,
                   .output_directory = "output_dir",
                   .output_format = "csv",
                   .compression_format = "zip",
                   .use_lp_names = true,
                   .use_uid_fname = false,
                   .annual_discount_rate = 0.05};

  // Check that all fields have the expected values
  REQUIRE(options.input_directory.has_value());
  CHECK(*options.input_directory == "input_dir");

  REQUIRE(options.input_format.has_value());
  CHECK(*options.input_format == "json");

  REQUIRE(options.demand_fail_cost.has_value());
  CHECK(*options.demand_fail_cost == doctest::Approx(1000.0));

  REQUIRE(options.reserve_fail_cost.has_value());
  CHECK(*options.reserve_fail_cost == doctest::Approx(500.0));

  REQUIRE(options.use_line_losses.has_value());
  CHECK(*options.use_line_losses == true);

  REQUIRE(options.use_kirchhoff.has_value());
  CHECK(*options.use_kirchhoff == false);

  REQUIRE(options.use_single_bus.has_value());
  CHECK(*options.use_single_bus == true);

  REQUIRE(options.kirchhoff_threshold.has_value());
  CHECK(*options.kirchhoff_threshold == doctest::Approx(0.01));

  REQUIRE(options.scale_objective.has_value());
  CHECK(*options.scale_objective == doctest::Approx(1.0));

  REQUIRE(options.scale_theta.has_value());
  CHECK(*options.scale_theta == doctest::Approx(10.0));

  REQUIRE(options.output_directory.has_value());
  CHECK(*options.output_directory == "output_dir");

  REQUIRE(options.output_format.has_value());
  CHECK(*options.output_format == "csv");

  REQUIRE(options.compression_format.has_value());
  CHECK(*options.compression_format == "zip");

  REQUIRE(options.use_lp_names.has_value());
  CHECK(*options.use_lp_names == true);

  REQUIRE(options.use_uid_fname.has_value());
  CHECK(*options.use_uid_fname == false);

  REQUIRE(options.annual_discount_rate.has_value());
  CHECK(*options.annual_discount_rate == doctest::Approx(0.05));
}

TEST_CASE("Options - Merge operation")
{
  using namespace gtopt;

  // Create a base options object with some values
  Options base {.input_directory = "base_input",
                .use_kirchhoff = true,
                .scale_objective = 1.0,
                .output_directory = "base_output"};

  // Create options to merge with some overlapping and some new values
  Options overlay {.input_directory = "overlay_input",
                   .demand_fail_cost = 2000.0,
                   .use_kirchhoff = false,
                   .output_format = "parquet"};

  // Merge overlay into base
  base.merge(overlay);

  // Check that values were properly merged
  // Overlapping values should be replaced by overlay
  if (base.input_directory.has_value()) {
    CHECK(*base.input_directory == "overlay_input");
  } else {
    FAIL("input_directory should have been set");
  }

  if (base.use_kirchhoff.has_value()) {
    CHECK(*base.use_kirchhoff == false);
  } else {
    FAIL("use_kirchhoff should have been set");
  }

  if (base.input_directory.has_value()) {
    CHECK(*base.input_directory == "overlay_input");
  } else {
    FAIL("output_format should have been set");
  }

  if (base.output_directory.has_value()) {
    CHECK(*base.output_directory == "base_output");
  } else {
    FAIL("output_directory should have been set");
  }

  if (base.demand_fail_cost.has_value()) {
    CHECK(*base.demand_fail_cost == doctest::Approx(2000.0));
  } else {
    FAIL("demand_fail_cost should have been set");
  }

  if (base.use_kirchhoff.has_value()) {
    CHECK(*base.use_kirchhoff == false);
  } else {
    FAIL("use_kirchhoff should have been set");
  }

  // Non-overlapping values should remain unchanged in base
  if (base.scale_objective.has_value()) {
    CHECK(*base.scale_objective == doctest::Approx(1.0));
  } else {
    FAIL("scale_objective should not have been set");
  }

  if (base.output_directory.has_value()) {
    CHECK(*base.output_directory == "base_output");
  } else {
    FAIL("output_directory should not have been set");
  }

  // New values from overlay should be added to base
  if (base.output_format.has_value()) {
    CHECK(*base.output_format == "parquet");
  } else {
    FAIL("output_format should have been set");
  }
}

TEST_CASE("Options - Merging with empty options")
{
  using namespace gtopt;

  // Create options with values
  Options filled {.input_directory = "input_dir",
                  .demand_fail_cost = 1000.0,
                  .use_kirchhoff = true};

  // Create empty options
  Options empty {};

  // Test merging empty into filled (should not change anything)
  Options filled_copy = filled;
  filled_copy.merge(empty);

  if (filled_copy.input_directory.has_value()) {
    CHECK(*filled_copy.input_directory == "input_dir");
  } else {
    FAIL("input_directory should not have been removed");
  }

  if (filled_copy.demand_fail_cost.has_value()) {
    CHECK(*filled_copy.demand_fail_cost == doctest::Approx(1000.0));
  } else {
    FAIL("demand_fail_cost should not have been removed");
  }

  if (filled_copy.use_kirchhoff.has_value()) {
    CHECK(*filled_copy.use_kirchhoff == true);
  } else {
    FAIL("use_kirchhoff should not have been removed");
  }

  // Test merging filled into empty (empty should gain all values)
  empty.merge(filled);

  if (empty.input_directory.has_value()) {
    CHECK(*empty.input_directory == "input_dir");
  } else {
    FAIL("input_directory should have been set");
  }

  if (empty.demand_fail_cost.has_value()) {
    CHECK(*empty.demand_fail_cost == doctest::Approx(1000.0));
  } else {
    FAIL("demand_fail_cost should have been set");
  }

  REQUIRE(empty.use_kirchhoff.has_value());
  if (empty.use_kirchhoff.has_value()) {
    CHECK(*empty.use_kirchhoff == true);
  } else {
    FAIL("use_kirchhoff should have been set");
  }
}
