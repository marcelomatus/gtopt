/**
 * @file      test_check_solvers.hpp
 * @brief     Unit tests for the C++ solver plugin test suite (check_solvers)
 * @date      2026-03-26
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Tests the public API of check_solvers.hpp / check_solvers.cpp using
 * the default (auto-detected) solver.  Every SolverTestReport returned
 * by run_solver_tests() must have passed == true for all tests.
 *
 * These tests are intentionally solver-agnostic: they ask the registry
 * for the default solver and exercise each test case, so they pass on
 * any installation that has at least one solver plugin available.
 */

#include <string>

#include <doctest/doctest.h>
#include <gtopt/check_solvers.hpp>
#include <gtopt/solver_registry.hpp>

#include "solver_test_helpers.hpp"

using namespace gtopt;

namespace
{

using namespace gtopt;

/// Return the default solver name, or "" if no solver is available.
[[nodiscard]] std::string default_solver_or_skip()
{
  auto& reg = SolverRegistry::instance();
  const auto avail = reg.available_solvers();
  if (avail.empty()) {
    return {};
  }
  return std::string(reg.default_solver());
}

/// Run a single `check_solvers` test against ONE specific solver plugin.
/// Skips with a MESSAGE when the plugin is not loaded — this lets the
/// split-out per-solver add_rows / add_cols TEST_CASEs parallelise
/// across solvers under `ctest -j20` (each TEST_CASE is its own
/// ctest entry).  Uses `run_one_solver_test` (single-test dispatch) so
/// each TEST_CASE invokes ONLY the named test — the prior form called
/// `run_solver_tests` (full per-solver suite) and discarded all but one
/// result, which on mindopt re-ran the full algorithm-fallback cycle
/// for every `add_rows` / `add_cols` invocation and inflated wall time
/// to ~34 s per case.
void run_named_test_on_solver(const std::string& solver,
                              const std::string& test_name)
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  if (!reg.has_solver(solver)) {
    MESSAGE("solver '" << solver << "' not available — skipping " << test_name);
    return;
  }
  CAPTURE(solver);
  const auto r = run_one_solver_test(solver, test_name);
  if (!r.passed
      && (gtopt::solver_test::is_license_failure(r.message)
          || gtopt::solver_test::is_license_failure(r.detail)))
  {
    MESSAGE("solver '" << solver << "' license unavailable — skipping "
                       << test_name);
    return;
  }
  INFO("solver: " << solver << "  detail: " << r.detail);
  CHECK(r.passed);
}

}  // namespace

// ---------------------------------------------------------------------------
// SolverTestReport helpers
// ---------------------------------------------------------------------------

TEST_CASE("SolverTestReport - passed() / n_passed() / n_failed()")  // NOLINT
{
  using namespace gtopt;

  SolverTestReport report;
  report.solver = "dummy";

  CHECK(report.passed());
  CHECK(report.n_passed() == 0);
  CHECK(report.n_failed() == 0);

  report.results.push_back(SolverTestResult {
      .name = "a",
      .passed = true,
      .message = "ok",
      .detail = {},
  });
  report.results.push_back(SolverTestResult {
      .name = "b",
      .passed = false,
      .message = "FAILED",
      .detail = {},
  });

  CHECK_FALSE(report.passed());
  CHECK(report.n_passed() == 1);
  CHECK(report.n_failed() == 1);
}

TEST_CASE("SolverTestReport - all passing")  // NOLINT
{
  using namespace gtopt;

  SolverTestReport report;
  report.solver = "dummy";
  for (int i = 0; i < 5; ++i) {
    report.results.push_back(SolverTestResult {
        .name = std::to_string(i),
        .passed = true,
        .message = "ok",
        .detail = {},
    });
  }
  CHECK(report.passed());
  CHECK(report.n_passed() == 5);
  CHECK(report.n_failed() == 0);
}

// ---------------------------------------------------------------------------
// run_solver_tests with the default solver
// ---------------------------------------------------------------------------

TEST_CASE("run_solver_tests - default solver - all tests pass")  // NOLINT
{
  using namespace gtopt;

  const auto solver = default_solver_or_skip();
  if (solver.empty()) {
    MESSAGE("No solver plugins available — skipping solver tests");
    return;
  }

  const auto report = run_solver_tests(solver, /*verbose=*/false);

  CHECK(report.solver == solver);
  CHECK_FALSE(report.results.empty());

  for (const auto& r : report.results) {
    INFO("Test: " << r.name << " — " << r.detail);
    CHECK(r.passed);
  }

  CHECK(report.passed());
  CHECK(report.n_failed() == 0);
}

