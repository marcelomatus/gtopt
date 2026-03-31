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
