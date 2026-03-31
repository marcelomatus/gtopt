/**
 * @file      test_lp_build_options_json.hpp
 * @brief     JSON serialization tests for LpBuildOptions
 * @date      2026-03-25
 * @copyright BSD-3-Clause
 */

#pragma once

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_lp_build_options.hpp>

TEST_CASE("LpBuildOptions JSON - Full deserialization")
{
  const std::string_view json_data = R"({
    "names_level": 2,
    "lp_coeff_ratio_threshold": 1e8
  })";

  const auto opts = daw::json::from_json<LpBuildOptions>(json_data);

  REQUIRE(opts.names_level.has_value());
  CHECK(*opts.names_level == LpNamesLevel::cols_and_rows);
  REQUIRE(opts.lp_coeff_ratio_threshold.has_value());
  CHECK(*opts.lp_coeff_ratio_threshold == doctest::Approx(1e8));
}

TEST_CASE("LpBuildOptions JSON - names_level values")
{
  SUBCASE("minimal (0)")
  {
    const std::string_view json_data = R"({"names_level": 0})";
    const auto opts = daw::json::from_json<LpBuildOptions>(json_data);
    REQUIRE(opts.names_level.has_value());
    CHECK(*opts.names_level == LpNamesLevel::minimal);
  }

  SUBCASE("only_cols (1)")
  {
    const std::string_view json_data = R"({"names_level": 1})";
    const auto opts = daw::json::from_json<LpBuildOptions>(json_data);
    REQUIRE(opts.names_level.has_value());
    CHECK(*opts.names_level == LpNamesLevel::only_cols);
  }

  SUBCASE("cols_and_rows (2)")
  {
    const std::string_view json_data = R"({"names_level": 2})";
    const auto opts = daw::json::from_json<LpBuildOptions>(json_data);
    REQUIRE(opts.names_level.has_value());
    CHECK(*opts.names_level == LpNamesLevel::cols_and_rows);
  }
}

TEST_CASE("LpBuildOptions JSON - Missing fields keep defaults")
{
  const std::string_view json_data = R"({})";
  const auto opts = daw::json::from_json<LpBuildOptions>(json_data);

  CHECK_FALSE(opts.names_level.has_value());
  CHECK_FALSE(opts.lp_coeff_ratio_threshold.has_value());
  // Non-JSON fields keep their struct defaults
  CHECK(opts.eps == doctest::Approx(0.0));
  CHECK(opts.col_with_names == true);
  CHECK(opts.row_with_names == false);
  CHECK_FALSE(opts.compute_stats.has_value());
}

TEST_CASE("LpBuildOptions JSON - Round-trip serialization")
{
  LpBuildOptions original;
  original.names_level = LpNamesLevel::only_cols;
  original.lp_coeff_ratio_threshold = 5e6;

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const auto rt = daw::json::from_json<LpBuildOptions>(json);

  REQUIRE(rt.names_level.has_value());
  CHECK(*rt.names_level == LpNamesLevel::only_cols);
  REQUIRE(rt.lp_coeff_ratio_threshold.has_value());
  CHECK(*rt.lp_coeff_ratio_threshold == doctest::Approx(5e6));
}
