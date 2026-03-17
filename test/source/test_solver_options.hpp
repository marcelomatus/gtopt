/**
 * @file      test_solver_options.hpp
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

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("SolverOptions - Default construction")
{
  // Test default construction of SolverOptions
  const SolverOptions options {};

  // Non-optional fields keep their defaults
  CHECK(options.algorithm == LPAlgo::barrier);
  CHECK(options.threads == 0);
  CHECK(options.presolve == true);
  CHECK(options.log_level == 0);

  // Tolerance fields are nullopt by default — solver uses its own defaults
  CHECK_FALSE(options.optimal_eps.has_value());
  CHECK_FALSE(options.feasible_eps.has_value());
  CHECK_FALSE(options.barrier_eps.has_value());
}

TEST_CASE("SolverOptions - Custom construction")
{
  // Test constructing SolverOptions with custom values
  const SolverOptions options {
      .algorithm = LPAlgo::barrier,
      .threads = 4,
      .presolve = false,
      .optimal_eps = 1e-6,
      .feasible_eps = 1e-5,
      .barrier_eps = 1e-7,
      .log_level = 2,
  };

  // Verify custom values
  CHECK(options.algorithm == LPAlgo::barrier);
  CHECK(options.threads == 4);
  CHECK(options.presolve == false);
  CHECK((options.optimal_eps && *options.optimal_eps == doctest::Approx(1e-6)));
  CHECK(
      (options.feasible_eps && *options.feasible_eps == doctest::Approx(1e-5)));
  CHECK((options.barrier_eps && *options.barrier_eps == doctest::Approx(1e-7)));
  CHECK(options.log_level == 2);
}

TEST_CASE("SolverOptions - LPAlgo enumeration values")
{
  // Test the LPAlgo enumeration values
  CHECK(std::to_underlying(LPAlgo::default_algo) == 0);
  CHECK(std::to_underlying(LPAlgo::primal) == 1);
  CHECK(std::to_underlying(LPAlgo::dual) == 2);
  CHECK(std::to_underlying(LPAlgo::barrier) == 3);
  CHECK(std::to_underlying(LPAlgo::last_algo) == 4);
}

TEST_CASE("SolverOptions - JSON serialization and deserialization")
{
  SUBCASE("with tolerance values")
  {
    // Create a SolverOptions object with non-default values
    const SolverOptions original {
        .algorithm = LPAlgo::primal,
        .threads = 2,
        .presolve = false,
        .optimal_eps = 1e-6,
        .feasible_eps = 1e-5,
        .barrier_eps = 1e-7,
        .log_level = 1,
    };

    // Serialize to JSON
    const auto json_string = daw::json::to_json(original);

    // Deserialize from JSON
    const auto deserialized = daw::json::from_json<SolverOptions>(json_string);

    // Verify deserialized values match original
    CHECK(deserialized.algorithm == original.algorithm);
    CHECK(deserialized.threads == original.threads);
    CHECK(deserialized.presolve == original.presolve);
    CHECK(deserialized.optimal_eps.value_or(-1.0)
          == doctest::Approx(original.optimal_eps.value_or(-1.0)));
    CHECK(deserialized.feasible_eps.value_or(-1.0)
          == doctest::Approx(original.feasible_eps.value_or(-1.0)));
    CHECK(deserialized.barrier_eps.value_or(-1.0)
          == doctest::Approx(original.barrier_eps.value_or(-1.0)));
    CHECK(deserialized.log_level == original.log_level);
  }

  SUBCASE("without tolerance values – nullopt round-trips as null")
  {
    // Default construction leaves tolerances as nullopt
    const SolverOptions original {};

    const auto json_string = daw::json::to_json(original);

    const auto deserialized = daw::json::from_json<SolverOptions>(json_string);

    CHECK_FALSE(deserialized.optimal_eps.has_value());
    CHECK_FALSE(deserialized.feasible_eps.has_value());
    CHECK_FALSE(deserialized.barrier_eps.has_value());
  }
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
  const SolverOptions solver_options {
      .algorithm = LPAlgo::primal,
      .presolve = true,
      .optimal_eps = 1e-6,
      .feasible_eps = 1e-5,
  };

  // Solve with custom options
  const auto result = lp.initial_solve(solver_options);

  // Check that the solve worked
  CHECK(result);
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

  SUBCASE(
      "Default – tolerances are nullopt (solver uses its built-in defaults)")
  {
    const SolverOptions options {};
    CHECK_FALSE(options.optimal_eps.has_value());
    CHECK_FALSE(options.feasible_eps.has_value());
    CHECK_FALSE(options.barrier_eps.has_value());
  }

  SUBCASE("Custom tolerances")
  {
    SolverOptions options;
    options.optimal_eps = 1e-8;
    options.feasible_eps = 1e-7;
    options.barrier_eps = 1e-6;

    CHECK(options.optimal_eps.value_or(0.0) == doctest::Approx(1e-8));
    CHECK(options.feasible_eps.value_or(0.0) == doctest::Approx(1e-7));
    CHECK(options.barrier_eps.value_or(0.0) == doctest::Approx(1e-6));
  }

  SUBCASE("Realistic tolerances")
  {
    SolverOptions options;
    options.optimal_eps = 1e-6;  // Typical optimality tolerance
    options.feasible_eps = 1e-6;  // Typical feasibility tolerance
    options.barrier_eps = 1e-8;  // Typical barrier convergence tolerance

    CHECK(options.optimal_eps.value_or(0.0) == doctest::Approx(1e-6));
    CHECK(options.feasible_eps.value_or(0.0) == doctest::Approx(1e-6));
    CHECK(options.barrier_eps.value_or(0.0) == doctest::Approx(1e-8));
  }
}

TEST_CASE("SolverOptions - merge() only applies to optional tolerance fields")
{
  SUBCASE("merge sets nullopt tolerance from non-null source")
  {
    SolverOptions dest {};
    const SolverOptions src {
        .optimal_eps = 1e-8,
        .feasible_eps = 1e-7,
    };
    dest.merge(src);

    CHECK(dest.optimal_eps.value_or(0.0) == doctest::Approx(1e-8));
    CHECK(dest.feasible_eps.value_or(0.0) == doctest::Approx(1e-7));
    CHECK_FALSE(dest.barrier_eps.has_value());
  }

  SUBCASE("merge first-wins: existing value is not overwritten")
  {
    SolverOptions dest {
        .optimal_eps = 1e-6,
    };
    const SolverOptions src {
        .optimal_eps = 1e-10,
    };
    dest.merge(src);

    // First-file value should win
    CHECK(dest.optimal_eps.value_or(0.0) == doctest::Approx(1e-6));
  }

  SUBCASE("merge leaves both nullopt when neither is set")
  {
    SolverOptions dest {};
    const SolverOptions src {};
    dest.merge(src);

    CHECK_FALSE(dest.optimal_eps.has_value());
    CHECK_FALSE(dest.feasible_eps.has_value());
    CHECK_FALSE(dest.barrier_eps.has_value());
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
    const SolverOptions options;
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
    const SolverOptions options;
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

TEST_CASE("SolverOptions - Algorithm selection with dual simplex")  // NOLINT
{
  // Verify that selecting the dual simplex algorithm still yields the correct
  // solution on the same simple 1x1 problem.
  FlatLinearProblem flat_lp;
  flat_lp.name = "dual_algo_test";
  flat_lp.ncols = 1;
  flat_lp.nrows = 1;
  flat_lp.matbeg = {0, 1};
  flat_lp.matind = {0};
  flat_lp.matval = {1.0};
  flat_lp.collb = {2.0};
  flat_lp.colub = {10.0};
  flat_lp.objval = {1.0};
  flat_lp.rowlb = {2.0};
  flat_lp.rowub = {10.0};
  flat_lp.colnm = {"x"};
  flat_lp.rownm = {"r1"};

  LinearInterface lp(flat_lp);

  const SolverOptions solver_options {
      .algorithm = LPAlgo::dual,
  };

  const auto result = lp.initial_solve(solver_options);

  CHECK(result);
  CHECK(lp.is_optimal() == true);
  CHECK(lp.get_obj_value() == doctest::Approx(2.0));
  const auto sol = lp.get_col_sol();
  REQUIRE(sol.size() == 1);
  CHECK(sol[0] == doctest::Approx(2.0));
}

TEST_CASE("SolverOptions - Algorithm selection with primal simplex")  // NOLINT
{
  FlatLinearProblem flat_lp;
  flat_lp.name = "primal_algo_test";
  flat_lp.ncols = 1;
  flat_lp.nrows = 1;
  flat_lp.matbeg = {0, 1};
  flat_lp.matind = {0};
  flat_lp.matval = {1.0};
  flat_lp.collb = {3.0};
  flat_lp.colub = {10.0};
  flat_lp.objval = {1.0};
  flat_lp.rowlb = {3.0};
  flat_lp.rowub = {10.0};
  flat_lp.colnm = {"x"};
  flat_lp.rownm = {"r1"};

  LinearInterface lp(flat_lp);

  const SolverOptions solver_options {
      .algorithm = LPAlgo::primal,
  };

  const auto result = lp.initial_solve(solver_options);

  CHECK(result);
  CHECK(lp.is_optimal() == true);
  CHECK(lp.get_obj_value() == doctest::Approx(3.0));
  const auto sol = lp.get_col_sol();
  REQUIRE(sol.size() == 1);
  CHECK(sol[0] == doctest::Approx(3.0));
}

TEST_CASE("SolverOptions - All algorithms solve correctly on 2x2 LP")  // NOLINT
{
  // Verify all available algorithms solve:
  //   min  x + y
  //   s.t. x + y >= 4
  //        x, y >= 0
  // Optimal solution: any (x,y) with x+y=4, obj=4.

  auto make_lp = []() -> LinearInterface
  {
    FlatLinearProblem flat_lp;
    flat_lp.name = "algo_test_2x2";
    flat_lp.ncols = 2;
    flat_lp.nrows = 1;
    flat_lp.matbeg = {0, 1, 2};
    flat_lp.matind = {0, 0};
    flat_lp.matval = {1.0, 1.0};
    flat_lp.collb = {0.0, 0.0};
    flat_lp.colub = {1e30, 1e30};
    flat_lp.objval = {1.0, 1.0};
    flat_lp.rowlb = {4.0};
    flat_lp.rowub = {1e30};
    flat_lp.colnm = {"x", "y"};
    flat_lp.rownm = {"sum_row"};
    return LinearInterface(flat_lp);
  };

  SUBCASE("default algorithm")
  {
    auto lp = make_lp();
    const auto result =
        lp.initial_solve(SolverOptions {.algorithm = LPAlgo::default_algo});
    CHECK(result);
    CHECK(lp.get_obj_value() == doctest::Approx(4.0));
  }

  SUBCASE("primal simplex")
  {
    auto lp = make_lp();
    const auto result =
        lp.initial_solve(SolverOptions {.algorithm = LPAlgo::primal});
    CHECK(result);
    CHECK(lp.get_obj_value() == doctest::Approx(4.0));
  }

  SUBCASE("dual simplex")
  {
    auto lp = make_lp();
    const auto result =
        lp.initial_solve(SolverOptions {.algorithm = LPAlgo::dual});
    CHECK(result);
    CHECK(lp.get_obj_value() == doctest::Approx(4.0));
  }
}

TEST_CASE("SolverOptions - Custom construction")
{
  // Test constructing SolverOptions with custom values
  const SolverOptions options {
      .algorithm = LPAlgo::barrier,
      .threads = 4,
      .presolve = false,
      .optimal_eps = 1e-6,
      .feasible_eps = 1e-5,
      .barrier_eps = 1e-7,
      .log_level = 2,
  };

  // Verify custom values
  CHECK(options.algorithm == LPAlgo::barrier);
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
  CHECK(std::to_underlying(LPAlgo::default_algo) == 0);
  CHECK(std::to_underlying(LPAlgo::primal) == 1);
  CHECK(std::to_underlying(LPAlgo::dual) == 2);
  CHECK(std::to_underlying(LPAlgo::barrier) == 3);
  CHECK(std::to_underlying(LPAlgo::last_algo) == 4);
}

TEST_CASE("SolverOptions - JSON serialization and deserialization")
{
  // Create a SolverOptions object with non-default values
  const SolverOptions original {
      .algorithm = LPAlgo::primal,
      .threads = 2,
      .presolve = false,
      .optimal_eps = 1e-6,
      .feasible_eps = 1e-5,
      .barrier_eps = 1e-7,
      .log_level = 1,
  };

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
  const SolverOptions solver_options {
      .algorithm = LPAlgo::primal,
      .presolve = true,
      .optimal_eps = 1e-6,
      .feasible_eps = 1e-5,
  };

  // Solve with custom options
  const auto result = lp.initial_solve(solver_options);

  // Check that the solve worked
  CHECK(result);
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
    const SolverOptions options;
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
    const SolverOptions options;
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

TEST_CASE("SolverOptions - Algorithm selection with dual simplex")  // NOLINT
{
  // Verify that selecting the dual simplex algorithm still yields the correct
  // solution on the same simple 1x1 problem.
  FlatLinearProblem flat_lp;
  flat_lp.name = "dual_algo_test";
  flat_lp.ncols = 1;
  flat_lp.nrows = 1;
  flat_lp.matbeg = {0, 1};
  flat_lp.matind = {0};
  flat_lp.matval = {1.0};
  flat_lp.collb = {2.0};
  flat_lp.colub = {10.0};
  flat_lp.objval = {1.0};
  flat_lp.rowlb = {2.0};
  flat_lp.rowub = {10.0};
  flat_lp.colnm = {"x"};
  flat_lp.rownm = {"r1"};

  LinearInterface lp(flat_lp);

  const SolverOptions solver_options {
      .algorithm = LPAlgo::dual,
  };

  const auto result = lp.initial_solve(solver_options);

  CHECK(result);
  CHECK(lp.is_optimal() == true);
  CHECK(lp.get_obj_value() == doctest::Approx(2.0));
  const auto sol = lp.get_col_sol();
  REQUIRE(sol.size() == 1);
  CHECK(sol[0] == doctest::Approx(2.0));
}

TEST_CASE("SolverOptions - Algorithm selection with primal simplex")  // NOLINT
{
  FlatLinearProblem flat_lp;
  flat_lp.name = "primal_algo_test";
  flat_lp.ncols = 1;
  flat_lp.nrows = 1;
  flat_lp.matbeg = {0, 1};
  flat_lp.matind = {0};
  flat_lp.matval = {1.0};
  flat_lp.collb = {3.0};
  flat_lp.colub = {10.0};
  flat_lp.objval = {1.0};
  flat_lp.rowlb = {3.0};
  flat_lp.rowub = {10.0};
  flat_lp.colnm = {"x"};
  flat_lp.rownm = {"r1"};

  LinearInterface lp(flat_lp);

  const SolverOptions solver_options {
      .algorithm = LPAlgo::primal,
  };

  const auto result = lp.initial_solve(solver_options);

  CHECK(result);
  CHECK(lp.is_optimal() == true);
  CHECK(lp.get_obj_value() == doctest::Approx(3.0));
  const auto sol = lp.get_col_sol();
  REQUIRE(sol.size() == 1);
  CHECK(sol[0] == doctest::Approx(3.0));
}

TEST_CASE("SolverOptions - All algorithms solve correctly on 2x2 LP")  // NOLINT
{
  // Verify all available algorithms solve:
  //   min  x + y
  //   s.t. x + y >= 4
  //        x, y >= 0
  // Optimal solution: any (x,y) with x+y=4, obj=4.

  auto make_lp = []() -> LinearInterface
  {
    FlatLinearProblem flat_lp;
    flat_lp.name = "algo_test_2x2";
    flat_lp.ncols = 2;
    flat_lp.nrows = 1;
    flat_lp.matbeg = {0, 1, 2};
    flat_lp.matind = {0, 0};
    flat_lp.matval = {1.0, 1.0};
    flat_lp.collb = {0.0, 0.0};
    flat_lp.colub = {1e30, 1e30};
    flat_lp.objval = {1.0, 1.0};
    flat_lp.rowlb = {4.0};
    flat_lp.rowub = {1e30};
    flat_lp.colnm = {"x", "y"};
    flat_lp.rownm = {"sum_row"};
    return LinearInterface(flat_lp);
  };

  SUBCASE("default algorithm")
  {
    auto lp = make_lp();
    const auto result =
        lp.initial_solve(SolverOptions {.algorithm = LPAlgo::default_algo});
    CHECK(result);
    CHECK(lp.get_obj_value() == doctest::Approx(4.0));
  }

  SUBCASE("primal simplex")
  {
    auto lp = make_lp();
    const auto result =
        lp.initial_solve(SolverOptions {.algorithm = LPAlgo::primal});
    CHECK(result);
    CHECK(lp.get_obj_value() == doctest::Approx(4.0));
  }

  SUBCASE("dual simplex")
  {
    auto lp = make_lp();
    const auto result =
        lp.initial_solve(SolverOptions {.algorithm = LPAlgo::dual});
    CHECK(result);
    CHECK(lp.get_obj_value() == doctest::Approx(4.0));
  }
}
