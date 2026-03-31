/**
 * @file      test_solver_options_json.hpp
 * @brief     JSON serialization tests for SolverOptions
 * @date      2026-03-18
 * @copyright BSD-3-Clause
 */

#pragma once

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_solver_options.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("SolverOptions JSON basic parsing")
{
  const std::string_view json_data = R"({
    "algorithm": 3,
    "threads": 4,
    "presolve": true,
    "log_level": 1,
    "reuse_basis": false
  })";

  const SolverOptions opts = daw::json::from_json<SolverOptions>(json_data);

  CHECK(opts.algorithm == LPAlgo::barrier);
  CHECK(opts.threads == 4);
  CHECK(opts.presolve == true);
  CHECK(opts.log_level == 1);
  CHECK_FALSE(opts.optimal_eps.has_value());
  CHECK_FALSE(opts.feasible_eps.has_value());
  CHECK_FALSE(opts.barrier_eps.has_value());
}

TEST_CASE("SolverOptions JSON with tolerances")
{
  const std::string_view json_data = R"({
    "algorithm": 2,
    "threads": 0,
    "presolve": false,
    "optimal_eps": 1e-8,
    "feasible_eps": 1e-7,
    "barrier_eps": 1e-10,
    "log_level": 0,
    "reuse_basis": false
  })";

  const SolverOptions opts = daw::json::from_json<SolverOptions>(json_data);

  CHECK(opts.algorithm == LPAlgo::dual);
  CHECK(opts.threads == 0);
  CHECK(opts.presolve == false);
  REQUIRE(opts.optimal_eps.has_value());
  CHECK(opts.optimal_eps.value_or(0.0) == doctest::Approx(1e-8));
  REQUIRE(opts.feasible_eps.has_value());
  CHECK(opts.feasible_eps.value_or(0.0) == doctest::Approx(1e-7));
  REQUIRE(opts.barrier_eps.has_value());
  CHECK(opts.barrier_eps.value_or(0.0) == doctest::Approx(1e-10));
}

TEST_CASE("SolverOptions JSON round-trip serialization")
{
  SolverOptions original;
  original.algorithm = LPAlgo::primal;
  original.threads = 8;
  original.presolve = true;
  original.optimal_eps = 1e-6;
  original.log_level = 2;

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const SolverOptions roundtrip = daw::json::from_json<SolverOptions>(json);
  CHECK(roundtrip.algorithm == LPAlgo::primal);
  CHECK(roundtrip.threads == 8);
  CHECK(roundtrip.presolve == true);
  REQUIRE(roundtrip.optimal_eps.has_value());
  CHECK(roundtrip.optimal_eps.value_or(0.0) == doctest::Approx(1e-6));
  CHECK(roundtrip.log_level == 2);
}

TEST_CASE("SolverOptions JSON empty object uses defaults")
{
  const std::string_view json_data = R"({})";

  const SolverOptions opts = daw::json::from_json<SolverOptions>(json_data);

  CHECK(opts.algorithm == LPAlgo::barrier);
  CHECK(opts.threads == 2);
  CHECK(opts.presolve == true);
  CHECK(opts.log_level == 0);
  CHECK(opts.reuse_basis == false);
  CHECK_FALSE(opts.optimal_eps.has_value());
  CHECK_FALSE(opts.feasible_eps.has_value());
  CHECK_FALSE(opts.barrier_eps.has_value());
  CHECK_FALSE(opts.log_mode.has_value());
  CHECK_FALSE(opts.time_limit.has_value());
  // scaling defaults to automatic when not specified in JSON
  REQUIRE(opts.scaling.has_value());
  CHECK(*opts.scaling == SolverScaling::automatic);
}

TEST_CASE("SolverOptions JSON scaling field round-trip")
{
  // Construct with explicit scaling
  SolverOptions original;
  original.scaling = SolverScaling::aggressive;

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const SolverOptions roundtrip = daw::json::from_json<SolverOptions>(json);
  REQUIRE(roundtrip.scaling.has_value());
  CHECK(*roundtrip.scaling == SolverScaling::aggressive);

  // Construct with nullopt scaling
  SolverOptions no_scale;
  no_scale.scaling = std::nullopt;

  const auto json2 = daw::json::to_json(no_scale);
  const SolverOptions rt2 = daw::json::from_json<SolverOptions>(json2);
  // JSON round-trip: nullopt serializes as null → constructor applies default
  REQUIRE(rt2.scaling.has_value());
  CHECK(*rt2.scaling == SolverScaling::automatic);
}

TEST_CASE("SolverOptions JSON scaling explicit values")
{
  // scaling=0 → none
  const std::string_view json_none = R"({"scaling": 0})";
  const SolverOptions opts_none =
      daw::json::from_json<SolverOptions>(json_none);
  REQUIRE(opts_none.scaling.has_value());
  CHECK(*opts_none.scaling == SolverScaling::none);

  // scaling=1 → automatic
  const std::string_view json_auto = R"({"scaling": 1})";
  const SolverOptions opts_auto =
      daw::json::from_json<SolverOptions>(json_auto);
  REQUIRE(opts_auto.scaling.has_value());
  CHECK(*opts_auto.scaling == SolverScaling::automatic);

  // scaling=2 → aggressive
  const std::string_view json_agg = R"({"scaling": 2})";
  const SolverOptions opts_agg = daw::json::from_json<SolverOptions>(json_agg);
  REQUIRE(opts_agg.scaling.has_value());
  CHECK(*opts_agg.scaling == SolverScaling::aggressive);
}

TEST_CASE("SolverOptions JSON partial object uses defaults for missing")
{
  const std::string_view json_data = R"({
    "algorithm": 1,
    "threads": 16
  })";

  const SolverOptions opts = daw::json::from_json<SolverOptions>(json_data);

  CHECK(opts.algorithm == LPAlgo::primal);
  CHECK(opts.threads == 16);
  CHECK(opts.presolve == true);
  CHECK(opts.log_level == 0);
  CHECK(opts.reuse_basis == false);
}

TEST_CASE("SolverOptions JSON max_fallbacks parsing and round-trip")
{
  SUBCASE("default when missing is 2")
  {
    const std::string_view json_data = R"({})";
    const SolverOptions opts = daw::json::from_json<SolverOptions>(json_data);
    CHECK(opts.max_fallbacks == 2);
  }

  SUBCASE("explicit zero")
  {
    const std::string_view json_data = R"({"max_fallbacks": 0})";
    const SolverOptions opts = daw::json::from_json<SolverOptions>(json_data);
    CHECK(opts.max_fallbacks == 0);
  }

  SUBCASE("explicit value")
  {
    const std::string_view json_data = R"({"max_fallbacks": 1})";
    const SolverOptions opts = daw::json::from_json<SolverOptions>(json_data);
    CHECK(opts.max_fallbacks == 1);
  }

  SUBCASE("round-trip")
  {
    SolverOptions original;
    original.max_fallbacks = 1;

    const auto json = daw::json::to_json(original);
    const SolverOptions roundtrip = daw::json::from_json<SolverOptions>(json);
    CHECK(roundtrip.max_fallbacks == 1);
  }
}
