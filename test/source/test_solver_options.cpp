/**
 * @file      test_solver_options.cpp
 * @brief     Unit tests for the SolverOptions class
 * @date      Sun May  5 11:30:00 2025
 * @author    Claude
 * @copyright BSD-3-Clause
 *
 * This module contains the unit tests for the SolverOptions class and related
 * functionality.
 */

#include <doctest/doctest.h>
#include <gtopt/json/json_solver_options.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/solver_options.hpp>

using namespace gtopt;

TEST_CASE("SolverOptions - Default construction")
{
  // Test default construction of SolverOptions
  const SolverOptions options {};

  // Verify default values
  CHECK(options.algorithm == static_cast<int>(LPAlgo::default_algo));
  CHECK(options.threads == 0);
  CHECK(options.presolve == true);  // Only non-zero default
  CHECK(options.optimal_eps == 1.0e-6);
  CHECK(options.feasible_eps == 1.0e-6);
  CHECK(options.barrier_eps == 1.0e-6);
  CHECK(options.log_level == 0);
}

TEST_CASE("SolverOptions - Custom construction")
{
  // Test constructing SolverOptions with custom values
  const SolverOptions options {.algorithm = static_cast<int>(LPAlgo::barrier),
                               .threads = 4,
                               .presolve = false,
                               .optimal_eps = 1e-6,
                               .feasible_eps = 1e-5,
                               .barrier_eps = 1e-7,
                               .log_level = 2};

  // Verify custom values
  CHECK(options.algorithm == static_cast<int>(LPAlgo::barrier));
  CHECK(options.threads == 4);
  CHECK(options.presolve == false);
  CHECK(options.optimal_eps == doctest::Approx(1e-6));
  CHECK(options.feasible_eps == doctest::Approx(1e-5));
  CHECK(options.barrier_eps == doctest::Approx(1e-7));
  CHECK(options.log_level == 2);
}

TEST_CASE("SolverOptions - LPAlgo enumeration values")
{
  // Test the LPAlgo enumeration values
  CHECK(static_cast<uint8_t>(LPAlgo::default_algo) == 0);
  CHECK(static_cast<uint8_t>(LPAlgo::primal) == 1);
  CHECK(static_cast<uint8_t>(LPAlgo::dual) == 2);
  CHECK(static_cast<uint8_t>(LPAlgo::barrier) == 3);
  CHECK(static_cast<uint8_t>(LPAlgo::last_algo) == 4);
}

TEST_CASE("SolverOptions - JSON serialization and deserialization")
{
  // Create a SolverOptions object with non-default values
  const SolverOptions original {.algorithm = static_cast<int>(LPAlgo::primal),
                                .threads = 2,
                                .presolve = false,
                                .optimal_eps = 1e-6,
                                .feasible_eps = 1e-5,
                                .barrier_eps = 1e-7,
                                .log_level = 1};

  // Serialize to JSON
  const auto json_string = daw::json::to_json(original);

  // Deserialize from JSON
  const auto deserialized = daw::json::from_json<SolverOptions>(json_string);

  // Verify deserialized values match original
  CHECK(deserialized.algorithm == original.algorithm);
  CHECK(deserialized.threads == original.threads);
  CHECK(deserialized.presolve == original.presolve);
  CHECK(deserialized.optimal_eps == original.optimal_eps);
  CHECK(deserialized.feasible_eps == original.feasible_eps);
  CHECK(deserialized.barrier_eps == original.barrier_eps);
  CHECK(deserialized.log_level == original.log_level);
}

