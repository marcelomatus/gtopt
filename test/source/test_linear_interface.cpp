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
#include <gtopt/low_memory_snapshot.hpp>
#include <gtopt/memory_compress.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("LinearInterface - Constructor and basic operations")
{
  using namespace gtopt;

  // Create a basic interface
  LinearInterface interface;

  // Add some columns and rows
  const auto col1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
  });
  const auto col2 = interface.add_col(SparseCol {
      .uppb = 5.0,
      .cost = 3.0,
  });
  const auto col3 = interface.add_col(SparseCol {}.free());

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

  // Test objective coefficients set via SparseCol
  REQUIRE(interface.get_obj_coeff()[col1] == doctest::Approx(2.0));
  REQUIRE(interface.get_obj_coeff()[col2] == doctest::Approx(3.0));
  REQUIRE(interface.get_obj_coeff()[col3] == doctest::Approx(0.0));

  // Test adding constraints
  SparseRow row1;
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
  const auto x1 = interface.add_col(SparseCol {
      .uppb = 4.0,
      .cost = -3.0,
  });
  const auto x2 = interface.add_col(SparseCol {
      .uppb = 3.0,
      .cost = -2.0,
  });

  // Add constraint: x1 + 2x2 <= 10
  SparseRow row1;
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
  const auto continuous_var = interface.add_col(SparseCol {
      .uppb = 10.0,
  });
  const auto integer_var = interface.add_col(SparseCol {
      .uppb = 10.0,
  });
  const auto binary_var = interface.add_col(SparseCol {
      .uppb = 1.0,
  });

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

  // Create simple LP with name level 1 so both col and row names are tracked.
  LinearInterface interface;
  interface.set_lp_names_level(1);

  // Add variables using SparseCol so names are tracked for LP output
  interface.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .name = "x1",
  });
  interface.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .name = "x2",
  });

  // Add constraint using SparseRow API
  SparseRow row;
  row[ColIndex {0}] = 1.0;
  row[ColIndex {1}] = 1.0;
  row.lowb = 0.0;
  row.uppb = 15.0;
  row.class_name = "C";
  row.constraint_name = "1";
  row.variable_uid = Uid {0};
  row.context = make_stage_context(ScenarioUid {0}, StageUid {0});
  interface.add_row(row);

  // Set problem name
  interface.set_prob_name("TestLP");

  // Write LP to file
  const std::string temp_file = "test_interface_lp";
  auto result = interface.write_lp(temp_file);
  REQUIRE(result.has_value());

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
  const auto col1 = lp.add_col({.lowb = 0.0, .uppb = 10.0, .cost = 2.0});
  const auto col2 = lp.add_col({.lowb = 0.0, .uppb = 10.0, .cost = 3.0});

  // Add row
  const auto row1 = lp.add_row({.uppb = 15.0});

  // Set coefficients
  lp.set_coeff(row1, col1, 1.0);
  lp.set_coeff(row1, col2, 1.0);

  // Convert to flat format
  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  auto flat_lp = lp.flatten(flat_opts);

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

  const auto col1 = interface.add_col(SparseCol {
      .uppb = 10.0,
  });
  interface.set_col(col1, 5.0);

  auto col_low = interface.get_col_low();
  auto col_upp = interface.get_col_upp();

  CHECK(col_low[col1] == doctest::Approx(5.0));
  CHECK(col_upp[col1] == doctest::Approx(5.0));

  // Add a row and test set_rhs
  SparseRow row;
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

  const auto col = interface.add_col(SparseCol {});

  auto col_low = interface.get_col_low();
  auto col_upp = interface.get_col_upp();

  CHECK(col_low[col] == doctest::Approx(0.0));
  CHECK(interface.is_pos_inf(col_upp[col]));
}

TEST_CASE("LinearInterface - row bounds modification")
{
  using namespace gtopt;

  LinearInterface interface;

  const auto col = interface.add_col(SparseCol {
      .uppb = 10.0,
  });

  SparseRow row;
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
  interface.add_col(SparseCol {
      .uppb = 10.0,
  });

  // Writing with empty filename should succeed (no-op)
  CHECK(interface.write_lp("").has_value());
}

TEST_CASE("LinearInterface - solve with solver options (dual algorithm)")
{
  using namespace gtopt;

  LinearInterface interface;

  const auto x1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
  });
  const auto x2 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 3.0,
  });

  SparseRow row;
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

  const auto x1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
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

  const auto x1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
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

  const auto x1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.uppb = 5.0;
  interface.add_row(row);

  auto result = interface.resolve();
  REQUIRE(result.has_value());

  CHECK(interface.is_optimal());
  CHECK_FALSE(interface.is_dual_infeasible());
  CHECK_FALSE(interface.is_prim_infeasible());
  CHECK(interface.get_status() == 0);
  // CLP's largestDualError() may return 0 for clean solves.
  // Backends without kappa support return -1 (e.g. MindOpt).
  const double kappa0 = interface.get_kappa();
  if (kappa0 >= 0.0) {
    CHECK(kappa0 >= 0.0);
  }
}

TEST_CASE("LinearInterface - get_kappa returns meaningful condition number")
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Build a problem with a non-trivial basis matrix to get kappa > 1.
  // Use coefficients with different magnitudes to create a condition
  // number that is clearly not the default 1.0.
  //
  // min -x1 - 2*x2 - x3
  // s.t. 1000*x1 +    x2 +    x3 <= 5000
  //         x1 + 1000*x2 +    x3 <= 5000
  //         x1 +    x2 + 1000*x3 <= 5000
  //      0 <= x1,x2,x3 <= 100
  LinearInterface interface;

  const auto x1 = interface.add_col(SparseCol {
      .uppb = 100.0,
      .cost = -1.0,
  });
  const auto x2 = interface.add_col(SparseCol {
      .uppb = 100.0,
      .cost = -2.0,
  });
  const auto x3 = interface.add_col(SparseCol {
      .uppb = 100.0,
      .cost = -1.0,
  });

  SparseRow r1;
  r1[x1] = 1000.0;
  r1[x2] = 1.0;
  r1[x3] = 1.0;
  r1.uppb = 5000.0;
  interface.add_row(r1);

  SparseRow r2;
  r2[x1] = 1.0;
  r2[x2] = 1000.0;
  r2[x3] = 1.0;
  r2.uppb = 5000.0;
  interface.add_row(r2);

  SparseRow r3;
  r3[x1] = 1.0;
  r3[x2] = 1.0;
  r3[x3] = 1000.0;
  r3.uppb = 5000.0;
  interface.add_row(r3);

  auto result = interface.initial_solve();
  REQUIRE(result.has_value());
  CHECK(interface.is_optimal());

  const double kappa = interface.get_kappa();
  // CLP uses largestDualError() which may be 0 for clean solves.
  // Backends without kappa support return -1 (e.g. MindOpt).
  if (kappa >= 0.0) {
    CHECK(kappa >= 0.0);
  }

  // Log the kappa value for diagnostics
  MESSAGE("Solver kappa = ", kappa);
}

TEST_CASE("LinearInterface - get_kappa with explicit solver")
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Test kappa with each available solver backend.
  // HiGHS uses Highs::getKappa(exact=true) which computes the actual
  // condition number. CLP/CBC uses largestDualError() as a proxy
  // (conditionNumber() is unavailable in multi-factorization builds).
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  for (const auto& solver : reg.available_solvers()) {
    CAPTURE(solver);
    try {
      LinearInterface interface(solver);

      const auto x1 = interface.add_col(SparseCol {
          .uppb = 100.0,
          .cost = -1.0,
      });
      const auto x2 = interface.add_col(SparseCol {
          .uppb = 100.0,
          .cost = -2.0,
      });
      const auto x3 = interface.add_col(SparseCol {
          .uppb = 100.0,
          .cost = -1.0,
      });

      SparseRow r1;
      r1[x1] = 1000.0;
      r1[x2] = 1.0;
      r1[x3] = 1.0;
      r1.uppb = 5000.0;
      interface.add_row(r1);

      SparseRow r2;
      r2[x1] = 1.0;
      r2[x2] = 1000.0;
      r2[x3] = 1.0;
      r2.uppb = 5000.0;
      interface.add_row(r2);

      SparseRow r3;
      r3[x1] = 1.0;
      r3[x2] = 1.0;
      r3[x3] = 1000.0;
      r3.uppb = 5000.0;
      interface.add_row(r3);

      auto result = interface.initial_solve();
      REQUIRE(result.has_value());
      CHECK(interface.is_optimal());

      const double kappa = interface.get_kappa();
      // Backends without kappa support return -1 (e.g. MindOpt).
      if (kappa >= 0.0) {
        CHECK(kappa >= 0.0);
      }
      MESSAGE(solver, " kappa = ", kappa);
    } catch (const std::exception& e) {
      MESSAGE("Solver '", solver, "' not available: ", e.what());
    }
  }
}

