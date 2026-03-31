/**
 * @file      test_options.hpp
 * @brief     Unit tests for Options class
 * @date      Fri May  3 10:15:00 2025
 * @author    Claude
 * @copyright BSD-3-Clause
 *
 * This module contains unit tests for the Options class.
 */

#include <doctest/doctest.h>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/solver_options.hpp>

TEST_CASE("Options - Default construction")
{
  using namespace gtopt;

  const PlanningOptions options {};

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
  CHECK_FALSE(options.output_compression.has_value());
  CHECK_FALSE(options.lp_build_options.names_level.has_value());
  CHECK_FALSE(options.use_uid_fname.has_value());
  CHECK_FALSE(options.annual_discount_rate.has_value());
  // Check solver_options defaults
  CHECK(options.solver_options.algorithm == LPAlgo::barrier);
  CHECK(options.solver_options.threads == 2);
  CHECK(options.solver_options.presolve == true);
}

TEST_CASE("Options - Construction with values")
{
  using namespace gtopt;

  PlanningOptions options {
      .input_directory = "input_dir",
      .input_format = DataFormat::csv,
      .demand_fail_cost = 1000.0,
      .reserve_fail_cost = 500.0,
      .use_line_losses = true,
      .use_kirchhoff = false,
      .use_single_bus = true,
      .kirchhoff_threshold = 0.01,
      .scale_objective = 1.0,
      .scale_theta = 10.0,
      .annual_discount_rate = 0.05,
      .output_directory = "output_dir",
      .output_format = DataFormat::csv,
      .output_compression = CompressionCodec::gzip,
      .use_uid_fname = false,
      .lp_build_options {
          .names_level = LpNamesLevel::only_cols,
      },
  };

  // Check that all fields have the expected values
  REQUIRE(options.input_directory.has_value());
  CHECK(*options.input_directory == "input_dir");

  REQUIRE(options.input_format.has_value());
  CHECK(*options.input_format == DataFormat::csv);

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
  CHECK(*options.output_format == DataFormat::csv);

  REQUIRE(options.output_compression.has_value());
  CHECK(*options.output_compression == CompressionCodec::gzip);

  REQUIRE(options.lp_build_options.names_level.has_value());
  CHECK(*options.lp_build_options.names_level == LpNamesLevel::only_cols);

  REQUIRE(options.use_uid_fname.has_value());
  CHECK(*options.use_uid_fname == false);

  REQUIRE(options.annual_discount_rate.has_value());
  CHECK(*options.annual_discount_rate == doctest::Approx(0.05));
}

TEST_CASE("Options - Merge operation")
{
  using namespace gtopt;

  // Create a base options object with some values
  PlanningOptions base {
      .input_directory = "base_input",
      .use_kirchhoff = true,
      .scale_objective = 1.0,
      .output_directory = "base_output",
  };

  // Create options to merge with some overlapping and some new values
  PlanningOptions overlay {
      .input_directory = "overlay_input",
      .demand_fail_cost = 2000.0,
      .use_kirchhoff = false,
      .output_format = DataFormat::parquet,
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
    CHECK(*base.output_format == DataFormat::parquet);
  } else {
    FAIL("output_format should have been set");
  }
}

