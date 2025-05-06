/**
 * @file      test_options_lp.cpp
 * @brief     Unit tests for OptionsLP class
 * @date      Fri May  3 10:15:00 2025
 * @author    Claude
 * @copyright BSD-3-Clause
 *
 * This module contains unit tests for the OptionsLP class.
 */

#include <doctest/doctest.h>
#include <gtopt/options_lp.hpp>

TEST_CASE("OptionsLP - Default construction")
{
  using namespace gtopt;

  // Create OptionsLP with default Options
  OptionsLP options_lp {};

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
  Options options {};
  OptionsLP options_lp {options};

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
  OptionsLP options_lp {Options {}};

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
  Options options {
      .input_directory = "custom_input",
      .input_format = "json",
      .demand_fail_cost = 1000.0,
      .use_kirchhoff = false,
      .scale_objective = 2000.0,
      .output_directory = "custom_output",
      // Leaving some values unset to test defaults
  };

  // Create OptionsLP with custom Options
  OptionsLP options_lp {options};

  // Check custom values
  CHECK(options_lp.input_directory() == "custom_input");
  CHECK(options_lp.input_format() == "json");
  REQUIRE(options_lp.demand_fail_cost().has_value());
  CHECK(*options_lp.demand_fail_cost() == doctest::Approx(1000.0));
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
  Options options {.input_directory = "test_input",
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
                   .annual_discount_rate = 0.07};

  // Create OptionsLP with all values set
  OptionsLP options_lp {options};

  // Test all accessor methods
  CHECK(options_lp.input_directory() == "test_input");
  CHECK(options_lp.input_format() == "csv");
  REQUIRE(options_lp.demand_fail_cost().has_value());
  CHECK(*options_lp.demand_fail_cost() == doctest::Approx(1500.0));
  REQUIRE(options_lp.reserve_fail_cost().has_value());
  CHECK(*options_lp.reserve_fail_cost() == doctest::Approx(750.0));
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
