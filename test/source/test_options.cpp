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
#include <gtopt/options_lp.hpp>

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

  Options options {
      .input_directory = "input_dir",
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
      .annual_discount_rate = 0.05,
  };

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
  Options base {
      .input_directory = "base_input",
      .use_kirchhoff = true,
      .scale_objective = 1.0,
      .output_directory = "base_output",
  };

  // Create options to merge with some overlapping and some new values
  Options overlay {
      .input_directory = "overlay_input",
      .demand_fail_cost = 2000.0,
      .use_kirchhoff = false,
      .output_format = "parquet",
  };

  // Merge overlay into base
  base.merge(std::move(overlay));

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
  Options filled {
      .input_directory = "input_dir",
      .demand_fail_cost = 1000.0,
      .use_kirchhoff = true,
  };

  // Create empty options
  Options empty {};

  // Test merging empty into filled (should not change anything)
  Options filled_copy = filled;
  filled_copy.merge(std::move(empty));

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
  empty = Options {};
  empty.merge(std::move(filled));

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

/**
 * @file      test_options_lp.cpp
 * @brief     Unit tests for OptionsLP class
 * @date      Fri May  3 10:15:00 2025
 * @author    Claude
 * @copyright BSD-3-Clause
 *
 * This module contains unit tests for the OptionsLP class.
 */


TEST_CASE("OptionsLP - Default construction")
{
  using namespace gtopt;

  // Create OptionsLP with default Options
  const OptionsLP options_lp {};

  // Check default values
  CHECK(options_lp.input_directory() == OptionsLP::default_input_directory);
  CHECK(options_lp.input_format() == OptionsLP::default_input_format);
  CHECK_FALSE(options_lp.demand_fail_cost().has_value());
  CHECK_FALSE(options_lp.reserve_fail_cost().has_value());
  CHECK(options_lp.use_line_losses() == OptionsLP::default_use_line_losses);
  CHECK(options_lp.use_kirchhoff() == OptionsLP::default_use_kirchhoff);
  CHECK(options_lp.use_single_bus() == OptionsLP::default_use_single_bus);
  CHECK(options_lp.kirchhoff_threshold()
        == doctest::Approx(OptionsLP::default_kirchhoff_threshold));
  CHECK(options_lp.scale_objective()
        == doctest::Approx(OptionsLP::default_scale_objective));
  CHECK(options_lp.scale_theta()
        == doctest::Approx(OptionsLP::default_scale_theta));
  CHECK(options_lp.output_directory() == OptionsLP::default_output_directory);
  CHECK(options_lp.output_format() == OptionsLP::default_output_format);
  CHECK(options_lp.compression_format()
        == OptionsLP::default_compression_format);
  CHECK(options_lp.use_lp_names() == OptionsLP::default_use_lp_names);
  CHECK(options_lp.use_uid_fname() == OptionsLP::default_use_uid_fname);
  CHECK(options_lp.annual_discount_rate()
        == doctest::Approx(OptionsLP::default_annual_discount_rate));
}

TEST_CASE("OptionsLP - Default construction 2")
{
  using namespace gtopt;

  // Create OptionsLP with default Options
  const Options options {};
  const OptionsLP options_lp {options};

  // Check default values
  CHECK(options_lp.input_directory() == OptionsLP::default_input_directory);
  CHECK(options_lp.input_format() == OptionsLP::default_input_format);
  CHECK_FALSE(options_lp.demand_fail_cost().has_value());
  CHECK_FALSE(options_lp.reserve_fail_cost().has_value());
  CHECK(options_lp.use_line_losses() == OptionsLP::default_use_line_losses);
  CHECK(options_lp.use_kirchhoff() == OptionsLP::default_use_kirchhoff);
  CHECK(options_lp.use_single_bus() == OptionsLP::default_use_single_bus);
  CHECK(options_lp.kirchhoff_threshold()
        == doctest::Approx(OptionsLP::default_kirchhoff_threshold));
  CHECK(options_lp.scale_objective()
        == doctest::Approx(OptionsLP::default_scale_objective));
  CHECK(options_lp.scale_theta()
        == doctest::Approx(OptionsLP::default_scale_theta));
  CHECK(options_lp.output_directory() == OptionsLP::default_output_directory);
  CHECK(options_lp.output_format() == OptionsLP::default_output_format);
  CHECK(options_lp.compression_format()
        == OptionsLP::default_compression_format);
  CHECK(options_lp.use_lp_names() == OptionsLP::default_use_lp_names);
  CHECK(options_lp.use_uid_fname() == OptionsLP::default_use_uid_fname);
  CHECK(options_lp.annual_discount_rate()
        == doctest::Approx(OptionsLP::default_annual_discount_rate));
}

