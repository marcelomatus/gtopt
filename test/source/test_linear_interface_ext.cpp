/**
 * @file      test_linear_interface_ext.cpp
 * @brief     Extended unit tests for LinearInterface covering additional
 *            branches
 * @date      2026-02-19
 * @copyright BSD-3-Clause
 */

#include <expected>
#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/error.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_options.hpp>

using namespace gtopt;

TEST_CASE("LinearInterface - get/set problem name")
{
  LinearInterface interface;
  interface.set_prob_name("TestProblem");
  CHECK(interface.get_prob_name() == "TestProblem");
}

TEST_CASE("LinearInterface - set_col and set_rhs")
{
  LinearInterface interface;

  const auto col1 = interface.add_col("x1", 0.0, 10.0);
  interface.set_col(col1, 5.0);

  auto col_low = interface.get_col_low();
  auto col_upp = interface.get_col_upp();

  CHECK(col_low[col1] == doctest::Approx(5.0));
  CHECK(col_upp[col1] == doctest::Approx(5.0));

  // Add a row and test set_rhs
  SparseRow row("c1");
  row[col1] = 1.0;
  row.lowb = 0.0;
  row.uppb = 10.0;
  const auto r1 = interface.add_row(row);

  interface.set_rhs(r1, 7.0);

  auto row_low = interface.get_row_low();
  auto row_upp = interface.get_row_upp();

  CHECK(row_low[r1] == doctest::Approx(7.0));
  CHECK(row_upp[r1] == doctest::Approx(7.0));
}

TEST_CASE("LinearInterface - add_col without bounds (default)")
{
  LinearInterface interface;

  const auto col = interface.add_col("x_default");

  auto col_low = interface.get_col_low();
  auto col_upp = interface.get_col_upp();

  CHECK(col_low[col] == doctest::Approx(0.0));
  CHECK(col_upp[col] > 1.0e20);
}

TEST_CASE("LinearInterface - row bounds modification")
{
  LinearInterface interface;

  const auto col = interface.add_col("x", 0.0, 10.0);

  SparseRow row("c1");
  row[col] = 1.0;
  row.lowb = 0.0;
  row.uppb = 10.0;
  const auto r1 = interface.add_row(row);

  interface.set_row_low(r1, 2.0);
  interface.set_row_upp(r1, 8.0);

  auto row_low = interface.get_row_low();
  auto row_upp = interface.get_row_upp();

  CHECK(row_low[r1] == doctest::Approx(2.0));
  CHECK(row_upp[r1] == doctest::Approx(8.0));
}

TEST_CASE("LinearInterface - write LP to empty filename (no-op)")
{
  LinearInterface interface;
  interface.add_col("x1", 0.0, 10.0);

  // Writing with empty filename should not crash
  CHECK_NOTHROW(interface.write_lp(""));
}

