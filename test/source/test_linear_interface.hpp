/**
 * @file      test_linear_interface.hpp
 * @brief     Tests for the LinearInterface class
 * @date      Sat Apr 19 12:45:00 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <expected>
#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/error.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_options.hpp>

TEST_CASE("LinearInterface - Constructor and basic operations")
{
  using namespace gtopt;

  // Create a basic interface
  LinearInterface interface;

  // Add some columns and rows
  const auto col1 = interface.add_col("x1", 0.0, 10.0);
  const auto col2 = interface.add_col("x2", 0.0, 5.0);
  const auto col3 = interface.add_free_col("x3");

  // Check dimensions
  REQUIRE(interface.get_numcols() == 3);
  REQUIRE(interface.get_numrows() == 0);

  // Check column bounds
  auto col_low = interface.get_col_low();
  auto col_upp = interface.get_col_upp();

  REQUIRE(col_low[col1] == doctest::Approx(0.0));
  REQUIRE(col_upp[col1] == doctest::Approx(10.0));
  REQUIRE(col_low[col2] == doctest::Approx(0.0));
  REQUIRE(col_upp[col2] == doctest::Approx(5.0));
  REQUIRE(col_low[col3] <= -1.0e20);  // Free columns have large negative bound
  REQUIRE(col_upp[col3] >= 1.0e20);  // Free columns have large positive bound

  // Test setting objective coefficients
  interface.set_obj_coeff(col1, 2.0);
  interface.set_obj_coeff(col2, 3.0);

  REQUIRE(interface.get_obj_coeff()[col1] == doctest::Approx(2.0));
  REQUIRE(interface.get_obj_coeff()[col2] == doctest::Approx(3.0));
  REQUIRE(interface.get_obj_coeff()[col3] == doctest::Approx(0.0));

  // Test adding constraints
  SparseRow row1("constraint1");
  row1[col1] = 1.0;
  row1[col2] = 2.0;
  row1.lowb = 0.0;
  row1.uppb = 15.0;

  const auto r1 = interface.add_row(row1);
  REQUIRE(interface.get_numrows() == 1);

  // Test getting row bounds
  auto row_low = interface.get_row_low();
  auto row_upp = interface.get_row_upp();

  REQUIRE(row_low[r1] == doctest::Approx(0.0));
  REQUIRE(row_upp[r1] == doctest::Approx(15.0));
}

TEST_CASE("LinearInterface - LP solution")
{
  using namespace gtopt;

  // Create interface with a simple LP problem
  // Maximize 3x1 + 2x2
  // Subject to:
  //   x1 + 2x2 <= 10
  //   x1 <= 4
  //   x2 <= 3
  //   x1, x2 >= 0

  LinearInterface interface;

  // Add variables
  const auto x1 = interface.add_col("x1", 0.0, 4.0);
  const auto x2 = interface.add_col("x2", 0.0, 3.0);

  // Set objective (negative for maximization in the OSI solvers)
  interface.set_obj_coeff(x1, -3.0);
  interface.set_obj_coeff(x2, -2.0);

  // Add constraint: x1 + 2x2 <= 10
  SparseRow row1("c1");
  row1[x1] = 1.0;
  row1[x2] = 2.0;
  row1.uppb = 10.0;
  interface.add_row(row1);

  // Solve the problem
  const SolverOptions options;

  auto result = interface.initial_solve(options);
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);  // 0 = optimal

  // Check the solution (should be x1=4, x2=3)
  auto sol = interface.get_col_sol();
  REQUIRE(sol[x1] == doctest::Approx(4.0));
  REQUIRE(sol[x2] == doctest::Approx(3.0));

  // Check objective value: 3*4 + 2*3 = 18
  const double obj_val =
      -interface.get_obj_value();  // Negate back to get maximization value
  REQUIRE(obj_val == doctest::Approx(18.0));
}

TEST_CASE("LinearInterface - Variable types and bounds")
{
  using namespace gtopt;

  LinearInterface interface;

  // Add different types of variables
  const auto continuous_var = interface.add_col("cont", 0.0, 10.0);
  const auto integer_var = interface.add_col("int", 0.0, 10.0);
  const auto binary_var = interface.add_col("bin", 0.0, 1.0);

  // Set variable types
  interface.set_continuous(continuous_var);
  interface.set_integer(integer_var);
  interface.set_binary(binary_var);

  // Check variable types
  REQUIRE(interface.is_continuous(continuous_var));
  REQUIRE(!interface.is_integer(continuous_var));

  REQUIRE(!interface.is_continuous(integer_var));
  REQUIRE(interface.is_integer(integer_var));

  REQUIRE(!interface.is_continuous(binary_var));
  REQUIRE(interface.is_integer(binary_var));

  // Test changing bounds
  interface.set_col_low(continuous_var, 2.0);
  interface.set_col_upp(continuous_var, 8.0);

  auto col_low = interface.get_col_low();
  auto col_upp = interface.get_col_upp();

  REQUIRE(col_low[continuous_var] == doctest::Approx(2.0));
  REQUIRE(col_upp[continuous_var] == doctest::Approx(8.0));
}

TEST_CASE("LinearInterface - LP file output")
{
  using namespace gtopt;

  // Create simple LP
  LinearInterface interface;

  // Add variables
  interface.add_col("x1", 0.0, 10.0);
  interface.add_col("x2", 0.0, 10.0);

  // Set objective
  interface.set_obj_coeff(ColIndex {0}, 2.0);
  interface.set_obj_coeff(ColIndex {1}, 3.0);

  // Add constraint
  SparseRow row;
  row[ColIndex {0}] = 1.0;
  row[ColIndex {1}] = 1.0;
  row.uppb = 15.0;
  interface.add_row(row);

  // Set problem name
  interface.set_prob_name("TestLP");

  // Write LP to file
  const std::string temp_file = "test_interface_lp";
  interface.write_lp(temp_file);

  // Verify file was created
  const std::string lp_file = temp_file + ".lp";
  const bool file_exists = std::filesystem::exists(lp_file);
  if (file_exists) {
    std::filesystem::remove(lp_file);  // Clean up
  }
  REQUIRE(file_exists);
}

TEST_CASE("LinearInterface - Loading from FlatLinearProblem")
{
  using namespace gtopt;

  // Create linear problem
  LinearProblem lp("TestLP");

  // Add columns
  const auto col1 =
      lp.add_col({.name = "x1", .lowb = 0.0, .uppb = 10.0, .cost = 2.0});
  const auto col2 =
      lp.add_col({.name = "x2", .lowb = 0.0, .uppb = 10.0, .cost = 3.0});

  // Add row
  const auto row1 = lp.add_row({.name = "c1", .uppb = 15.0});

  // Set coefficients
  lp.set_coeff(row1, col1, 1.0);
  lp.set_coeff(row1, col2, 1.0);

  // Convert to flat format
  FlatOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  auto flat_lp = lp.to_flat(flat_opts);

  // Create interface and load the problem
  LinearInterface interface;
  interface.load_flat(flat_lp);

  // Verify dimensions
  REQUIRE(interface.get_numcols() == 2);
  REQUIRE(interface.get_numrows() == 1);

  // Verify objective coefficients
  REQUIRE(interface.get_obj_coeff()[0] == doctest::Approx(2.0));
  REQUIRE(interface.get_obj_coeff()[1] == doctest::Approx(3.0));

  // Solve the problem
  auto result = interface.initial_solve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);  // 0 = optimal

  // Get solution
  const auto sol = interface.get_col_sol();

  // For this simple problem, both variables should be at their lower bounds (0)
  // since we're minimizing and they have positive objective coefficients
  REQUIRE(sol[0] == doctest::Approx(0.0));
  REQUIRE(sol[1] == doctest::Approx(0.0));
}

TEST_CASE("LinearInterface - get/set problem name")
{
  using namespace gtopt;

  LinearInterface interface;
  interface.set_prob_name("TestProblem");
  CHECK(interface.get_prob_name() == "TestProblem");
}

TEST_CASE("LinearInterface - set_col and set_rhs")
{
  using namespace gtopt;

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
  using namespace gtopt;

  LinearInterface interface;

  const auto col = interface.add_col("x_default");

  auto col_low = interface.get_col_low();
  auto col_upp = interface.get_col_upp();

  CHECK(col_low[col] == doctest::Approx(0.0));
  CHECK(col_upp[col] > 1.0e20);
}

TEST_CASE("LinearInterface - row bounds modification")
{
  using namespace gtopt;

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
  using namespace gtopt;

  LinearInterface interface;
  interface.add_col("x1", 0.0, 10.0);

  // Writing with empty filename should not crash
  CHECK_NOTHROW(interface.write_lp(""));
}

TEST_CASE("LinearInterface - solve with solver options (dual algorithm)")
{
  using namespace gtopt;

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
  opts.algorithm = LPAlgo::dual;
  opts.presolve = true;

  auto result = interface.initial_solve(opts);
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("LinearInterface - solve with primal algorithm")
{
  using namespace gtopt;

  LinearInterface interface;

  const auto x1 = interface.add_col("x1", 0.0, 10.0);
  interface.set_obj_coeff(x1, 1.0);

  SparseRow row("c1");
  row[x1] = 1.0;
  row.uppb = 5.0;
  interface.add_row(row);

  SolverOptions opts;
  opts.algorithm = LPAlgo::primal;

  auto result = interface.initial_solve(opts);
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("LinearInterface - solver options with tolerances")
{
  using namespace gtopt;

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
  using namespace gtopt;

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
  using namespace gtopt;

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
  using namespace gtopt;

  LinearInterface interface;
  interface.add_col("x1", 0.0, 10.0);

  const std::span<const double> null_span;
  CHECK_NOTHROW(interface.set_col_sol(null_span));

  const std::span<const double> null_dual;
  CHECK_NOTHROW(interface.set_row_dual(null_dual));
}

TEST_CASE("LinearInterface - get_obj_value after solve")
{
  using namespace gtopt;

  LinearInterface interface;

  const auto x1 = interface.add_col("x1", 0.0, 10.0);
  interface.set_obj_coeff(x1, 5.0);

  auto result = interface.resolve();
  REQUIRE(result.has_value());

  CHECK(interface.get_obj_value() == doctest::Approx(0.0));
}

TEST_CASE("LinearInterface - log file handling")
{
  using namespace gtopt;

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
  using namespace gtopt;

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
  using namespace gtopt;

  LinearInterface interface;

  const auto x1 = interface.add_col("x1", 0.0, 10.0);
  interface.set_obj_coeff(x1, 1.0);

  interface.set_time_limit(100.0);

  auto result = interface.resolve();
  REQUIRE(result.has_value());
}

TEST_CASE("LinearInterface - duplicate name detection level 0 (disabled)")
{
  using namespace gtopt;

  LinearInterface li;
  li.set_lp_names_level(0);

  // Duplicate names are silently accepted (no tracking)
  const auto c1 = li.add_col("x", 0.0, 1.0);
  const auto c2 = li.add_col("x", 0.0, 1.0);
  CHECK(c1 != c2);

  // Name maps stay empty at level 0
  CHECK(li.col_name_map().empty());
  CHECK(li.row_name_map().empty());
}

TEST_CASE("LinearInterface - duplicate name detection level 1 (warn)")
{
  using namespace gtopt;

  LinearInterface li;
  li.set_lp_names_level(1);

  // First insertions populate the name maps
  const auto c1 = li.add_col("x", 0.0, 1.0);
  const auto c2 = li.add_col("y", 0.0, 1.0);
  CHECK(li.col_name_map().size() == 2);

  // Duplicate column name: warns but still adds the column
  const auto c3 = li.add_col("x", 0.0, 1.0);
  CHECK(li.get_numcols() == 3);
  // Map still has 2 entries (first "x" wins)
  CHECK(li.col_name_map().size() == 2);
  CHECK(li.col_name_map().at("x") == static_cast<int32_t>(c1));

  // Rows: same behavior
  li.set_obj_coeff(c1, 1.0);
  li.set_obj_coeff(c2, 1.0);
  li.set_obj_coeff(c3, 1.0);

  SparseRow row_a {
      .name = "cons_a",
      .lowb = 0.0,
      .uppb = 1.0,
  };
  row_a[c1] = 1.0;
  const auto r1 = li.add_row(row_a);
  CHECK(li.row_name_map().size() == 1);
  CHECK(li.row_name_map().at("cons_a") == static_cast<int32_t>(r1));

  // Duplicate row name: warns but still adds the row
  SparseRow row_a2 {
      .name = "cons_a",
      .lowb = 0.0,
      .uppb = 1.0,
  };
  row_a2[c2] = 1.0;
  const auto r2 = li.add_row(row_a2);
  CHECK(li.get_numrows() == 2);
  CHECK(li.row_name_map().size() == 1);
  CHECK(li.row_name_map().at("cons_a") == static_cast<int32_t>(r1));
  CHECK(r1 != r2);
}

TEST_CASE("LinearInterface - duplicate name detection level 2 (error)")
{
  using namespace gtopt;

  LinearInterface li;
  li.set_lp_names_level(2);

  li.add_col("x", 0.0, 1.0);
  CHECK(li.col_name_map().size() == 1);

  // Duplicate column name at level 2 throws
  CHECK_THROWS_AS(li.add_col("x", 0.0, 1.0), std::runtime_error);

  // Different name is fine
  li.add_col("y", 0.0, 1.0);
  CHECK(li.col_name_map().size() == 2);

  // Same for rows
  SparseRow row1 {
      .name = "r1",
      .lowb = 0.0,
      .uppb = 1.0,
  };
  row1[ColIndex {0}] = 1.0;
  li.add_row(row1);
  CHECK(li.row_name_map().size() == 1);

  SparseRow row1_dup {
      .name = "r1",
      .lowb = 0.0,
      .uppb = 1.0,
  };
  row1_dup[ColIndex {0}] = 1.0;
  CHECK_THROWS_AS(li.add_row(row1_dup), std::runtime_error);
}