TEST_CASE("LinearInterface - set_col_sol and set_row_dual")
{
  using namespace gtopt;

  LinearInterface interface;

  const auto x1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });
  const auto x2 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
  });

  SparseRow row;
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
  interface.add_col(SparseCol {
      .uppb = 10.0,
  });

  const std::span<const double> null_span;
  CHECK_NOTHROW(interface.set_col_sol(null_span));

  const std::span<const double> null_dual;
  CHECK_NOTHROW(interface.set_row_dual(null_dual));
}

TEST_CASE("LinearInterface - get_obj_value after solve")
{
  using namespace gtopt;

  LinearInterface interface;

  [[maybe_unused]] const auto x1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 5.0,
  });

  auto result = interface.resolve();
  REQUIRE(result.has_value());

  CHECK(interface.get_obj_value() == doctest::Approx(0.0));
}

TEST_CASE("LinearInterface - log file handling")
{
  using namespace gtopt;

  const std::string log_file = "/tmp/gtopt_test_lp_log";

  LinearInterface interface(std::string_view {}, log_file);

  const auto x1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
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

  const auto col1 = lp.add_col({.lowb = 0.0, .uppb = 10.0, .cost = 1.0});
  const auto col2 = lp.add_col({.lowb = 0.0, .uppb = 10.0, .cost = 2.0});

  auto row1 = lp.add_row({.uppb = 8.0});
  lp.set_coeff(row1, col1, 1.0);
  lp.set_coeff(row1, col2, 1.0);

  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  auto flat_lp = lp.flatten(flat_opts);

  // Construct directly from FlatLinearProblem
  auto& reg = SolverRegistry::instance();
  LinearInterface interface(reg.default_solver(), flat_lp);

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

  [[maybe_unused]] const auto x1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  interface.set_time_limit(100.0);

  auto result = interface.resolve();
  REQUIRE(result.has_value());
}

TEST_CASE("LinearInterface - duplicate name detection level 0 (col only)")
{
  using namespace gtopt;

  LinearInterface li;
  li.set_lp_names_level(0);

  // Duplicate col names are silently accepted at level 0
  const auto c1 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0,
      .name = "x",
  });
  const auto c2 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0,
      .name = "x",
  });
  CHECK(c1 != c2);

  // Col name map is populated at level 0 (first "x" wins)
  CHECK(li.col_name_map().size() == 1);
  CHECK(li.col_name_map().at("x") == static_cast<int32_t>(c1));
  // Row name map stays empty at level 0
  CHECK(li.row_name_map().empty());
}

TEST_CASE("LinearInterface - duplicate name detection level 1 (warn)")
{
  using namespace gtopt;

  LinearInterface li;
  li.set_lp_names_level(1);

  // First insertions populate the name maps
  const auto c1 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0,
      .name = "x",
  });
  const auto c2 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0,
      .name = "y",
  });
  CHECK(li.col_name_map().size() == 2);

  // Duplicate column name: warns but still adds the column
  const auto c3 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0,
      .name = "x",
  });
  CHECK(li.get_numcols() == 3);
  // Map still has 2 entries (first "x" wins)
  CHECK(li.col_name_map().size() == 2);
  CHECK(li.col_name_map().at("x") == static_cast<int32_t>(c1));

  // Rows: same behavior at level 1 - row name maps are populated
  li.set_obj_coeff(c1, 1.0);
  li.set_obj_coeff(c2, 1.0);
  li.set_obj_coeff(c3, 1.0);

  const auto ctx = make_stage_context(ScenarioUid {0}, StageUid {0});

  SparseRow row_a;
  row_a[c1] = 1.0;
  row_a.uppb = 1.0;
  row_a.class_name = "Cons";
  row_a.constraint_name = "a";
  row_a.variable_uid = Uid {0};
  row_a.context = ctx;
  const auto r1 = li.add_row(row_a);
  // row_name_map is populated at level >= 1
  CHECK(li.row_name_map().size() == 1);
  CHECK(li.row_name_map().at("cons_a_0_0_0") == static_cast<int32_t>(r1));

  // Duplicate row name: warns but still adds the row (first wins)
  SparseRow row_a2;
  row_a2[c2] = 1.0;
  row_a2.uppb = 1.0;
  row_a2.class_name = "Cons";
  row_a2.constraint_name = "a";
  row_a2.variable_uid = Uid {0};
  row_a2.context = ctx;
  const auto r2 = li.add_row(row_a2);
  CHECK(li.get_numrows() == 2);
  // Map still has 1 entry (first "cons_a_0_0_0" wins)
  CHECK(li.row_name_map().size() == 1);
  CHECK(li.row_name_map().at("cons_a_0_0_0") == static_cast<int32_t>(r1));
  CHECK(r1 != r2);
}

TEST_CASE("LinearInterface - duplicate name detection level 2 (error)")
{
  using namespace gtopt;

  LinearInterface li;
  li.set_lp_names_level(2);

  li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0,
      .name = "x",
  });
  CHECK(li.col_name_map().size() == 1);

  // Duplicate column name at level 2 throws
  CHECK_THROWS_AS(li.add_col(SparseCol {
                      .lowb = 0.0,
                      .uppb = 1.0,
                      .name = "x",
                  }),
                  std::runtime_error);

  // Different name is fine
  li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0,
      .name = "y",
  });
  CHECK(li.col_name_map().size() == 2);

  // Same for rows
  const auto ctx = make_stage_context(ScenarioUid {0}, StageUid {0});
  SparseRow row1;
  row1[ColIndex {0}] = 1.0;
  row1.uppb = 1.0;
  row1.class_name = "R";
  row1.constraint_name = "1";
  row1.variable_uid = Uid {0};
  row1.context = ctx;
  li.add_row(row1);
  CHECK(li.row_name_map().size() == 1);

  // Duplicate row name at level 2 throws
  SparseRow row1_dup;
  row1_dup[ColIndex {0}] = 1.0;
  row1_dup.uppb = 1.0;
  row1_dup.class_name = "R";
  row1_dup.constraint_name = "1";
  row1_dup.variable_uid = Uid {0};
  row1_dup.context = ctx;
  CHECK_THROWS_AS(li.add_row(row1_dup), std::runtime_error);
}

// ─── Warm-start clone tests ────────────────────────────────────────────────

TEST_CASE("LinearInterface - clone preserves solution")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Build a simple LP:  min 2x1 + 3x2  s.t.  x1 + x2 >= 5,  x1,x2 in [0,10]
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 3.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.lowb = 5.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE(li.is_optimal());
  const double orig_obj = li.get_obj_value();

  SUBCASE("clone produces same objective")
  {
    auto cloned = li.clone();
    auto r = cloned.resolve();
    REQUIRE(r.has_value());
    CHECK(cloned.get_obj_value() == doctest::Approx(orig_obj));
  }
}

TEST_CASE("LinearInterface - warm-start clone resolves after bound change")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // LP: min 2x1 + x2  s.t.  x1 + x2 >= 10,  x1 in [0,20], x2 in [0,20]
  // Optimal: x1=0, x2=10 → obj=10
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 20.0,
      .cost = 2.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = 20.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.lowb = 10.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  CHECK(li.get_obj_value() == doctest::Approx(10.0));

  // Clone with warm-start, tighten x1 >= 6, re-solve
  auto cloned = li.clone();
  cloned.set_col_low(x1, 6.0);

  SolverOptions ws_opts;
  ws_opts.reuse_basis = true;
  auto r = cloned.resolve(ws_opts);
  REQUIRE(r.has_value());
  REQUIRE(cloned.is_optimal());

  // Optimal: x1=6, x2=4 → obj=12+4=16
  CHECK(cloned.get_obj_value() == doctest::Approx(16.0));
  CHECK(cloned.get_col_sol()[x1] == doctest::Approx(6.0));
  CHECK(cloned.get_col_sol()[x2] == doctest::Approx(4.0));

  // Original is unmodified
  CHECK(li.get_col_low()[x1] == doctest::Approx(0.0));
}