TEST_CASE("LinearInterface - solve with solver options (dual algorithm)")
{
  LinearInterface interface;

  const auto x1 = interface.add_col("x1", 0.0, 10.0);
  const auto x2 = interface.add_col("x2", 0.0, 10.0);

  interface.set_obj_coeff(x1, 2.0);
  interface.set_obj_coeff(x2, 3.0);

  SparseRow row("c1");
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.uppb = 12.0;
  interface.add_row(row);

  SolverOptions opts;
  opts.algorithm = static_cast<int>(LPAlgo::dual);
  opts.presolve = true;

  auto result = interface.initial_solve(opts);
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("LinearInterface - solve with primal algorithm")
{
  LinearInterface interface;

  const auto x1 = interface.add_col("x1", 0.0, 10.0);
  interface.set_obj_coeff(x1, 1.0);

  SparseRow row("c1");
  row[x1] = 1.0;
  row.uppb = 5.0;
  interface.add_row(row);

  SolverOptions opts;
  opts.algorithm = static_cast<int>(LPAlgo::primal);

  auto result = interface.initial_solve(opts);
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("LinearInterface - solver options with tolerances")
{
  LinearInterface interface;

  const auto x1 = interface.add_col("x1", 0.0, 10.0);
  interface.set_obj_coeff(x1, 1.0);

  SparseRow row("c1");
  row[x1] = 1.0;
  row.uppb = 5.0;
  interface.add_row(row);

  SolverOptions opts;
  opts.optimal_eps = 1e-8;
  opts.feasible_eps = 1e-8;

  auto result = interface.initial_solve(opts);
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("LinearInterface - get_status and status checks")
{
  LinearInterface interface;

  const auto x1 = interface.add_col("x1", 0.0, 10.0);
  interface.set_obj_coeff(x1, 1.0);

  SparseRow row("c1");
  row[x1] = 1.0;
  row.uppb = 5.0;
  interface.add_row(row);

  auto result = interface.resolve();
  REQUIRE(result.has_value());

  CHECK(interface.is_optimal());
  CHECK_FALSE(interface.is_dual_infeasible());
  CHECK_FALSE(interface.is_prim_infeasible());
  CHECK(interface.get_status() == 0);
  CHECK(interface.get_kappa() >= 0.0);
}

TEST_CASE("LinearInterface - set_col_sol and set_row_dual")
{
  LinearInterface interface;

  const auto x1 = interface.add_col("x1", 0.0, 10.0);
  const auto x2 = interface.add_col("x2", 0.0, 10.0);

  interface.set_obj_coeff(x1, 1.0);
  interface.set_obj_coeff(x2, 2.0);

  SparseRow row("c1");
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.uppb = 8.0;
  interface.add_row(row);

  // Set initial solution
  std::vector<double> sol = {3.0, 4.0};
  interface.set_col_sol(sol);

  // Set row prices
  std::vector<double> dual = {1.0};
  interface.set_row_dual(dual);

  auto result = interface.resolve();
  REQUIRE(result.has_value());
}

TEST_CASE("LinearInterface - set_col_sol with null span (no-op)")
{
  LinearInterface interface;
  interface.add_col("x1", 0.0, 10.0);

  const std::span<const double> null_span;
  CHECK_NOTHROW(interface.set_col_sol(null_span));

  const std::span<const double> null_dual;
  CHECK_NOTHROW(interface.set_row_dual(null_dual));
}

TEST_CASE("LinearInterface - get_obj_value after solve")
{
  LinearInterface interface;

  const auto x1 = interface.add_col("x1", 0.0, 10.0);
  interface.set_obj_coeff(x1, 5.0);

  auto result = interface.resolve();
  REQUIRE(result.has_value());

  CHECK(interface.get_obj_value() == doctest::Approx(0.0));
}

TEST_CASE("LinearInterface - log file handling")
{
  const std::string log_file = "/tmp/gtopt_test_lp_log";

  LinearInterface interface(log_file);

  const auto x1 = interface.add_col("x1", 0.0, 10.0);
  interface.set_obj_coeff(x1, 1.0);

  SparseRow row("c1");
  row[x1] = 1.0;
  row.uppb = 5.0;
  interface.add_row(row);

  SolverOptions opts;
  opts.log_level = 1;

  auto result = interface.resolve(opts);
  REQUIRE(result.has_value());

  // Clean up log file
  const std::string log_filename = log_file + ".log";
  if (std::filesystem::exists(log_filename)) {
    std::filesystem::remove(log_filename);
  }
}

TEST_CASE("LinearInterface - FlatLinearProblem constructor")
{
  LinearProblem lp("DirectLoad");

  const auto col1 =
      lp.add_col({.name = "x1", .lowb = 0.0, .uppb = 10.0, .cost = 1.0});
  const auto col2 =
      lp.add_col({.name = "x2", .lowb = 0.0, .uppb = 10.0, .cost = 2.0});

  auto row1 = lp.add_row({.name = "c1", .uppb = 8.0});
  lp.set_coeff(row1, col1, 1.0);
  lp.set_coeff(row1, col2, 1.0);

  FlatOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  auto flat_lp = lp.to_flat(flat_opts);

  // Construct directly from FlatLinearProblem
  LinearInterface interface(flat_lp);

  CHECK(interface.get_numcols() == 2);
  CHECK(interface.get_numrows() == 1);

  auto result = interface.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("LinearInterface - time limit")
{
  LinearInterface interface;

  const auto x1 = interface.add_col("x1", 0.0, 10.0);
  interface.set_obj_coeff(x1, 1.0);

  interface.set_time_limit(100.0);

  auto result = interface.resolve();
  REQUIRE(result.has_value());
}