TEST_CASE("SolverOptions - Usage with LinearInterface")
{
  // Create a minimal linear problem for testing
  FlatLinearProblem flat_lp;
  flat_lp.name = "test_problem";
  // Setup a simple 1x1 LP problem: min x s.t. x >= 1
  flat_lp.ncols = 1;
  flat_lp.nrows = 1;
  flat_lp.matbeg = {0, 1};  // Column start indices
  flat_lp.matind = {0};  // Row indices
  flat_lp.matval = {1.0};  // Matrix coefficients
  flat_lp.collb = {1.0};  // Column lower bounds
  flat_lp.colub = {10.0};  // Column upper bounds
  flat_lp.objval = {1.0};  // Objective coefficients
  flat_lp.rowlb = {1.0};  // Row lower bounds
  flat_lp.rowub = {10.0};  // Row upper bounds
  flat_lp.colnm = {"x"};  // Column names
  flat_lp.rownm = {"r1"};  // Row names

  // Create LinearInterface with default options
  LinearInterface lp(flat_lp);

  // Create solver options with custom values
  SolverOptions solver_options {.algorithm = static_cast<int>(LPAlgo::primal),
                                .presolve = true,
                                .optimal_eps = 1e-6,
                                .feasible_eps = 1e-5};

  // Solve with custom options
  const bool result = lp.initial_solve(solver_options);

  // Check that the solve worked
  CHECK(result == true);
  CHECK(lp.is_optimal() == true);
  CHECK(lp.get_obj_value() == doctest::Approx(1.0));

  // Get solution and check it
  const auto sol = lp.get_col_sol();
  REQUIRE(sol.size() == 1);
  CHECK(sol[0] == doctest::Approx(1.0));
}

TEST_CASE("SolverOptions - Numerical parameters")
{
  // Test with different numerical parameter settings

  SUBCASE("Zero tolerances")
  {
    SolverOptions options;
    options.optimal_eps = 0.0;
    options.feasible_eps = 0.0;
    options.barrier_eps = 0.0;

    CHECK(options.optimal_eps == 0.0);
    CHECK(options.feasible_eps == 0.0);
    CHECK(options.barrier_eps == 0.0);
  }

  SUBCASE("Custom tolerances")
  {
    SolverOptions options;
    options.optimal_eps = 1e-8;
    options.feasible_eps = 1e-7;
    options.barrier_eps = 1e-6;

    CHECK(options.optimal_eps == doctest::Approx(1e-8));
    CHECK(options.feasible_eps == doctest::Approx(1e-7));
    CHECK(options.barrier_eps == doctest::Approx(1e-6));
  }

  SUBCASE("Realistic tolerances")
  {
    SolverOptions options;
    options.optimal_eps = 1e-6;  // Typical optimality tolerance
    options.feasible_eps = 1e-6;  // Typical feasibility tolerance
    options.barrier_eps = 1e-8;  // Typical barrier convergence tolerance

    CHECK(options.optimal_eps == doctest::Approx(1e-6));
    CHECK(options.feasible_eps == doctest::Approx(1e-6));
    CHECK(options.barrier_eps == doctest::Approx(1e-8));
  }
}

TEST_CASE("SolverOptions - Threading options")
{
  // Test with different threading options

  SUBCASE("Default threading (auto)")
  {
    SolverOptions options;
    options.threads = 0;  // 0 means automatic selection

    CHECK(options.threads == 0);
  }

  SUBCASE("Single-threaded")
  {
    SolverOptions options;
    options.threads = 1;

    CHECK(options.threads == 1);
  }

  SUBCASE("Multi-threaded")
  {
    SolverOptions options;
    options.threads = 4;

    CHECK(options.threads == 4);
  }
}

TEST_CASE("SolverOptions - Presolve and logging options")
{
  // Test presolve and logging options

  SUBCASE("Default presolve (enabled)")
  {
    SolverOptions options;
    // Default is true

    CHECK(options.presolve == true);
  }

  SUBCASE("Disable presolve")
  {
    SolverOptions options;
    options.presolve = false;

    CHECK(options.presolve == false);
  }

  SUBCASE("Default logging (silent)")
  {
    SolverOptions options;
    // Default is 0

    CHECK(options.log_level == 0);
  }

  SUBCASE("Verbose logging")
  {
    SolverOptions options;
    options.log_level = 3;  // Highly verbose

    CHECK(options.log_level == 3);
  }
}