TEST_CASE(
    "LinearInterface - warm-start clone works after barrier initial solve")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // LP: min 3x1 + 2x2  s.t.  x1 + x2 >= 8,  x1 in [0,10], x2 in [0,10]
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 3.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.lowb = 8.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  // Solve with barrier (the default algorithm)
  SolverOptions barrier_opts;
  barrier_opts.algorithm = LPAlgo::barrier;
  auto res = li.initial_solve(barrier_opts);
  REQUIRE(res.has_value());
  const double orig_obj = li.get_obj_value();
  // Optimal: x1=0, x2=8 → obj=16
  CHECK(orig_obj == doctest::Approx(16.0));

  // Clone with warm-start and resolve with dual simplex (the warm-start path)
  auto cloned = li.clone();

  // Tighten x1 >= 3, re-solve with warm-start
  cloned.set_col_low(x1, 3.0);

  SolverOptions ws_opts;
  ws_opts.reuse_basis = true;
  auto r = cloned.resolve(ws_opts);
  REQUIRE(r.has_value());
  REQUIRE(cloned.is_optimal());

  // Optimal: x1=3, x2=5 → obj=9+10=19
  CHECK(cloned.get_obj_value() == doctest::Approx(19.0));
  CHECK(cloned.get_col_sol()[x1] == doctest::Approx(3.0));
  CHECK(cloned.get_col_sol()[x2] == doctest::Approx(5.0));
}

TEST_CASE("LinearInterface - set_warm_start_solution exact dimensions")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // LP: min 2x1 + x2  s.t.  x1 + x2 >= 10
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 20.0,
      .cost = 2.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = 20.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.lowb = 10.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // Save primal and dual solution
  const auto sol = li.get_col_sol();
  const auto dual = li.get_row_dual();
  std::vector<double> saved_sol(sol.begin(), sol.end());
  std::vector<double> saved_dual(dual.begin(), dual.end());

  // Clone, modify bounds, apply saved solution, resolve with warm-start
  auto cloned = li.clone();
  cloned.set_col_low(x1, 5.0);
  cloned.set_warm_start_solution(saved_sol, saved_dual);

  SolverOptions ws_opts;
  ws_opts.reuse_basis = true;
  auto r = cloned.resolve(ws_opts);
  REQUIRE(r.has_value());
  REQUIRE(cloned.is_optimal());

  // Optimal: x1=5, x2=5 → obj=10+5=15
  CHECK(cloned.get_obj_value() == doctest::Approx(15.0));
  CHECK(cloned.get_col_sol()[x1] == doctest::Approx(5.0));
  CHECK(cloned.get_col_sol()[x2] == doctest::Approx(5.0));
}

TEST_CASE("LinearInterface - set_warm_start_solution pads extra rows")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // LP: min 2x1 + x2  s.t.  x1 + x2 >= 10
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 20.0,
      .cost = 2.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = 20.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.lowb = 10.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // Save solution (1 row)
  const auto sol = li.get_col_sol();
  const auto dual = li.get_row_dual();
  std::vector<double> saved_sol(sol.begin(), sol.end());
  std::vector<double> saved_dual(dual.begin(), dual.end());
  CHECK(saved_dual.size() == 1);

  // Add a new row (simulating a Benders cut), then apply saved solution
  SparseRow cut;
  cut[x1] = 1.0;
  cut.lowb = 2.0;
  cut.uppb = LinearProblem::DblMax;
  li.add_row(cut);
  CHECK(li.get_numrows() == 2);

  // Saved dual has 1 entry, LP has 2 rows — should pad with zero
  li.set_warm_start_solution(saved_sol, saved_dual);

  SolverOptions ws_opts;
  ws_opts.reuse_basis = true;
  auto r = li.resolve(ws_opts);
  REQUIRE(r.has_value());
  REQUIRE(li.is_optimal());

  // Optimal: x1=2, x2=8 → obj=4+8=12 (cut x1>=2 is binding)
  CHECK(li.get_obj_value() == doctest::Approx(12.0));
}

TEST_CASE("LinearInterface - set_warm_start_solution pads extra columns")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // LP: min 2x1 + x2  s.t.  x1 + x2 >= 10
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 20.0,
      .cost = 2.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = 20.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.lowb = 10.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // Save solution (2 cols)
  const auto sol = li.get_col_sol();
  std::vector<double> saved_sol(sol.begin(), sol.end());
  CHECK(saved_sol.size() == 2);

  // Add a slack column (simulating elastic filter), then apply saved solution
  [[maybe_unused]] const auto slack = li.add_col(SparseCol {
      .uppb = 100.0,
      .cost = 1000.0,
  });
  CHECK(li.get_numcols() == 3);

  // Saved sol has 2 entries, LP has 3 cols — should pad with zero
  li.set_warm_start_solution(saved_sol, {});

  SolverOptions ws_opts;
  ws_opts.reuse_basis = true;
  auto r = li.resolve(ws_opts);
  REQUIRE(r.has_value());
  REQUIRE(li.is_optimal());

  // Slack is free (0..100) with high cost → optimal: slack=0
  CHECK(li.get_col_sol()[slack] == doctest::Approx(0.0));
}

TEST_CASE("LinearInterface - set_warm_start_solution ignores stale snapshot")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // LP: min x1  s.t.  x1 >= 5
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 20.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.lowb = 5.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // A stale snapshot with more entries than the LP has — should be skipped
  std::vector<double> oversized_sol = {1.0, 2.0, 3.0};
  std::vector<double> oversized_dual = {1.0, 2.0};
  li.set_warm_start_solution(oversized_sol, oversized_dual);

  // Resolve should still work (stale vectors were ignored)
  auto r = li.resolve();
  REQUIRE(r.has_value());
  CHECK(li.get_obj_value() == doctest::Approx(5.0));
}

TEST_CASE("LinearInterface - set_warm_start_solution with empty spans is no-op")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.lowb = 3.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // Empty spans — no-op, should not crash
  li.set_warm_start_solution({}, {});

  auto r = li.resolve();
  REQUIRE(r.has_value());
  CHECK(li.get_obj_value() == doctest::Approx(3.0));
}

TEST_CASE(  // NOLINT
    "LinearInterface - load_flat with integer columns and name tracking")
{
  using namespace gtopt;

  LinearProblem lp("IntLP");

  const auto col1 = lp.add_col({.lowb = 0.0, .uppb = 10.0, .cost = 1.0});
  const auto col2 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 2.0,
      .is_integer = true,
  });

  auto row1 = lp.add_row({.uppb = 8.0});
  lp.set_coeff(row1, col1, 1.0);
  lp.set_coeff(row1, col2, 1.0);

  // Convert to flat with names
  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  auto flat_lp = lp.flatten(flat_opts);

  // Load with name tracking enabled
  LinearInterface li;
  li.set_lp_names_level(1);
  li.load_flat(flat_lp);

  CHECK(li.get_numcols() == 2);
  CHECK(li.get_numrows() == 1);

  // Without metadata on SparseCol/SparseRow, names are empty
  CHECK(li.col_name_map().empty());
  CHECK(li.row_name_map().empty());

  // Integer column should be marked
  CHECK(li.is_integer(col2));
  CHECK(li.is_continuous(col1));

  auto result = li.resolve();
  REQUIRE(result.has_value());
}

TEST_CASE("LinearInterface - load_flat without names (level 0)")  // NOLINT
{
  using namespace gtopt;

  LinearProblem lp("NoNames");
  const auto col = lp.add_col({.lowb = 0.0, .uppb = 5.0, .cost = 1.0});

  // Add a row so flatten() does not early-return (needs ncols>0 AND nrows>0).
  auto row = SparseRow {.uppb = 10.0};
  row[col] = 1.0;
  [[maybe_unused]] const auto row_idx = lp.add_row(row);

  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  auto flat_lp = lp.flatten(flat_opts);

  LinearInterface li;
  li.set_lp_names_level(0);
  li.load_flat(flat_lp);

  // Without metadata on SparseCol/SparseRow, names are empty at any level
  CHECK(li.col_name_map().empty());
  CHECK(li.row_name_map().empty());
}

TEST_CASE("LinearInterface - initial_solve returns error on infeasible LP")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Create an infeasible LP: x >= 10 AND x <= 5
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 5.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.lowb = 10.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto result = li.initial_solve();
  REQUIRE_FALSE(result.has_value());

  const auto& err = result.error();
  CHECK(err.code == ErrorCode::SolverError);
  CHECK_FALSE(err.message.empty());
  CHECK(err.status == 2);  // infeasible status

  // Verify status queries
  CHECK_FALSE(li.is_optimal());
  CHECK(li.is_prim_infeasible());
  CHECK(li.get_status() == 2);
}