TEST_CASE("OptionsLP - Default construction 3")
{
  using namespace gtopt;

  // Create OptionsLP with default Options
  const OptionsLP options_lp {Options {}};

  // Check default values
  CHECK(options_lp.input_directory() == OptionsLP::default_input_directory);
  CHECK(options_lp.input_format() == OptionsLP::default_input_format);
  CHECK_FALSE(options_lp.demand_fail_cost().has_value());
  CHECK_FALSE(options_lp.reserve_fail_cost().has_value());
  CHECK(options_lp.use_line_losses() == OptionsLP::default_use_line_losses);
  CHECK(options_lp.use_kirchhoff() == OptionsLP::default_use_kirchhoff);
  CHECK(options_lp.use_single_bus() == OptionsLP::default_use_single_bus);
  CHECK(options_lp.kirchhoff_threshold()
        == doctest::Approx(OptionsLP::default_kirchhoff_threshold));
  CHECK(options_lp.scale_objective()
        == doctest::Approx(OptionsLP::default_scale_objective));
  CHECK(options_lp.scale_theta()
        == doctest::Approx(OptionsLP::default_scale_theta));
  CHECK(options_lp.output_directory() == OptionsLP::default_output_directory);
  CHECK(options_lp.output_format() == OptionsLP::default_output_format);
  CHECK(options_lp.compression_format()
        == OptionsLP::default_compression_format);
  CHECK(options_lp.use_lp_names() == OptionsLP::default_use_lp_names);
  CHECK(options_lp.use_uid_fname() == OptionsLP::default_use_uid_fname);
  CHECK(options_lp.annual_discount_rate()
        == doctest::Approx(OptionsLP::default_annual_discount_rate));
}

TEST_CASE("OptionsLP - Construction with Options")
{
  using namespace gtopt;

  // Create Options with some values
  const Options options {
      .input_directory = "custom_input",
      .input_format = "json",
      .demand_fail_cost = 1000.0,
      .use_kirchhoff = false,
      .scale_objective = 2000.0,
      .output_directory = "custom_output",
      // Leaving some values unset to test defaults
  };

  // Create OptionsLP with custom Options
  const OptionsLP options_lp {options};

  // Check custom values
  CHECK(options_lp.input_directory() == "custom_input");
  CHECK(options_lp.input_format() == "json");
  REQUIRE(options_lp.demand_fail_cost().has_value());
  if (options_lp.demand_fail_cost()) {
    CHECK(*options_lp.demand_fail_cost() == doctest::Approx(1000.0));
  }
  CHECK(options_lp.use_kirchhoff() == false);  // Overridden
  CHECK(options_lp.scale_objective() == doctest::Approx(2000.0));  // Overridden
  CHECK(options_lp.output_directory() == "custom_output");

  // Check defaults for unset values
  CHECK_FALSE(options_lp.reserve_fail_cost().has_value());
  CHECK(options_lp.use_line_losses() == OptionsLP::default_use_line_losses);
  CHECK(options_lp.use_single_bus() == OptionsLP::default_use_single_bus);
  CHECK(options_lp.kirchhoff_threshold()
        == doctest::Approx(OptionsLP::default_kirchhoff_threshold));
  CHECK(options_lp.scale_theta()
        == doctest::Approx(OptionsLP::default_scale_theta));
  CHECK(options_lp.output_format() == OptionsLP::default_output_format);
  CHECK(options_lp.compression_format()
        == OptionsLP::default_compression_format);
  CHECK(options_lp.use_lp_names() == OptionsLP::default_use_lp_names);
  CHECK(options_lp.use_uid_fname() == OptionsLP::default_use_uid_fname);
  CHECK(options_lp.annual_discount_rate()
        == doctest::Approx(OptionsLP::default_annual_discount_rate));
}

TEST_CASE("OptionsLP - Test all accessor methods")
{
  using namespace gtopt;

  // Create Options with all values set
  const Options options {
      .input_directory = "test_input",
      .input_format = "csv",
      .demand_fail_cost = 1500.0,
      .reserve_fail_cost = 750.0,
      .use_line_losses = false,
      .use_kirchhoff = true,
      .use_single_bus = true,
      .kirchhoff_threshold = 0.05,
      .scale_objective = 500.0,
      .scale_theta = 20.0,
      .output_directory = "test_output",
      .output_format = "json",
      .compression_format = "bzip2",
      .use_lp_names = true,
      .use_uid_fname = true,
      .annual_discount_rate = 0.07,
  };

  // Create OptionsLP with all values set
  const OptionsLP options_lp {options};

  // Test all accessor methods
  CHECK(options_lp.input_directory() == "test_input");
  CHECK(options_lp.input_format() == "csv");
  REQUIRE(options_lp.demand_fail_cost().has_value());
  if (options_lp.demand_fail_cost()) {
    CHECK(*options_lp.demand_fail_cost() == doctest::Approx(1500.0));
  }
  REQUIRE(options_lp.reserve_fail_cost().has_value());

  if (options_lp.reserve_fail_cost()) {
    CHECK(*options_lp.reserve_fail_cost() == doctest::Approx(750.0));
  }

  CHECK(options_lp.use_line_losses() == false);
  CHECK(options_lp.use_kirchhoff() == true);
  CHECK(options_lp.use_single_bus() == true);
  CHECK(options_lp.kirchhoff_threshold() == doctest::Approx(0.05));
  CHECK(options_lp.scale_objective() == doctest::Approx(500.0));
  CHECK(options_lp.scale_theta() == doctest::Approx(20.0));
  CHECK(options_lp.output_directory() == "test_output");
  CHECK(options_lp.output_format() == "json");
  CHECK(options_lp.compression_format() == "bzip2");
  CHECK(options_lp.use_lp_names() == true);
  CHECK(options_lp.use_uid_fname() == true);
  CHECK(options_lp.annual_discount_rate() == doctest::Approx(0.07));
}
