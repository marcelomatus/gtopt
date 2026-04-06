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

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

using namespace gtopt;  // NOLINT(google-build-using-namespace)

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

}  // namespace

// ---------------------------------------------------------------------------
// SolverTestReport helpers
// ---------------------------------------------------------------------------

TEST_CASE("SolverTestReport - passed() / n_passed() / n_failed()")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("construction");
}
TEST_CASE("check_solvers - add_col")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("add_col");
}
TEST_CASE("check_solvers - add_row")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("add_row");
}
TEST_CASE("check_solvers - obj_coeff")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("obj_coeff");
}
TEST_CASE("check_solvers - get_set_coeff")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("get_set_coeff");
}
TEST_CASE("check_solvers - variable_types")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("variable_types");
}
TEST_CASE("check_solvers - name_maps")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("name_maps");
}
TEST_CASE("check_solvers - load_flat_stats")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("load_flat_stats");
}
TEST_CASE("check_solvers - initial_solve_optimal")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("initial_solve_optimal");
}
TEST_CASE("check_solvers - primal_infeasible")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("primal_infeasible");
}
TEST_CASE("check_solvers - resolve")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("resolve");
}
TEST_CASE("check_solvers - clone")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("clone");
}
TEST_CASE("check_solvers - warm_start")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("warm_start");
}
TEST_CASE("check_solvers - base_numrows_reset")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("base_numrows_reset");
}
TEST_CASE("check_solvers - write_lp")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("write_lp");
}
TEST_CASE("check_solvers - maximisation")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("maximisation");
}
TEST_CASE("check_solvers - warm_col_sol_accessors")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("warm_col_sol_accessors");
}
TEST_CASE("check_solvers - col_scales")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("col_scales");
}
TEST_CASE("check_solvers - barrier_threads")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("barrier_threads");
}
TEST_CASE("check_solvers - barrier_resolve")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  run_named_test("barrier_resolve");
}

// ---------------------------------------------------------------------------
// check_all_solvers smoke test (only verifies exit code)
// ---------------------------------------------------------------------------

TEST_CASE("check_all_solvers - returns 0 when all pass")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  if (default_solver_or_skip().empty()) {
    MESSAGE("No solver plugins — skipping check_all_solvers");
    return;
  }
  const int rc = check_all_solvers(/*verbose=*/false);
  CHECK(rc == 0);
}