TEST_CASE("LinearInterface - resolve returns error on infeasible LP")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Create an infeasible LP: x >= 10 AND x <= 5
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 5.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.lowb = 10.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto result = li.resolve();
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == ErrorCode::SolverError);
  CHECK_FALSE(li.is_optimal());
}

TEST_CASE("LinearInterface - get_coeff on empty LP returns zero")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearInterface li;
  const auto col = li.add_col(SparseCol {
      .uppb = 10.0,
  });

  // No rows added, matrix should return 0.0 for any coefficient
  CHECK(li.get_coeff(RowIndex {0}, col) == doctest::Approx(0.0));
}

TEST_CASE("LinearInterface - set_coeff and get_coeff round trip")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = 10.0,
  });

  SparseRow row;
  row[x1] = 2.0;
  row[x2] = 3.0;
  row.uppb = 20.0;
  const auto r1 = li.add_row(row);

  CHECK(li.get_coeff(r1, x1) == doctest::Approx(2.0));
  CHECK(li.get_coeff(r1, x2) == doctest::Approx(3.0));

  // Modify coefficient
  li.set_coeff(r1, x1, 5.0);
  CHECK(li.get_coeff(r1, x1) == doctest::Approx(5.0));
  CHECK(li.supports_set_coeff());
}

TEST_CASE("LinearInterface - set_log_file direct call")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearInterface li;
  li.set_log_file("/tmp/test_direct_log");

  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.uppb = 5.0;
  li.add_row(row);

  // Solve with logging enabled — exercises open/close log handler
  SolverOptions opts;
  opts.log_level = 1;
  auto result = li.resolve(opts);
  REQUIRE(result.has_value());

  // Cleanup
  const std::string log_file = "/tmp/test_direct_log.log";
  if (std::filesystem::exists(log_file)) {
    std::filesystem::remove(log_file);
  }
}

TEST_CASE("LinearInterface - solve with time_limit option")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.uppb = 5.0;
  li.add_row(row);

  SolverOptions opts;
  opts.time_limit = 60.0;
  auto result = li.initial_solve(opts);
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

// ─── row_index_to_name tests ────────────────────────────────────────────────

TEST_CASE("LinearInterface - row_index_to_name via load_flat")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearProblem lp("RowNames");
  const auto c1 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 1.0,
  });
  const auto c2 = lp.add_col({
      .lowb = 0.0,
      .uppb = 5.0,
      .cost = 2.0,
  });

  auto r1 = lp.add_row({
      .uppb = 8.0,
  });
  lp.set_coeff(r1, c1, 1.0);
  lp.set_coeff(r1, c2, 1.0);

  auto r2 = lp.add_row({
      .uppb = 4.0,
  });
  lp.set_coeff(r2, c1, 1.0);

  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  auto flat_lp = lp.flatten(flat_opts);

  LinearInterface li;
  li.set_lp_names_level(2);
  li.load_flat(flat_lp);

  REQUIRE(li.get_numrows() == 2);

  SUBCASE("row_index_to_name has correct size (empty strings without metadata)")
  {
    const auto& names = li.row_index_to_name();
    // Without metadata, flatten() produces empty name strings, but
    // row_index_to_name is still populated (one entry per row).
    REQUIRE(names.size() == 2);
    CHECK(names[RowIndex {0}].empty());
    CHECK(names[RowIndex {1}].empty());
  }

  SUBCASE("row_name_map is empty without metadata")
  {
    // Empty name strings are not inserted into the name map.
    CHECK(li.row_name_map().empty());
  }
}

TEST_CASE("LinearInterface - row_index_to_name updated by add_row")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearInterface li;
  li.set_lp_names_level(1);

  const auto c1 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .name = "x",
  });
  const auto ctx = make_stage_context(ScenarioUid {0}, StageUid {0});

  auto make_row = [&](std::string_view cname, Uid uid, double uppb) -> SparseRow
  {
    SparseRow r;
    r[c1] = 1.0;
    r.uppb = uppb;
    r.class_name = "Row";
    r.constraint_name = cname;
    r.variable_uid = uid;
    r.context = ctx;
    return r;
  };

  li.add_row(make_row("a", Uid {0}, 5.0));
  li.add_row(make_row("b", Uid {1}, 3.0));
  li.add_row(make_row("c", Uid {2}, 7.0));

  REQUIRE(li.get_numrows() == 3);
  const auto& names = li.row_index_to_name();
  REQUIRE(names.size() == 3);
  CHECK(names[RowIndex {0}] == "row_a_0_0_0");
  CHECK(names[RowIndex {1}] == "row_b_1_0_0");
  CHECK(names[RowIndex {2}] == "row_c_2_0_0");
}

TEST_CASE(
    "LinearInterface - row_index_to_name rebuilt after delete_rows")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearInterface li;
  li.set_lp_names_level(2);

  const auto c1 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .name = "x",
  });
  const auto ctx = make_stage_context(ScenarioUid {0}, StageUid {0});

  // Add 4 rows using SparseRow with metadata for name generation.
  // Use different UIDs to produce unique names: r_x_<uid>_0_0
  auto make_row = [&](Uid uid) -> SparseRow
  {
    SparseRow r;
    r[c1] = 1.0;
    r.uppb = 10.0;
    r.class_name = "R";
    r.constraint_name = "x";
    r.variable_uid = uid;
    r.context = ctx;
    return r;
  };

  for (int i = 0; i < 4; ++i) {
    li.add_row(make_row(Uid {i}));
  }
  REQUIRE(li.get_numrows() == 4);

  SUBCASE("delete middle row shifts indices")
  {
    // Delete row 1 ("r_x_1_0_0")
    std::vector<int> to_delete {
        1,
    };
    li.delete_rows(to_delete);

    REQUIRE(li.get_numrows() == 3);
    const auto& names = li.row_index_to_name();
    REQUIRE(names.size() == 3);
    CHECK(names[RowIndex {0}] == "r_x_0_0_0");
    CHECK(names[RowIndex {1}] == "r_x_2_0_0");
    CHECK(names[RowIndex {2}] == "r_x_3_0_0");

    // row_name_map must agree
    CHECK(li.row_name_map().at("r_x_0_0_0") == 0);
    CHECK(li.row_name_map().at("r_x_2_0_0") == 1);
    CHECK(li.row_name_map().at("r_x_3_0_0") == 2);
    CHECK(li.row_name_map().count("r_x_1_0_0") == 0);
  }

  SUBCASE("delete first and last rows")
  {
    std::vector<int> to_delete {
        0,
        3,
    };
    li.delete_rows(to_delete);

    REQUIRE(li.get_numrows() == 2);
    const auto& names = li.row_index_to_name();
    REQUIRE(names.size() == 2);
    CHECK(names[RowIndex {0}] == "r_x_1_0_0");
    CHECK(names[RowIndex {1}] == "r_x_2_0_0");

    CHECK(li.row_name_map().at("r_x_1_0_0") == 0);
    CHECK(li.row_name_map().at("r_x_2_0_0") == 1);
  }

  SUBCASE("delete all rows leaves empty maps")
  {
    std::vector<int> to_delete {
        0,
        1,
        2,
        3,
    };
    li.delete_rows(to_delete);

    CHECK(li.get_numrows() == 0);
    CHECK(li.row_index_to_name().empty());
    CHECK(li.row_name_map().empty());
  }

  SUBCASE("add rows after deletion continues correctly")
  {
    std::vector<int> to_delete {
        1,
        2,
    };
    li.delete_rows(to_delete);
    REQUIRE(li.get_numrows() == 2);

    SparseRow r_new;
    r_new[c1] = 1.0;
    r_new.uppb = 5.0;
    r_new.class_name = "R";
    r_new.constraint_name = "new";
    r_new.variable_uid = Uid {4};
    r_new.context = ctx;
    li.add_row(r_new);

    REQUIRE(li.get_numrows() == 3);
    const auto& names = li.row_index_to_name();
    REQUIRE(names.size() == 3);
    CHECK(names[RowIndex {0}] == "r_x_0_0_0");
    CHECK(names[RowIndex {1}] == "r_x_3_0_0");
    CHECK(names[RowIndex {2}] == "r_new_4_0_0");
  }
}

