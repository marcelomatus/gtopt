/**
 * @file      check_solvers.hpp
 * @brief     C++ solver plugin test suite callable as a library
 * @date      2026-03-26
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides a self-contained test suite that exercises every method of
 * LinearInterface against every available solver plugin.  The results
 * are returned as structured data so callers can pretty-print, log, or
 * assert on them without any external tool dependency.
 *
 * Entry points
 * ------------
 * - ``run_solver_tests(solver_name)``   – run all tests for one solver.
 * - ``check_all_solvers()``             – run all tests for every available
 *   solver; returns a non-zero exit code if any test fails.
 *
 * CLI integration
 * ---------------
 * Both functions are invoked by the ``--check-solvers`` flag of the gtopt
 * binary so operators can validate the solver environment without writing
 * any input files.
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace gtopt
{

// ---------------------------------------------------------------------------
// Result types
// ---------------------------------------------------------------------------

/**
 * @brief Result of running one named test against one solver.
 */
struct SolverTestResult
{
  /// Name of the test case (e.g. "single_bus_lp").
  std::string name;

  /// true = test passed; false = test failed.
  bool passed {};

  /// Human-readable one-line outcome message.
  std::string message;

  /// Optional additional detail (first failure assertion, exception text).
  std::string detail;

  /// Wall-clock duration of the test in seconds.
  double duration_s {};
};

/**
 * @brief Aggregated results for all tests run against one solver.
 */
struct SolverTestReport
{
  /// Solver name (e.g. "clp", "highs").
  std::string solver;

  /// One entry per test case that was executed.
  std::vector<SolverTestResult> results;

  /// true iff every result has passed == true.
  [[nodiscard]] bool passed() const noexcept;

  /// Number of passing tests.
  [[nodiscard]] std::ptrdiff_t n_passed() const noexcept;

  /// Number of failing tests.
  [[nodiscard]] std::ptrdiff_t n_failed() const noexcept;
};

// ---------------------------------------------------------------------------
// Test functions
// ---------------------------------------------------------------------------

/**
 * @brief Run the full LinearInterface test suite against @p solver_name.
 *
 * The test suite covers:
 *  - construction (default, by-name, from FlatLinearProblem)
 *  - problem-name get/set
 *  - add_col / add_free_col / add_row / delete_rows
 *  - set/get objective coefficients, column bounds, row bounds
 *  - get_coeff / set_coeff (skipped when !supports_set_coeff())
 *  - set_continuous / set_integer / is_continuous / is_integer
 *  - LP names and name maps (row_name_map, col_name_map)
 *  - load_flat from FlatLinearProblem
 *  - initial_solve with all LPAlgo variants (default, primal, dual, barrier)
 *  - get_obj_value / get_col_sol / get_row_dual / get_col_cost
 *  - is_optimal / is_prim_infeasible / is_dual_infeasible
 *  - get_kappa
 *  - resolve (warm re-solve after bound tightening)
 *  - clone (deep copy; independent solve)
 *  - set_warm_start_solution
 *  - save_base_numrows / reset_from
 *  - lp_stats_* fields after load_flat
 *  - write_lp (creates a temp file and verifies creation)
 *
 * @param solver_name   Solver identifier understood by SolverRegistry
 *                      (e.g. "clp", "cbc", "highs").
 * @param verbose       When true, print each test name as it executes to
 *                      stdout (useful when running from a terminal).
 * @return              Aggregated pass/fail for all test cases.
 */
[[nodiscard]] SolverTestReport run_solver_tests(std::string_view solver_name,
                                                bool verbose = false);

/**
 * @brief Run the test suite against every solver known to SolverRegistry.
 *
 * Calls run_solver_tests() for each available solver in the order returned
 * by SolverRegistry::available_solvers() and prints a summary to stdout.
 *
 * @param verbose   Forward to run_solver_tests.
 * @return 0 if all tests for all solvers pass, 1 otherwise.
 */
[[nodiscard]] int check_all_solvers(bool verbose = false);

}  // namespace gtopt
