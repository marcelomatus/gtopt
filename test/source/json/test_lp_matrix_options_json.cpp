/**
 * @file      test_lp_matrix_options_json.hpp
 * @brief     JSON serialization tests for LpMatrixOptions
 * @date      2026-03-25
 * @copyright BSD-3-Clause
 */

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_lp_matrix_options.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("LpMatrixOptions JSON - Full deserialization")
{
  const std::string_view json_data = R"({
    "names_level": 2,
    "lp_coeff_ratio_threshold": 1e8
  })";

  const auto opts = daw::json::from_json<LpMatrixOptions>(json_data);

  REQUIRE(opts.names_level.has_value());
  CHECK(*opts.names_level == LpNamesLevel::cols_and_rows);
  REQUIRE(opts.lp_coeff_ratio_threshold.has_value());
  CHECK(*opts.lp_coeff_ratio_threshold == doctest::Approx(1e8));
}

TEST_CASE("LpMatrixOptions JSON - names_level values")
{
  SUBCASE("only_cols (1)")
  {
    const std::string_view json_data = R"({"names_level": 1})";
    const auto opts = daw::json::from_json<LpMatrixOptions>(json_data);
    REQUIRE(opts.names_level.has_value());
    CHECK(*opts.names_level == LpNamesLevel::only_cols);
  }

  SUBCASE("cols_and_rows (2)")
  {
    const std::string_view json_data = R"({"names_level": 2})";
    const auto opts = daw::json::from_json<LpMatrixOptions>(json_data);
    REQUIRE(opts.names_level.has_value());
    CHECK(*opts.names_level == LpNamesLevel::cols_and_rows);
  }
}

TEST_CASE("LpMatrixOptions JSON - Missing fields keep defaults")
{
  const std::string_view json_data = R"({})";
  const auto opts = daw::json::from_json<LpMatrixOptions>(json_data);

  CHECK_FALSE(opts.names_level.has_value());
  CHECK_FALSE(opts.lp_coeff_ratio_threshold.has_value());
  // Non-JSON fields keep their struct defaults
  CHECK(opts.eps == doctest::Approx(0.0));
  CHECK(opts.col_with_names == false);
  CHECK(opts.row_with_names == false);
  CHECK_FALSE(opts.compute_stats.has_value());
}

TEST_CASE("LpMatrixOptions JSON - Round-trip serialization")
{
  LpMatrixOptions original;
  original.names_level = LpNamesLevel::only_cols;
  original.lp_coeff_ratio_threshold = 5e6;

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const auto rt = daw::json::from_json<LpMatrixOptions>(json);

  REQUIRE(rt.names_level.has_value());
  CHECK(*rt.names_level == LpNamesLevel::only_cols);
  REQUIRE(rt.lp_coeff_ratio_threshold.has_value());
  CHECK(*rt.lp_coeff_ratio_threshold == doctest::Approx(5e6));
}