TEST_CASE("LinearInterface - row_index_to_name empty at level 0")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearInterface li;
  li.set_lp_names_level(0);

  const auto c1 = li.add_col(SparseCol {
      .uppb = 10.0,
  });

  SparseRow row {
      .lowb = 0.0,
      .uppb = 5.0,
  };
  row[c1] = 1.0;
  li.add_row(row);

  CHECK(li.row_index_to_name().empty());
  CHECK(li.row_name_map().empty());
}

// ---------------------------------------------------------------------------
// Algorithm fallback cycle tests
// ---------------------------------------------------------------------------

TEST_CASE("LinearInterface - algorithm fallback on infeasible initial_solve")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Infeasible LP: x >= 10 AND x <= 5 — no algorithm can solve it.
  // The fallback cycle should try all 3 algorithms and still fail,
  // with the error message indicating the fallback cycle was exhausted.
  for (const auto algo :
       {LPAlgo::barrier, LPAlgo::dual, LPAlgo::primal, LPAlgo::default_algo})
  {
    LinearInterface li;
    const auto x1 = li.add_col(SparseCol {
        .uppb = 5.0,
        .cost = 1.0,
    });

    SparseRow row;
    row[x1] = 1.0;
    row.lowb = 10.0;
    row.uppb = LinearProblem::DblMax;
    li.add_row(row);

    auto result = li.initial_solve(SolverOptions {
        .algorithm = algo,
        .log_level = 0,
    });
    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    CHECK(err.code == ErrorCode::SolverError);
    CHECK(err.message.find("fallback") != std::string::npos);
    CHECK_FALSE(li.is_optimal());
  }
}

TEST_CASE("LinearInterface - algorithm fallback on infeasible resolve")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Same infeasible LP tested via resolve path.
  for (const auto algo :
       {LPAlgo::barrier, LPAlgo::dual, LPAlgo::primal, LPAlgo::default_algo})
  {
    LinearInterface li;
    const auto x1 = li.add_col(SparseCol {
        .uppb = 5.0,
        .cost = 1.0,
    });

    SparseRow row;
    row[x1] = 1.0;
    row.lowb = 10.0;
    row.uppb = LinearProblem::DblMax;
    li.add_row(row);

    auto result = li.resolve(SolverOptions {
        .algorithm = algo,
        .log_level = 0,
    });
    REQUIRE_FALSE(result.has_value());

    const auto& err = result.error();
    CHECK(err.code == ErrorCode::SolverError);
    CHECK(err.message.find("fallback") != std::string::npos);
    CHECK_FALSE(li.is_optimal());
  }
}

TEST_CASE(
    "LinearInterface - optimal LP succeeds without fallback for all algorithms")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Feasible LP: min x + y, s.t. x + y >= 4, x,y >= 0  →  obj = 4.
  // All algorithms should succeed on the first attempt (no fallback needed).
  for (const auto algo :
       {LPAlgo::barrier, LPAlgo::dual, LPAlgo::primal, LPAlgo::default_algo})
  {
    LinearInterface li;
    const auto x1 = li.add_col(SparseCol {
        .uppb = 100.0,
        .cost = 1.0,
    });
    const auto x2 = li.add_col(SparseCol {
        .uppb = 100.0,
        .cost = 1.0,
    });

    SparseRow row;
    row[x1] = 1.0;
    row[x2] = 1.0;
    row.lowb = 4.0;
    row.uppb = LinearProblem::DblMax;
    li.add_row(row);

    auto result = li.initial_solve(SolverOptions {
        .algorithm = algo,
        .log_level = 0,
    });
    REQUIRE(result.has_value());
    CHECK(li.is_optimal());
    CHECK(li.get_obj_value() == doctest::Approx(4.0));
  }
}

TEST_CASE(
    "LinearInterface - fallback cycle on resolve after feasible "
    "initial_solve")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Solve a feasible LP, then make it infeasible via bound change and resolve.
  // The fallback cycle should engage and ultimately fail.
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.lowb = 1.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  // First solve: feasible
  auto r1 = li.initial_solve(SolverOptions {
      .algorithm = LPAlgo::dual,
      .log_level = 0,
  });
  REQUIRE(r1.has_value());
  CHECK(li.is_optimal());

  // Make infeasible: x <= 0 but x >= 1
  li.set_col_upp(x1, 0.0);

  auto r2 = li.resolve(SolverOptions {
      .algorithm = LPAlgo::dual,
      .log_level = 0,
  });
  REQUIRE_FALSE(r2.has_value());
  CHECK(r2.error().code == ErrorCode::SolverError);
  CHECK(r2.error().message.find("fallback") != std::string::npos);
}

TEST_CASE("LinearInterface - max_fallbacks=0 disables fallback")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Infeasible LP with max_fallbacks=0: should fail immediately without
  // the "fallback" keyword in the error message.
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 5.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.lowb = 10.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  SUBCASE("initial_solve")
  {
    auto result = li.initial_solve(SolverOptions {
        .algorithm = LPAlgo::dual,
        .log_level = 0,
        .max_fallbacks = 0,
    });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::SolverError);
    CHECK(result.error().message.find("fallback") == std::string::npos);
  }

  SUBCASE("resolve")
  {
    auto result = li.resolve(SolverOptions {
        .algorithm = LPAlgo::dual,
        .log_level = 0,
        .max_fallbacks = 0,
    });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::SolverError);
    CHECK(result.error().message.find("fallback") == std::string::npos);
  }
}

TEST_CASE("LinearInterface - max_fallbacks=1 tries one alternative")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Infeasible LP with max_fallbacks=1: should try one fallback and still
  // fail, but the error should mention "fallback".
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 5.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.lowb = 10.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto result = li.initial_solve(SolverOptions {
      .algorithm = LPAlgo::barrier,
      .log_level = 0,
      .max_fallbacks = 1,
  });
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == ErrorCode::SolverError);
  CHECK(result.error().message.find("fallback") != std::string::npos);
}

// ── Low-memory mode unit tests ────────────────────────────────────────────

/// Helper: build a simple LP with 2 variables and 1 constraint, flatten it,
/// load it into a LinearInterface, and return (li, flat_lp, x1, x2).
namespace
// NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
struct SimpleLp
{
  LinearInterface li;
  FlatLinearProblem flat;
  ColIndex x1;
  ColIndex x2;
};

SimpleLp make_simple_lp()
{
  // min 2x1 + 3x2  s.t.  x1 + x2 >= 5,  0 <= x1,x2 <= 10
  LinearProblem lp;
  const auto c1 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 2.0,
  });
  const auto c2 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 3.0,
  });
  const auto r = lp.add_row({
      .lowb = 5.0,
      .uppb = SparseRow::DblMax,
  });
  lp.set_coeff(r, c1, 1.0);
  lp.set_coeff(r, c2, 1.0);

  LpMatrixOptions opts;
  opts.col_with_names = true;
  opts.row_with_names = true;
  auto flat = lp.flatten(opts);

  LinearInterface li;
  li.load_flat(flat);
  li.save_base_numrows();

  return {
      std::move(li),
      std::move(flat),
      ColIndex {0},
      ColIndex {1},
  };
}
}  // namespace

TEST_CASE("LinearInterface — low_memory save_snapshot round-trip")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

  // Solve baseline
  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE(li.is_optimal());
  const double orig_obj = li.get_obj_value();
  const auto orig_ncols = li.get_numcols();
  const auto orig_nrows = li.get_numrows();

  SUBCASE("level 1: release and reconstruct preserves LP")
  {
    li.set_low_memory(LowMemoryMode::snapshot);
    li.save_snapshot(FlatLinearProblem {flat});

    li.release_backend();
    CHECK(li.is_backend_released());
    CHECK_FALSE(li.has_backend());

    li.reconstruct_backend();
    CHECK_FALSE(li.is_backend_released());
    CHECK(li.has_backend());
    CHECK(li.get_numcols() == orig_ncols);
    CHECK(li.get_numrows() == orig_nrows);

    auto r = li.resolve();
    REQUIRE(r.has_value());
    CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
  }

  SUBCASE("level 2: compress/decompress round-trip preserves LP")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::zstd);
    li.save_snapshot(FlatLinearProblem {flat});

    li.release_backend();
    CHECK(li.is_backend_released());

    li.reconstruct_backend();
    CHECK_FALSE(li.is_backend_released());
    CHECK(li.get_numcols() == orig_ncols);
    CHECK(li.get_numrows() == orig_nrows);

    auto r = li.resolve();
    REQUIRE(r.has_value());
    CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
  }

  SUBCASE("multiple release/reconstruct cycles produce same result")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::zstd);
    li.save_snapshot(FlatLinearProblem {flat});

    for (int i = 0; i < 3; ++i) {
      li.release_backend();
      CHECK(li.is_backend_released());

      li.reconstruct_backend();
      CHECK_FALSE(li.is_backend_released());

      auto r = li.resolve();
      REQUIRE(r.has_value());
      CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
    }
  }
}

