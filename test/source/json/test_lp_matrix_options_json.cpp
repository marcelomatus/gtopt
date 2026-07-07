/**
 * @file      test_lp_matrix_options_json.hpp
 * @brief     JSON serialization tests for LpMatrixOptions
 * @date      2026-03-25
 * @copyright BSD-3-Clause
 */

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_lp_matrix_options.hpp>

using namespace gtopt;
// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_CASE("LpMatrixOptions JSON - Full deserialization")
{
  const std::string_view json_data = R"({
    "lp_coeff_ratio_threshold": 1e8
  })";

  const auto opts = daw::json::from_json<LpMatrixOptions>(json_data);

  REQUIRE(opts.lp_coeff_ratio_threshold.has_value());
  CHECK(*opts.lp_coeff_ratio_threshold == doctest::Approx(1e8));
}

TEST_CASE("LpMatrixOptions JSON - Missing fields keep defaults")
{
  const std::string_view json_data = R"({})";
  const auto opts = daw::json::from_json<LpMatrixOptions>(json_data);

  CHECK_FALSE(opts.lp_coeff_ratio_threshold.has_value());
  // Non-JSON fields keep their struct defaults
  CHECK(opts.eps == doctest::Approx(0.0));
  CHECK(opts.col_with_names == false);
  CHECK(opts.row_with_names == false);
  CHECK_FALSE(opts.compute_stats.has_value());
  // Ruiz tuning knobs: omitted JSON keeps the struct defaults (5 / 1e-3).
  CHECK(opts.ruiz_max_iterations == 5);
  CHECK(opts.ruiz_tolerance == doctest::Approx(1e-3));
}

TEST_CASE("LpMatrixOptions JSON - Ruiz tuning knobs deserialize")
{
  const std::string_view json_data = R"({
    "equilibration_method": "ruiz",
    "ruiz_max_iterations": 3,
    "ruiz_tolerance": 1e-2
  })";
  const auto opts = daw::json::from_json<LpMatrixOptions>(json_data);

  REQUIRE(opts.equilibration_method.has_value());
  CHECK(*opts.equilibration_method == LpEquilibrationMethod::ruiz);
  CHECK(opts.ruiz_max_iterations == 3);
  CHECK(opts.ruiz_tolerance == doctest::Approx(1e-2));
}

TEST_CASE("LpMatrixOptions JSON - Round-trip serialization")
{
  LpMatrixOptions original;
  original.lp_coeff_ratio_threshold = 5e6;
  original.ruiz_max_iterations = 4;
  original.ruiz_tolerance = 5e-4;

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const auto rt = daw::json::from_json<LpMatrixOptions>(json);

  REQUIRE(rt.lp_coeff_ratio_threshold.has_value());
  CHECK(*rt.lp_coeff_ratio_threshold == doctest::Approx(5e6));
  // Non-optional ruiz knobs are always serialized → survive the round-trip.
  CHECK(rt.ruiz_max_iterations == 4);
  CHECK(rt.ruiz_tolerance == doctest::Approx(5e-4));
}

// NOLINTEND(bugprone-unchecked-optional-access)