// ---------------------------------------------------------------------------
// Each individual test case by name (so failures show up individually in CI)
// ---------------------------------------------------------------------------

namespace
{

void run_named_test(const std::string& test_name)
{
  const auto solver = default_solver_or_skip();
  if (solver.empty()) {
    MESSAGE("No solver plugins — skipping " << test_name);
    return;
  }
  const auto report = run_solver_tests(solver, /*verbose=*/false);
  for (const auto& r : report.results) {
    if (r.name == test_name) {
      INFO("detail: " << r.detail);
      CHECK(r.passed);
      return;
    }
  }
  // Test name not found in report — shouldn't happen.
  FAIL("test '" << test_name << "' not found in report");
}

}  // namespace

TEST_CASE("check_solvers - construction")  // NOLINT
{
  using namespace gtopt;

  run_named_test("construction");
}
TEST_CASE("check_solvers - add_col")  // NOLINT
{
  using namespace gtopt;

  run_named_test("add_col");
}
TEST_CASE("check_solvers - add_row")  // NOLINT
{
  using namespace gtopt;

  run_named_test("add_row");
}
// Bulk-row / bulk-column addition uses a different per-backend code
// path than the single-row / single-column variants: CPLEX / HiGHS use
// native CSR/CSC APIs, while OSI/CLP+CBC, MindOpt, and Gurobi route
// through their own bulk APIs (added in the recent backend fix).
// We exercise the full matrix of backends so any bulk-path regression
// on any plugin shows up immediately.
//
// Splitting one TEST_CASE per (solver, test) means ctest -j20 can
// dispatch the per-solver invocations in parallel — wall-time for the
// combined add_rows + add_cols matrix drops from one 30s+ monolithic
// case to ~N parallel ~5-9s cases.  Each gate uses has_solver() so an
// installation that lacks a plugin emits MESSAGE and passes without
// CHECK noise.
//
// One TEST_CASE per (solver, base_name) pair — explicit instead of
// X-macro-expanded so each TEST_CASE sits on its own source line.
// doctest derives anonymous registrar identifiers from `__LINE__`;
// expanding multiple TEST_CASEs from a single macro invocation
// produces DOCTEST_ANON_VAR_<line> redefinition errors.  The
// `MESSAGE`-on-missing-plugin gating lives inside
// `run_named_test_on_solver`; missing backends pass cleanly without
// firing `CHECK`s.
//
// Mirrors the known set of backend plugins
// (`libgtopt_solver_<name>.so`).  Add a new solver: append the
// matching ``TEST_CASE("check_solvers - add_rows [solver=X]")`` and
// ``... [solver=X]`` for ``add_cols`` below.
//
TEST_CASE("check_solvers - add_rows [solver=clp]")
{
  using namespace gtopt;
  run_named_test_on_solver("clp", "add_rows");
}
TEST_CASE("check_solvers - add_rows [solver=cbc]")
{
  using namespace gtopt;
  run_named_test_on_solver("cbc", "add_rows");
}
TEST_CASE("check_solvers - add_rows [solver=cplex]")
{
  using namespace gtopt;
  run_named_test_on_solver("cplex", "add_rows");
}
TEST_CASE("check_solvers - add_rows [solver=highs]")
{
  using namespace gtopt;
  run_named_test_on_solver("highs", "add_rows");
}
TEST_CASE("check_solvers - add_rows [solver=mindopt]")
{
  using namespace gtopt;
  run_named_test_on_solver("mindopt", "add_rows");
}
TEST_CASE("check_solvers - add_rows [solver=gurobi]")
{
  using namespace gtopt;
  run_named_test_on_solver("gurobi", "add_rows");
}
TEST_CASE("check_solvers - add_rows [solver=osi]")
{
  using namespace gtopt;
  run_named_test_on_solver("osi", "add_rows");
}
TEST_CASE("check_solvers - add_rows [solver=cuopt]")
{
  using namespace gtopt;
  run_named_test_on_solver("cuopt", "add_rows");
}
TEST_CASE("check_solvers - add_cols [solver=clp]")
{
  using namespace gtopt;
  run_named_test_on_solver("clp", "add_cols");
}
TEST_CASE("check_solvers - add_cols [solver=cbc]")
{
  using namespace gtopt;
  run_named_test_on_solver("cbc", "add_cols");
}
TEST_CASE("check_solvers - add_cols [solver=cplex]")
{
  using namespace gtopt;
  run_named_test_on_solver("cplex", "add_cols");
}
TEST_CASE("check_solvers - add_cols [solver=highs]")
{
  using namespace gtopt;
  run_named_test_on_solver("highs", "add_cols");
}
TEST_CASE("check_solvers - add_cols [solver=mindopt]")
{
  using namespace gtopt;
  run_named_test_on_solver("mindopt", "add_cols");
}
TEST_CASE("check_solvers - add_cols [solver=gurobi]")
{
  using namespace gtopt;
  run_named_test_on_solver("gurobi", "add_cols");
}
TEST_CASE("check_solvers - add_cols [solver=osi]")
{
  using namespace gtopt;
  run_named_test_on_solver("osi", "add_cols");
}
TEST_CASE("check_solvers - add_cols [solver=cuopt]")
{
  using namespace gtopt;
  run_named_test_on_solver("cuopt", "add_cols");
}
TEST_CASE("check_solvers - obj_coeff")  // NOLINT
{
  using namespace gtopt;

  run_named_test("obj_coeff");
}
TEST_CASE("check_solvers - get_set_coeff")  // NOLINT
{
  using namespace gtopt;

  run_named_test("get_set_coeff");
}
TEST_CASE("check_solvers - variable_types")  // NOLINT
{
  using namespace gtopt;

  run_named_test("variable_types");
}
TEST_CASE("check_solvers - name_maps")  // NOLINT
{
  using namespace gtopt;

  run_named_test("name_maps");
}
TEST_CASE("check_solvers - load_flat_stats")  // NOLINT
{
  using namespace gtopt;

  run_named_test("load_flat_stats");
}
TEST_CASE("check_solvers - initial_solve_optimal")  // NOLINT
{
  using namespace gtopt;

  run_named_test("initial_solve_optimal");
}
TEST_CASE("check_solvers - primal_infeasible")  // NOLINT
{
  using namespace gtopt;

  run_named_test("primal_infeasible");
}
TEST_CASE("check_solvers - resolve")  // NOLINT
{
  using namespace gtopt;

  run_named_test("resolve");
}
TEST_CASE("check_solvers - clone")  // NOLINT
{
  using namespace gtopt;

  run_named_test("clone");
}
TEST_CASE("check_solvers - base_numrows_reset")  // NOLINT
{
  using namespace gtopt;

  run_named_test("base_numrows_reset");
}
TEST_CASE("check_solvers - write_lp")  // NOLINT
{
  using namespace gtopt;

  run_named_test("write_lp");
}
TEST_CASE("check_solvers - maximisation")  // NOLINT
{
  using namespace gtopt;

  run_named_test("maximisation");
}
TEST_CASE("check_solvers - col_scales")  // NOLINT
{
  using namespace gtopt;

  run_named_test("col_scales");
}
TEST_CASE("check_solvers - barrier_threads")  // NOLINT
{
  using namespace gtopt;

  run_named_test("barrier_threads");
}
TEST_CASE("check_solvers - barrier_resolve")  // NOLINT
{
  using namespace gtopt;

  run_named_test("barrier_resolve");
}
TEST_CASE("check_solvers - advanced_basis")  // NOLINT
{
  using namespace gtopt;

  // Validates the SolverOptions::advanced_basis warm-start mapping per
  // backend (cold solve → bound change → warm re-solve → same optimum).
  // `check_all_solvers` below runs the full self-test suite — including
  // this one — against EVERY available solver plugin.
  //
  // Ensure plugins are loaded so this entry actually runs in isolation
  // (`run_named_test` resolves the default solver, which is empty until
  // the plugins are loaded).
  SolverRegistry::instance().load_all_plugins();
  run_named_test("advanced_basis");
}

// ---------------------------------------------------------------------------
// check_all_solvers smoke test (only verifies exit code)
// ---------------------------------------------------------------------------

TEST_CASE("check_all_solvers - returns 0 when all pass")  // NOLINT
{
  using namespace gtopt;

  if (default_solver_or_skip().empty()) {
    MESSAGE("No solver plugins — skipping check_all_solvers");
    return;
  }
  const int rc = check_all_solvers(/*verbose=*/false);
  CHECK(rc == 0);
}