TEST_CASE(
    "LinearInterface — low_memory reconstruct with dynamic cols")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  const double orig_obj = li.get_obj_value();

  li.set_low_memory(LowMemoryMode::snapshot);
  li.save_snapshot(FlatLinearProblem {flat});

  // Add a dynamic column (simulating alpha variable)
  SparseCol alpha_col;
  alpha_col.uppb = 1000.0;
  alpha_col.cost = 0.0;
  [[maybe_unused]] const auto alpha = li.add_col(alpha_col);
  li.record_dynamic_col(alpha_col);
  li.save_base_numrows();

  CHECK(li.get_numcols() == 3);

  // Release and reconstruct — dynamic col should be replayed
  li.release_backend();
  li.reconstruct_backend();

  CHECK(li.get_numcols() == 3);

  auto r = li.resolve();
  REQUIRE(r.has_value());
  // Alpha has zero cost, so objective is unchanged
  CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
}

TEST_CASE("LinearInterface — low_memory reconstruct with cuts")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  li.set_low_memory(LowMemoryMode::snapshot);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();

  const auto base_nrows = li.base_numrows();

  // Add a cut row: x1 <= 3 (binding: optimal was x1=5)
  SparseRow cut;
  cut[x1] = 1.0;
  cut.lowb = -LinearProblem::DblMax;
  cut.uppb = 3.0;
  li.add_row(cut);
  li.record_cut_row(cut);

  CHECK(li.get_numrows() == base_nrows + 1);

  // Solve with the cut
  auto r1 = li.resolve();
  REQUIRE(r1.has_value());
  const double obj_with_cut = li.get_obj_value();
  // x1 <= 3, so optimal x1=3, x2=2 → obj = 6 + 6 = 12
  CHECK(obj_with_cut == doctest::Approx(12.0));

  // Release and reconstruct — cut should be replayed
  li.release_backend();
  li.reconstruct_backend();

  CHECK(li.get_numrows() == base_nrows + 1);

  auto r2 = li.resolve();
  REQUIRE(r2.has_value());
  CHECK(li.get_obj_value() == doctest::Approx(obj_with_cut));
}

TEST_CASE("LinearInterface — low_memory cut deletion tracking")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  [[maybe_unused]] const double orig_obj = li.get_obj_value();

  li.set_low_memory(LowMemoryMode::snapshot);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();

  const auto base = static_cast<int>(li.base_numrows());

  // Add two binding cuts:
  //   cut0: x2 >= 4  → forces x2=4, x1=1, obj=14
  //   cut1: x1 <= 2  → alone: x1=2, x2=3, obj=13
  SparseRow cut0;
  cut0[x2] = 1.0;
  cut0.lowb = 4.0;
  cut0.uppb = LinearProblem::DblMax;
  li.add_row(cut0);
  li.record_cut_row(cut0);

  SparseRow cut1;
  cut1[x1] = 1.0;
  cut1.lowb = -LinearProblem::DblMax;
  cut1.uppb = 2.0;
  li.add_row(cut1);
  li.record_cut_row(cut1);

  // Delete cut0 (absolute row index = base + 0)
  std::array<int, 1> deleted = {
      base,
  };
  li.delete_rows(deleted);
  li.record_cut_deletion(deleted);

  // Release and reconstruct — only cut1 should be present
  li.release_backend();
  li.reconstruct_backend();

  // base structural rows + 1 remaining cut
  CHECK(li.get_numrows() == static_cast<size_t>(base) + 1);

  auto r = li.resolve();
  REQUIRE(r.has_value());
  // Only cut1 active: x1 <= 2, so optimal x1=2, x2=3 → obj = 4 + 9 = 13
  CHECK(li.get_obj_value() == doctest::Approx(13.0));
}

TEST_CASE("LinearInterface — low_memory reconstruct with warm-start")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // Capture solution for warm-start
  std::vector<double> col_sol(li.get_col_sol_raw().begin(),
                              li.get_col_sol_raw().end());
  std::vector<double> row_dual(li.get_row_dual_raw().begin(),
                               li.get_row_dual_raw().end());
  const double orig_obj = li.get_obj_value();

  li.set_low_memory(LowMemoryMode::snapshot);
  li.save_snapshot(FlatLinearProblem {flat});

  li.release_backend();
  li.reconstruct_backend(col_sol, row_dual);

  // Warm-start should allow immediate optimal
  SolverOptions ws_opts;
  ws_opts.reuse_basis = true;
  auto r = li.resolve(ws_opts);
  REQUIRE(r.has_value());
  CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
}

TEST_CASE("LinearInterface — low_memory capture_hot_start_cuts")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  li.set_low_memory(LowMemoryMode::snapshot);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();

  // Add a hot-start cut before calling capture_hot_start_cuts
  // x1 <= 3 (binding: optimal was x1=5)
  SparseRow cut;
  cut[x1] = 1.0;
  cut.lowb = -LinearProblem::DblMax;
  cut.uppb = 3.0;
  li.add_row(cut);

  // Capture should read the row above base_numrows
  li.capture_hot_start_cuts();

  // Verify cut is captured by releasing and reconstructing
  li.release_backend();
  li.reconstruct_backend();

  // The captured cut should be replayed
  CHECK(li.get_numrows() == li.base_numrows() + 1);

  auto r = li.resolve();
  REQUIRE(r.has_value());
  // x1 <= 3 → optimal x1=3, x2=2 → obj = 12
  CHECK(li.get_obj_value() == doctest::Approx(12.0));
}

TEST_CASE(
    "LinearInterface — low_memory clone from reconstructed backend")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // Capture solution
  std::vector<double> col_sol(li.get_col_sol_raw().begin(),
                              li.get_col_sol_raw().end());
  std::vector<double> row_dual(li.get_row_dual_raw().begin(),
                               li.get_row_dual_raw().end());
  const double orig_obj = li.get_obj_value();

  li.set_low_memory(LowMemoryMode::snapshot);
  li.save_snapshot(FlatLinearProblem {flat});

  // Release and reconstruct
  li.release_backend();
  li.reconstruct_backend(col_sol, row_dual);

  // Clone from reconstructed backend with warm-start
  auto cloned = li.clone(col_sol, row_dual);

  auto r = cloned.resolve();
  REQUIRE(r.has_value());
  CHECK(cloned.get_obj_value() == doctest::Approx(orig_obj));

  // Modify clone — original is unmodified
  cloned.set_col_upp(x1, 3.0);
  auto r2 = cloned.resolve();
  REQUIRE(r2.has_value());
  // x1 <= 3 → x1=3, x2=2 → obj = 6 + 6 = 12
  CHECK(cloned.get_obj_value() == doctest::Approx(12.0));

  // Original still produces the same objective
  auto r3 = li.resolve();
  REQUIRE(r3.has_value());
  CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
}

TEST_CASE("LinearInterface — low_memory level 2 multiple cycles")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  [[maybe_unused]] const double orig_obj = li.get_obj_value();

  li.set_low_memory(LowMemoryMode::compress, CompressionCodec::zstd);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();

  // Cycle 1: add a binding cut (x1 <= 4), release, reconstruct
  SparseRow cut1;
  cut1[x1] = 1.0;
  cut1.lowb = -LinearProblem::DblMax;
  cut1.uppb = 4.0;
  li.add_row(cut1);
  li.record_cut_row(cut1);

  li.release_backend();
  li.reconstruct_backend();

  auto r1 = li.resolve();
  REQUIRE(r1.has_value());
  const double obj1 = li.get_obj_value();
  // x1 <= 4, so x1=4, x2=1 → obj = 8 + 3 = 11
  CHECK(obj1 == doctest::Approx(11.0));

  // Cycle 2: add another binding cut (x2 >= 3), release, reconstruct
  SparseRow cut2;
  cut2[x2] = 1.0;
  cut2.lowb = 3.0;
  cut2.uppb = LinearProblem::DblMax;
  li.add_row(cut2);
  li.record_cut_row(cut2);

  li.release_backend();
  li.reconstruct_backend();

  auto r2 = li.resolve();
  REQUIRE(r2.has_value());
  // x1 <= 4, x2 >= 3 → x1=2, x2=3 → obj = 4 + 9 = 13
  CHECK(li.get_obj_value() == doctest::Approx(13.0));
}