TEST_CASE("Options - Merging with empty options")
{
  using namespace gtopt;

  // Create options with values
  PlanningOptions filled {
      .input_directory = "input_dir",
      .demand_fail_cost = 1000.0,
      .use_kirchhoff = true,
  };

  // Create empty options
  PlanningOptions empty {};

  // Test merging empty into filled (should not change anything)
  PlanningOptions filled_copy = filled;
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
  empty = PlanningOptions {};
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

TEST_CASE("PlanningOptionsLP - Default construction")
{
  using namespace gtopt;

  // Create PlanningOptionsLP with default Options
  const PlanningOptionsLP options_lp {};

  // Check default values
  CHECK(options_lp.input_directory()
        == PlanningOptionsLP::default_input_directory);
  CHECK(options_lp.input_format_enum()
        == PlanningOptionsLP::default_input_format);
  CHECK_FALSE(options_lp.demand_fail_cost().has_value());
  CHECK_FALSE(options_lp.reserve_fail_cost().has_value());
  CHECK(options_lp.use_line_losses()
        == PlanningOptionsLP::default_use_line_losses);
  CHECK(options_lp.use_kirchhoff() == PlanningOptionsLP::default_use_kirchhoff);
  CHECK(options_lp.use_single_bus()
        == PlanningOptionsLP::default_use_single_bus);
  CHECK(options_lp.kirchhoff_threshold()
        == doctest::Approx(PlanningOptionsLP::default_kirchhoff_threshold));
  CHECK(options_lp.scale_objective()
        == doctest::Approx(PlanningOptionsLP::default_scale_objective));
  CHECK(options_lp.scale_theta()
        == doctest::Approx(PlanningOptionsLP::default_scale_theta));
  CHECK(options_lp.output_directory()
        == PlanningOptionsLP::default_output_directory);
  CHECK(options_lp.output_format_enum()
        == PlanningOptionsLP::default_output_format);
  CHECK(options_lp.output_compression_enum()
        == PlanningOptionsLP::default_output_compression);
  CHECK(options_lp.names_level() == PlanningOptionsLP::default_names_level);
  CHECK(options_lp.use_uid_fname() == PlanningOptionsLP::default_use_uid_fname);
  CHECK(options_lp.annual_discount_rate()
        == doctest::Approx(PlanningOptionsLP::default_annual_discount_rate));
}

TEST_CASE("PlanningOptionsLP - Default construction 2")
{
  using namespace gtopt;

  // Create PlanningOptionsLP with default Options
  const PlanningOptions options {};
  const PlanningOptionsLP options_lp {options};

  // Check default values
  CHECK(options_lp.input_directory()
        == PlanningOptionsLP::default_input_directory);
  CHECK(options_lp.input_format_enum()
        == PlanningOptionsLP::default_input_format);
  CHECK_FALSE(options_lp.demand_fail_cost().has_value());
  CHECK_FALSE(options_lp.reserve_fail_cost().has_value());
  CHECK(options_lp.use_line_losses()
        == PlanningOptionsLP::default_use_line_losses);
  CHECK(options_lp.use_kirchhoff() == PlanningOptionsLP::default_use_kirchhoff);
  CHECK(options_lp.use_single_bus()
        == PlanningOptionsLP::default_use_single_bus);
  CHECK(options_lp.kirchhoff_threshold()
        == doctest::Approx(PlanningOptionsLP::default_kirchhoff_threshold));
  CHECK(options_lp.scale_objective()
        == doctest::Approx(PlanningOptionsLP::default_scale_objective));
  CHECK(options_lp.scale_theta()
        == doctest::Approx(PlanningOptionsLP::default_scale_theta));
  CHECK(options_lp.output_directory()
        == PlanningOptionsLP::default_output_directory);
  CHECK(options_lp.output_format_enum()
        == PlanningOptionsLP::default_output_format);
  CHECK(options_lp.output_compression_enum()
        == PlanningOptionsLP::default_output_compression);
  CHECK(options_lp.names_level() == PlanningOptionsLP::default_names_level);
  CHECK(options_lp.use_uid_fname() == PlanningOptionsLP::default_use_uid_fname);
  CHECK(options_lp.annual_discount_rate()
        == doctest::Approx(PlanningOptionsLP::default_annual_discount_rate));
}

TEST_CASE("PlanningOptionsLP - Default construction 3")
{
  using namespace gtopt;

  // Create PlanningOptionsLP with default Options
  const PlanningOptionsLP options_lp {PlanningOptions {}};

  // Check default values
  CHECK(options_lp.input_directory()
        == PlanningOptionsLP::default_input_directory);
  CHECK(options_lp.input_format_enum()
        == PlanningOptionsLP::default_input_format);
  CHECK_FALSE(options_lp.demand_fail_cost().has_value());
  CHECK_FALSE(options_lp.reserve_fail_cost().has_value());
  CHECK(options_lp.use_line_losses()
        == PlanningOptionsLP::default_use_line_losses);
  CHECK(options_lp.use_kirchhoff() == PlanningOptionsLP::default_use_kirchhoff);
  CHECK(options_lp.use_single_bus()
        == PlanningOptionsLP::default_use_single_bus);
  CHECK(options_lp.kirchhoff_threshold()
        == doctest::Approx(PlanningOptionsLP::default_kirchhoff_threshold));
  CHECK(options_lp.scale_objective()
        == doctest::Approx(PlanningOptionsLP::default_scale_objective));
  CHECK(options_lp.scale_theta()
        == doctest::Approx(PlanningOptionsLP::default_scale_theta));
  CHECK(options_lp.output_directory()
        == PlanningOptionsLP::default_output_directory);
  CHECK(options_lp.output_format_enum()
        == PlanningOptionsLP::default_output_format);
  CHECK(options_lp.output_compression_enum()
        == PlanningOptionsLP::default_output_compression);
  CHECK(options_lp.names_level() == PlanningOptionsLP::default_names_level);
  CHECK(options_lp.use_uid_fname() == PlanningOptionsLP::default_use_uid_fname);
  CHECK(options_lp.annual_discount_rate()
        == doctest::Approx(PlanningOptionsLP::default_annual_discount_rate));
}

TEST_CASE("PlanningOptionsLP - Construction with Options")
{
  using namespace gtopt;

  // Create Options with some values
  const PlanningOptions options {
      .input_directory = "custom_input",
      .input_format = DataFormat::csv,
      .demand_fail_cost = 1000.0,
      .use_kirchhoff = false,
      .scale_objective = 2000.0,
      .output_directory = "custom_output",
      // Leaving some values unset to test defaults
  };

  // Create PlanningOptionsLP with custom Options
  const PlanningOptionsLP options_lp {options};

  // Check custom values
  CHECK(options_lp.input_directory() == "custom_input");
  CHECK(options_lp.input_format() == "csv");
  REQUIRE(options_lp.demand_fail_cost().has_value());
  if (options_lp.demand_fail_cost()) {
    CHECK(*options_lp.demand_fail_cost() == doctest::Approx(1000.0));
  }
  CHECK(options_lp.use_kirchhoff() == false);  // Overridden
  CHECK(options_lp.scale_objective() == doctest::Approx(2000.0));  // Overridden
  CHECK(options_lp.output_directory() == "custom_output");

  // Check defaults for unset values
  CHECK_FALSE(options_lp.reserve_fail_cost().has_value());
  CHECK(options_lp.use_line_losses()
        == PlanningOptionsLP::default_use_line_losses);
  CHECK(options_lp.use_single_bus()
        == PlanningOptionsLP::default_use_single_bus);
  CHECK(options_lp.kirchhoff_threshold()
        == doctest::Approx(PlanningOptionsLP::default_kirchhoff_threshold));
  CHECK(options_lp.scale_theta()
        == doctest::Approx(PlanningOptionsLP::default_scale_theta));
  CHECK(options_lp.output_format_enum()
        == PlanningOptionsLP::default_output_format);
  CHECK(options_lp.output_compression_enum()
        == PlanningOptionsLP::default_output_compression);
  CHECK(options_lp.names_level() == PlanningOptionsLP::default_names_level);
  CHECK(options_lp.use_uid_fname() == PlanningOptionsLP::default_use_uid_fname);
  CHECK(options_lp.annual_discount_rate()
        == doctest::Approx(PlanningOptionsLP::default_annual_discount_rate));
}

TEST_CASE("PlanningOptionsLP - Test all accessor methods")
{
  using namespace gtopt;

  // Create Options with all values set
  const PlanningOptions options {
      .input_directory = "test_input",
      .input_format = DataFormat::csv,
      .demand_fail_cost = 1500.0,
      .reserve_fail_cost = 750.0,
      .use_line_losses = false,
      .use_kirchhoff = true,
      .use_single_bus = true,
      .kirchhoff_threshold = 0.05,
      .scale_objective = 500.0,
      .scale_theta = 20.0,
      .annual_discount_rate = 0.07,
      .output_directory = "test_output",
      .output_format = DataFormat::parquet,
      .output_compression = CompressionCodec::bzip2,
      .use_uid_fname = true,
      .lp_build_options {
          .names_level = LpNamesLevel::only_cols,
      },
  };

  // Create PlanningOptionsLP with all values set
  const PlanningOptionsLP options_lp {options};

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
  CHECK(options_lp.output_format() == "parquet");
  CHECK(options_lp.output_compression() == "bzip2");
  CHECK(options_lp.names_level() == LpNamesLevel::only_cols);
  CHECK(options_lp.use_uid_fname() == true);
  CHECK(options_lp.annual_discount_rate() == doctest::Approx(0.07));
}

TEST_CASE("Options - Merge appends variable_scales")  // NOLINT
{
  using namespace gtopt;

  PlanningOptions base {};
  base.variable_scales.push_back({
      .class_name = "Bus",
      .variable = "theta",
      .scale = 1000.0,
  });

  PlanningOptions overlay {};
  overlay.variable_scales.push_back({
      .class_name = "Reservoir",
      .variable = "volume",
      .scale = 0.001,
  });
  overlay.variable_scales.push_back({
      .class_name = "Battery",
      .variable = "energy",
      .scale = 0.01,
  });

  base.merge(std::move(overlay));

  // Base had 1, overlay had 2 → merged should have 3
  REQUIRE(base.variable_scales.size() == 3);
  CHECK(base.variable_scales[0].class_name == "Bus");
  CHECK(base.variable_scales[1].class_name == "Reservoir");
  CHECK(base.variable_scales[2].class_name == "Battery");
}

TEST_CASE("Options - Merge with empty variable_scales does nothing")  // NOLINT
{
  using namespace gtopt;

  PlanningOptions base {};
  base.variable_scales.push_back({
      .class_name = "Bus",
      .variable = "theta",
      .scale = 1000.0,
  });

  PlanningOptions overlay {};

  base.merge(std::move(overlay));

  REQUIRE(base.variable_scales.size() == 1);
  CHECK(base.variable_scales[0].class_name == "Bus");
}

TEST_CASE("Options - Solver options fields have defaults")  // NOLINT
{
  using namespace gtopt;

  const PlanningOptions options {};
  CHECK(options.solver_options.algorithm == LPAlgo::barrier);
  CHECK(options.solver_options.threads == 2);
  CHECK(options.solver_options.presolve == true);
}

TEST_CASE("Options - Solver options fields construction")  // NOLINT
{
  using namespace gtopt;

  const PlanningOptions options {
      .solver_options =
          {
              .algorithm = LPAlgo::dual,
              .threads = 2,
              .presolve = false,
          },
  };

  CHECK(options.solver_options.algorithm == LPAlgo::dual);
  CHECK(options.solver_options.threads == 2);
  CHECK(options.solver_options.presolve == false);
}

TEST_CASE("Options - Solver options merge")  // NOLINT
{
  using namespace gtopt;

  PlanningOptions base {
      .solver_options =
          {
              .algorithm = LPAlgo::primal,
              .optimal_eps = 1e-8,
          },
  };
  PlanningOptions overlay {
      .solver_options =
          {
              .algorithm = LPAlgo::dual,
              .threads = 4,
              .optimal_eps = 1e-6,
              .feasible_eps = 1e-7,
          },
  };
  base.merge(std::move(overlay));

  // Non-optional fields keep base values (merge only touches optional fields)
  CHECK(base.solver_options.algorithm == LPAlgo::primal);
  CHECK(base.solver_options.threads == 2);
  CHECK(base.solver_options.presolve == true);
  // Optional field already set in base: keeps base value
  REQUIRE(base.solver_options.optimal_eps.has_value());
  CHECK(base.solver_options.optimal_eps.value_or(0.0) == doctest::Approx(1e-8));
  // Optional field not set in base: gains overlay value
  REQUIRE(base.solver_options.feasible_eps.has_value());
  CHECK(base.solver_options.feasible_eps.value_or(0.0)
        == doctest::Approx(1e-7));
}

TEST_CASE(
    "PlanningOptionsLP - Solver options accessor returns defaults")  // NOLINT
{
  using namespace gtopt;

  const PlanningOptionsLP options_lp {};
  CHECK(options_lp.solver_options().algorithm == LPAlgo::barrier);
  CHECK(options_lp.solver_options().threads == 2);
  CHECK(options_lp.solver_options().presolve == true);
}

TEST_CASE("Options - Solver options scaling default value")  // NOLINT
{
  using namespace gtopt;

  const SolverOptions opts {};
  // scaling defaults to automatic
  REQUIRE(opts.scaling.has_value());
  CHECK(*opts.scaling == SolverScaling::automatic);
}

TEST_CASE("Options - Solver options merge with scaling")  // NOLINT
{
  using namespace gtopt;

  // Base has default scaling (automatic), overlay has aggressive
  // merge_opt is last-value-wins: overlay replaces base
  PlanningOptions base {};
  PlanningOptions overlay {
      .solver_options = {.scaling = SolverScaling::aggressive},
  };
  base.merge(std::move(overlay));

  REQUIRE(base.solver_options.scaling.has_value());
  CHECK(*base.solver_options.scaling == SolverScaling::aggressive);

  // Overlay without scaling (nullopt) does NOT overwrite base
  PlanningOptions base2 {};
  PlanningOptions overlay2 {};
  overlay2.solver_options.scaling = std::nullopt;
  base2.merge(std::move(overlay2));

  // base2 had automatic, overlay2 had nullopt → base2 keeps automatic
  REQUIRE(base2.solver_options.scaling.has_value());
  CHECK(*base2.solver_options.scaling == SolverScaling::automatic);
}

TEST_CASE(
    "Options - LpBuildOptions row_equilibration merge OR logic")  // NOLINT
{
  using namespace gtopt;

  SUBCASE("false OR false = false")
  {
    PlanningOptions base {};
    PlanningOptions overlay {};
    base.merge(std::move(overlay));
    CHECK(base.lp_build_options.row_equilibration == false);
  }

  SUBCASE("true OR false = true")
  {
    PlanningOptions base {};
    base.lp_build_options.row_equilibration = true;
    PlanningOptions overlay {};
    base.merge(std::move(overlay));
    CHECK(base.lp_build_options.row_equilibration == true);
  }

  SUBCASE("false OR true = true")
  {
    PlanningOptions base {};
    PlanningOptions overlay {};
    overlay.lp_build_options.row_equilibration = true;
    base.merge(std::move(overlay));
    CHECK(base.lp_build_options.row_equilibration == true);
  }

  SUBCASE("true OR true = true")
  {
    PlanningOptions base {};
    base.lp_build_options.row_equilibration = true;
    PlanningOptions overlay {};
    overlay.lp_build_options.row_equilibration = true;
    base.merge(std::move(overlay));
    CHECK(base.lp_build_options.row_equilibration == true);
  }
}

TEST_CASE("Options - LpBuildOptions lp_coeff_ratio_threshold merge")  // NOLINT
{
  using namespace gtopt;

  // merge_opt is last-value-wins: overlay replaces base
  PlanningOptions base {};
  base.lp_build_options.lp_coeff_ratio_threshold = 1e7;
  PlanningOptions overlay {};
  overlay.lp_build_options.lp_coeff_ratio_threshold = 1e5;

  base.merge(std::move(overlay));

  REQUIRE(base.lp_build_options.lp_coeff_ratio_threshold.has_value());
  CHECK(base.lp_build_options.lp_coeff_ratio_threshold.value_or(0.0)
        == doctest::Approx(1e5));

  // Overlay without value does NOT overwrite base
  PlanningOptions base2 {};
  base2.lp_build_options.lp_coeff_ratio_threshold = 1e7;
  PlanningOptions overlay2 {};

  base2.merge(std::move(overlay2));

  REQUIRE(base2.lp_build_options.lp_coeff_ratio_threshold.has_value());
  CHECK(base2.lp_build_options.lp_coeff_ratio_threshold.value_or(0.0)
        == doctest::Approx(1e7));
}

TEST_CASE(
    "PlanningOptionsLP - Solver options accessors with set values")  // NOLINT
{
  using namespace gtopt;

  const PlanningOptions options {
      .solver_options =
          {
              .algorithm = LPAlgo::barrier,
              .threads = 4,
              .presolve = false,
          },
  };
  const PlanningOptionsLP options_lp {options};

  CHECK(options_lp.solver_options().algorithm == LPAlgo::barrier);
  CHECK(options_lp.solver_options().threads == 4);
  CHECK(options_lp.solver_options().presolve == false);
}