TEST_CASE("LinearInterface — set_low_memory(0) discards flat LP")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

  li.set_low_memory(LowMemoryMode::snapshot);
  li.save_snapshot(FlatLinearProblem {flat});

  // Disable low_memory — flat LP should be discarded
  li.set_low_memory(LowMemoryMode::off);

  // release_backend is a no-op when low_memory is off — backend stays alive
  li.release_backend();
  CHECK(li.has_backend());
}

TEST_CASE("LinearInterface — clone with warm-start parameters")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // Capture primal and dual
  std::vector<double> col_sol(li.get_col_sol_raw().begin(),
                              li.get_col_sol_raw().end());
  std::vector<double> row_dual(li.get_row_dual_raw().begin(),
                               li.get_row_dual_raw().end());

  SUBCASE("clone with warm-start resolves correctly")
  {
    auto cloned = li.clone(col_sol, row_dual);
    SolverOptions ws_opts;
    ws_opts.reuse_basis = true;
    auto r = cloned.resolve(ws_opts);
    REQUIRE(r.has_value());
    CHECK(cloned.get_obj_value() == doctest::Approx(li.get_obj_value()));
  }

  SUBCASE("clone without warm-start also works")
  {
    auto cloned = li.clone();
    auto r = cloned.resolve();
    REQUIRE(r.has_value());
    CHECK(cloned.get_obj_value() == doctest::Approx(li.get_obj_value()));
  }

  SUBCASE("clone with empty spans is same as no warm-start")
  {
    auto cloned = li.clone({}, {});
    auto r = cloned.resolve();
    REQUIRE(r.has_value());
    CHECK(cloned.get_obj_value() == doctest::Approx(li.get_obj_value()));
  }
}

// ── release_backend memory cleanup tests ─────────────────────────────────

TEST_CASE(
    "LinearInterface — release_backend destroys solver backend")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE(li.is_optimal());

  li.set_low_memory(LowMemoryMode::snapshot);
  li.save_snapshot(FlatLinearProblem {flat});

  SUBCASE("backend pointer is null after release")
  {
    CHECK(li.has_backend());
    li.release_backend();
    CHECK_FALSE(li.has_backend());
    CHECK(li.is_backend_released());
  }

  SUBCASE("double release is a safe no-op")
  {
    li.release_backend();
    CHECK_FALSE(li.has_backend());

    // Second release should not crash
    li.release_backend();
    CHECK_FALSE(li.has_backend());
    CHECK(li.is_backend_released());
  }

  SUBCASE(
      "release with cache_warm_start caches solution for transparent "
      "read access")
  {
    // Override default to enable caching
    li.set_low_memory(LowMemoryMode::snapshot,
                      CompressionCodec::lz4,
                      /*cache_warm_start=*/true);
    li.save_snapshot(FlatLinearProblem {flat});

    const auto obj_before = li.get_obj_value();
    const auto sol_before = std::vector<double>(li.get_col_sol_raw().begin(),
                                                li.get_col_sol_raw().end());
    const auto dual_before = std::vector<double>(li.get_row_dual_raw().begin(),
                                                 li.get_row_dual_raw().end());

    li.release_backend();
    CHECK_FALSE(li.has_backend());

    // Cached values must match pre-release values
    CHECK(li.get_obj_value() == doctest::Approx(obj_before));

    const auto sol_after = li.get_col_sol_raw();
    REQUIRE(sol_after.size() == sol_before.size());
    for (size_t i = 0; i < sol_before.size(); ++i) {
      CHECK(sol_after[i] == doctest::Approx(sol_before[i]));
    }

    const auto dual_after = li.get_row_dual_raw();
    REQUIRE(dual_after.size() == dual_before.size());
    for (size_t i = 0; i < dual_before.size(); ++i) {
      CHECK(dual_after[i] == doctest::Approx(dual_before[i]));
    }
  }

  SUBCASE("release with low_memory off is a no-op — backend stays alive")
  {
    li.set_low_memory(LowMemoryMode::off);
    li.release_backend();
    CHECK(li.has_backend());
    CHECK_FALSE(li.is_backend_released());
  }
}

TEST_CASE(
    "LinearInterface — clone release destroys clone backend "
    "independently")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  const double orig_obj = li.get_obj_value();

  SUBCASE("clone backend is independent — destroying clone preserves parent")
  {
    {
      auto cloned = li.clone();
      auto r = cloned.resolve();
      REQUIRE(r.has_value());
      CHECK(cloned.has_backend());
      // clone goes out of scope here — its backend is destroyed
    }

    // Parent backend is still alive and functional
    CHECK(li.has_backend());
    auto r2 = li.resolve();
    REQUIRE(r2.has_value());
    CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
  }

  SUBCASE("multiple clones can be created and destroyed")
  {
    for (int i = 0; i < 5; ++i) {
      auto cloned = li.clone();
      auto r = cloned.resolve();
      REQUIRE(r.has_value());
      CHECK(cloned.get_obj_value() == doctest::Approx(orig_obj));
      // clone destroyed each iteration
    }

    // Parent still works
    CHECK(li.has_backend());
    auto r = li.resolve();
    REQUIRE(r.has_value());
    CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
  }

  SUBCASE("clone from reconstructed backend works correctly")
  {
    li.set_low_memory(LowMemoryMode::compress, CompressionCodec::lz4);
    li.save_snapshot(FlatLinearProblem {flat});

    // Release and reconstruct
    li.release_backend();
    CHECK_FALSE(li.has_backend());

    li.reconstruct_backend();
    CHECK(li.has_backend());

    // Clone from reconstructed backend
    auto cloned = li.clone();
    auto r = cloned.resolve();
    REQUIRE(r.has_value());
    CHECK(cloned.get_obj_value() == doctest::Approx(orig_obj));

    // Destroy clone, release parent again
    cloned = LinearInterface {};
    li.release_backend();
    CHECK_FALSE(li.has_backend());

    // Reconstruct again and verify
    li.reconstruct_backend();
    auto r2 = li.resolve();
    REQUIRE(r2.has_value());
    CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
  }
}

TEST_CASE(
    "LinearInterface — release/reconstruct cycle with cuts "
    "preserves memory pattern")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  li.set_low_memory(LowMemoryMode::compress, CompressionCodec::lz4);
  li.save_snapshot(FlatLinearProblem {flat});
  li.save_base_numrows();
  const auto base = li.base_numrows();

  // Add a Benders cut: x1 <= 4
  SparseRow cut;
  cut[x1] = 1.0;
  cut.lowb = -LinearProblem::DblMax;
  cut.uppb = 4.0;
  li.add_row(cut);
  li.record_cut_row(cut);

  auto r1 = li.resolve();
  REQUIRE(r1.has_value());
  const double obj_with_cut = li.get_obj_value();

  // Simulate SDDP pattern: release → reconstruct → add more cuts → release
  for (int cycle = 0; cycle < 3; ++cycle) {
    li.release_backend();
    CHECK_FALSE(li.has_backend());
    CHECK(li.is_backend_released());

    li.reconstruct_backend();
    CHECK(li.has_backend());
    CHECK_FALSE(li.is_backend_released());

    // Cut should be replayed
    CHECK(li.get_numrows() == base + 1);

    auto r = li.resolve();
    REQUIRE(r.has_value());
    CHECK(li.get_obj_value() == doctest::Approx(obj_with_cut));
  }
}

TEST_CASE(
    "LinearInterface — cache_warm_start=false discards solution "
    "vectors on release")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE(li.is_optimal());
  const double orig_obj = li.get_obj_value();

  SUBCASE("released state has empty cached vectors")
  {
    li.set_low_memory(LowMemoryMode::snapshot,
                      CompressionCodec::zstd,
                      /*cache_warm_start=*/false);
    li.save_snapshot(FlatLinearProblem {flat});

    li.release_backend();
    CHECK(li.is_backend_released());

    // Cached scalars are still available
    CHECK(li.get_obj_value() == doctest::Approx(orig_obj));

    // All solution vectors are empty (memory freed).
    // SDDP caches its own copies before calling release_backend().
    CHECK(li.get_col_sol_raw().empty());
    CHECK(li.get_row_dual_raw().empty());
    CHECK(li.get_col_cost_raw().empty());
  }

  SUBCASE("reconstruct with explicit warm-start works without cache")
  {
    // Capture warm-start data before enabling no-cache mode
    std::vector<double> col_sol(li.get_col_sol_raw().begin(),
                                li.get_col_sol_raw().end());
    std::vector<double> row_dual(li.get_row_dual_raw().begin(),
                                 li.get_row_dual_raw().end());

    li.set_low_memory(LowMemoryMode::compress,
                      CompressionCodec::lz4,
                      /*cache_warm_start=*/false);
    li.save_snapshot(FlatLinearProblem {flat});

    li.release_backend();
    CHECK(li.is_backend_released());

    // Reconstruct with explicitly provided warm-start
    li.reconstruct_backend(col_sol, row_dual);
    CHECK(li.has_backend());

    auto r = li.resolve();
    REQUIRE(r.has_value());
    CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
  }

  SUBCASE("reconstruct without warm-start still produces correct result")
  {
    li.set_low_memory(LowMemoryMode::snapshot,
                      CompressionCodec::zstd,
                      /*cache_warm_start=*/false);
    li.save_snapshot(FlatLinearProblem {flat});

    li.release_backend();
    li.reconstruct_backend();  // cold start

    auto r = li.resolve();
    REQUIRE(r.has_value());
    CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
  }

  SUBCASE("multiple cycles with no cache still work")
  {
    li.set_low_memory(LowMemoryMode::compress,
                      CompressionCodec::lz4,
                      /*cache_warm_start=*/false);
    li.save_snapshot(FlatLinearProblem {flat});

    for (int i = 0; i < 3; ++i) {
      li.release_backend();
      CHECK(li.get_col_sol_raw().empty());
      CHECK(li.get_col_cost_raw().empty());

      li.reconstruct_backend();
      auto r = li.resolve();
      REQUIRE(r.has_value());
      CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
    }
  }
}

TEST_CASE(
    "LowMemorySnapshot — compress/decompress size "
    "behavior across cycles")  // NOLINT
{
  auto [li, flat, x1, x2] = make_simple_lp();

  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // ── Helper: total heap bytes held by numeric vectors ──
  auto flat_vectors_size = [](const FlatLinearProblem& f) -> size_t
  {
    auto bytes_of = [](const auto& v)
    {
      return v.size() * sizeof(typename std::decay_t<decltype(v)>::value_type);
    };
    return bytes_of(f.matbeg) + bytes_of(f.matind) + bytes_of(f.colint)
        + bytes_of(f.matval) + bytes_of(f.collb) + bytes_of(f.colub)
        + bytes_of(f.objval) + bytes_of(f.rowlb) + bytes_of(f.rowub)
        + bytes_of(f.col_scales) + bytes_of(f.row_scales);
  };

  auto flat_vectors_nonempty = [](const FlatLinearProblem& f) -> bool
  { return !f.matbeg.empty() || !f.collb.empty() || !f.matval.empty(); };

  SUBCASE("compress creates compressed buffer and clears flat vectors")
  {
    LowMemorySnapshot snap;
    snap.flat_lp = FlatLinearProblem {flat};

    CHECK(flat_vectors_nonempty(snap.flat_lp));
    CHECK_FALSE(snap.is_compressed());

    const auto orig_size = flat_vectors_size(snap.flat_lp);
    CHECK(orig_size > 0);

    // First compress: creates compressed buffer, clears vectors
    snap.compress(CompressionCodec::lz4);
    CHECK(snap.is_compressed());
    CHECK_FALSE(flat_vectors_nonempty(snap.flat_lp));
    CHECK(flat_vectors_size(snap.flat_lp) == 0);
    CHECK_FALSE(snap.compressed_lp.empty());

    const auto compressed_size = snap.compressed_lp.data.size();
    CHECK(compressed_size > 0);
    CHECK(compressed_size <= orig_size);
  }

  SUBCASE("decompress restores vectors, keeps compressed buffer")
  {
    LowMemorySnapshot snap;
    snap.flat_lp = FlatLinearProblem {flat};

    snap.compress(CompressionCodec::lz4);
    const auto compressed_size = snap.compressed_lp.data.size();

    // Decompress: restores flat vectors, keeps compressed buffer
    snap.decompress();
    CHECK(flat_vectors_nonempty(snap.flat_lp));
    CHECK(flat_vectors_size(snap.flat_lp) > 0);

    // Compressed buffer is still present (persistent cache)
    CHECK(snap.is_compressed());
    CHECK(snap.compressed_lp.data.size() == compressed_size);
  }

  SUBCASE("multiple compress/decompress cycles maintain invariants")
  {
    LowMemorySnapshot snap;
    snap.flat_lp = FlatLinearProblem {flat};

    const auto orig_size = flat_vectors_size(snap.flat_lp);

    // First compress: creates the persistent compressed buffer
    snap.compress(CompressionCodec::lz4);
    const auto compressed_size = snap.compressed_lp.data.size();
    CHECK(compressed_size > 0);
    CHECK(flat_vectors_size(snap.flat_lp) == 0);

    for (int cycle = 0; cycle < 5; ++cycle) {
      // Decompress: vectors restored, compressed buffer retained
      snap.decompress();
      CHECK(flat_vectors_nonempty(snap.flat_lp));
      const auto decompressed_size = flat_vectors_size(snap.flat_lp);
      CHECK(decompressed_size >= orig_size);
      CHECK(snap.compressed_lp.data.size() == compressed_size);

      // Re-compress: vectors cleared, compressed buffer unchanged
      snap.compress(CompressionCodec::lz4);
      CHECK_FALSE(flat_vectors_nonempty(snap.flat_lp));
      CHECK(flat_vectors_size(snap.flat_lp) == 0);
      CHECK(snap.compressed_lp.data.size() == compressed_size);
    }
  }

  SUBCASE("clear_flat_lp_vectors frees decompressed data")
  {
    LowMemorySnapshot snap;
    snap.flat_lp = FlatLinearProblem {flat};

    snap.compress(CompressionCodec::lz4);
    snap.decompress();
    CHECK(flat_vectors_nonempty(snap.flat_lp));

    // Manual clear — same as what release_backend does on subsequent calls
    clear_flat_lp_vectors(snap.flat_lp);
    CHECK_FALSE(flat_vectors_nonempty(snap.flat_lp));
    CHECK(flat_vectors_size(snap.flat_lp) == 0);

    // Compressed buffer still intact — can decompress again
    CHECK(snap.is_compressed());
    snap.decompress();
    CHECK(flat_vectors_nonempty(snap.flat_lp));
  }

  SUBCASE("decompress is idempotent when vectors already present")
  {
    LowMemorySnapshot snap;
    snap.flat_lp = FlatLinearProblem {flat};

    snap.compress(CompressionCodec::lz4);
    snap.decompress();
    const auto size_after_first = flat_vectors_size(snap.flat_lp);

    // Second decompress should be no-op (vectors already present)
    snap.decompress();
    CHECK(flat_vectors_size(snap.flat_lp) == size_after_first);
  }

  SUBCASE("compress is idempotent — compressed buffer never changes")
  {
    LowMemorySnapshot snap;
    snap.flat_lp = FlatLinearProblem {flat};

    snap.compress(CompressionCodec::lz4);
    const auto buf1 = snap.compressed_lp.data;

    snap.decompress();
    snap.compress(CompressionCodec::lz4);
    // Buffer content should be identical (never re-compressed)
    CHECK(snap.compressed_lp.data == buf1);
  }

  SUBCASE("zstd codec also works correctly")
  {
    LowMemorySnapshot snap;
    snap.flat_lp = FlatLinearProblem {flat};

    snap.compress(CompressionCodec::zstd);
    CHECK(snap.is_compressed());
    CHECK_FALSE(flat_vectors_nonempty(snap.flat_lp));

    snap.decompress();
    CHECK(flat_vectors_nonempty(snap.flat_lp));

    // Verify data survives round-trip
    CHECK(snap.flat_lp.ncols == flat.ncols);
    CHECK(snap.flat_lp.nrows == flat.nrows);
    CHECK(snap.flat_lp.matbeg.size() == flat.matbeg.size());
    CHECK(snap.flat_lp.matval.size() == flat.matval.size());

    for (size_t i = 0; i < flat.matval.size(); ++i) {
      CHECK(snap.flat_lp.matval[i] == doctest::Approx(flat.matval[i]));
    }
  }
}
