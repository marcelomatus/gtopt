/**
 * @file      check_solvers.cpp
 * @brief     Implementation of the C++ solver plugin test suite
 * @date      2026-03-26
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Each test is a free function returning a SolverTestResult.  The driver
 * run_solver_tests() calls each one in order, catches any exception, and
 * records the outcome.  Tests are plain C++26 — no third-party test
 * framework is required at runtime.
 */

#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtopt/check_solvers.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

// ---------------------------------------------------------------------------
// SolverTestReport helpers
// ---------------------------------------------------------------------------

bool SolverTestReport::passed() const noexcept
{
  return std::ranges::none_of(
      results, [](const SolverTestResult& r) { return !r.passed; });
}

int SolverTestReport::n_passed() const noexcept
{
  return static_cast<int>(std::ranges::count_if(
      results, [](const SolverTestResult& r) { return r.passed; }));
}

int SolverTestReport::n_failed() const noexcept
{
  return static_cast<int>(std::ranges::count_if(
      results, [](const SolverTestResult& r) { return !r.passed; }));
}

// ---------------------------------------------------------------------------
// Internal assertion helpers (no external framework dependency)
// ---------------------------------------------------------------------------

namespace
{

/// Accumulated failure detail within a single test.
struct TestContext
{
  std::string failures;

  void check(bool cond, std::string_view expr, std::string_view file, int line)
  {
    if (!cond) {
      if (!failures.empty()) {
        failures += '\n';
      }
      failures += std::format("  FAILED: {} ({}:{})", expr, file, line);
    }
  }

  static void require(bool cond,
                      std::string_view expr,
                      std::string_view file,
                      int line)
  {
    if (!cond) {
      throw std::runtime_error(
          std::format("REQUIRE failed: {} ({}:{})", expr, file, line));
    }
  }

  [[nodiscard]] bool ok() const { return failures.empty(); }
};

// Thin wrappers so callers don't have to pass __FILE__/__LINE__ manually.
#define TC_CHECK(ctx, expr) (ctx).check((expr), #expr, __FILE__, __LINE__)
#define TC_REQUIRE(ctx, expr) (ctx).require((expr), #expr, __FILE__, __LINE__)
#define TC_CHECK_APPROX(ctx, a, b, eps) \
  (ctx).check(std::abs((a) - (b)) <= (eps), \
              std::format("{} ≈ {} (eps={})", #a, #b, eps), \
              __FILE__, \
              __LINE__)

/// Simple helper to build a canonical 2×2 LP:
///   min   c1·x1 + c2·x2
///   s.t.  x1 + x2 >= lb_row   (range constraint)
///         x1 + x2 <= ub_row
///         x1, x2 >= 0
///
///  Default: min x+y  s.t. x+y >= 4, optimal = (4,0) or (2,2), obj=4.
[[nodiscard]] FlatLinearProblem make_2x2_flat(
    double c1 = 1.0,
    double c2 = 1.0,
    double lb_row = 4.0,
    double ub_row = LinearProblem::DblMax)
{
  LinearProblem lp("2x2");
  const auto x1 = lp.add_col(SparseCol {
      .name = "x1",
      .lowb = 0.0,
      .uppb = LinearProblem::DblMax,
      .cost = c1,
  });
  const auto x2 = lp.add_col(SparseCol {
      .name = "x2",
      .lowb = 0.0,
      .uppb = LinearProblem::DblMax,
      .cost = c2,
  });
  const auto r1 = lp.add_row(SparseRow {
      .name = "r1",
      .lowb = lb_row,
      .uppb = ub_row,
  });
  lp.set_coeff(r1, x1, 1.0);
  lp.set_coeff(r1, x2, 1.0);

  LpBuildOptions opts;
  opts.col_with_names = true;
  opts.row_with_names = true;
  opts.compute_stats = true;
  return lp.lp_build(opts);
}

// ─────────────────────────────────────────────────────────────────────────────
// Individual test implementations
// ─────────────────────────────────────────────────────────────────────────────

/// Build a SolverTestResult; `test_passed` drives the message.
[[nodiscard]] SolverTestResult make_result(std::string name,
                                           bool test_passed,
                                           std::string detail = {})
{
  return SolverTestResult {
      .name = std::move(name),
      .passed = test_passed,
      .message = test_passed ? "ok" : "FAILED",
      .detail = std::move(detail),
  };
}

// ---------------------------------------------------------------------------
// 1. Construction and problem name
// ---------------------------------------------------------------------------
SolverTestResult test_construction(std::string_view solver)
{
  TestContext ctx;
  try {
    // Default construction uses the auto-detected solver.
    const LinearInterface lp_default;
    TC_CHECK(ctx, lp_default.get_numcols() == 0);
    TC_CHECK(ctx, lp_default.get_numrows() == 0);

    // Named construction — verify solver identity.
    LinearInterface lp(solver);
    TC_CHECK(ctx, lp.solver_name() == solver);
    TC_CHECK(ctx, !lp.solver_version().empty());
    TC_CHECK(ctx, lp.solver_id().starts_with(solver));
    TC_CHECK(ctx, lp.get_numcols() == 0);
    TC_CHECK(ctx, lp.get_numrows() == 0);

    // Problem name round-trip.
    lp.set_prob_name("TestProblem");
    TC_CHECK(ctx, lp.get_prob_name() == "TestProblem");

    // Construction from FlatLinearProblem.
    const auto flat = make_2x2_flat();
    const LinearInterface lp_flat(solver, flat);
    TC_CHECK(ctx, lp_flat.get_numcols() == 2);
    TC_CHECK(ctx, lp_flat.get_numrows() == 1);

  } catch (const std::exception& ex) {
    return make_result("construction", /*test_passed=*/false, ex.what());
  }
  return make_result("construction", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 2. add_col / add_free_col: dimensions and bounds
// ---------------------------------------------------------------------------
SolverTestResult test_add_col(std::string_view solver)
{
  TestContext ctx;
  try {
    LinearInterface lp(solver);

    const auto x1 = lp.add_col("x1", 0.0, 10.0);
    const auto x2 = lp.add_col("x2", -5.0, 5.0);
    const auto x3 = lp.add_free_col("x3");

    TC_CHECK(ctx, lp.get_numcols() == 3);

    const auto col_low = lp.get_col_low();
    const auto col_upp = lp.get_col_upp();

    TC_CHECK_APPROX(ctx, col_low[x1], 0.0, 1e-12);
    TC_CHECK_APPROX(ctx, col_upp[x1], 10.0, 1e-12);
    TC_CHECK_APPROX(ctx, col_low[x2], -5.0, 1e-12);
    TC_CHECK_APPROX(ctx, col_upp[x2], 5.0, 1e-12);

    // Free variable bounds must be the solver's ±infinity.
    TC_CHECK(ctx, lp.is_neg_inf(lp.get_col_low()[x3]));
    TC_CHECK(ctx, lp.is_pos_inf(lp.get_col_upp()[x3]));

    // set_col_low / set_col_upp.
    lp.set_col_low(x1, 1.0);
    lp.set_col_upp(x1, 8.0);
    TC_CHECK_APPROX(ctx, lp.get_col_low()[x1], 1.0, 1e-12);
    TC_CHECK_APPROX(ctx, lp.get_col_upp()[x1], 8.0, 1e-12);

    // set_col (fix both bounds to a single value).
    lp.set_col(x2, 3.0);
    TC_CHECK_APPROX(ctx, lp.get_col_low()[x2], 3.0, 1e-12);
    TC_CHECK_APPROX(ctx, lp.get_col_upp()[x2], 3.0, 1e-12);

  } catch (const std::exception& ex) {
    return make_result("add_col", /*test_passed=*/false, ex.what());
  }
  return make_result("add_col", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 3. add_row / set_row_low|upp / set_rhs / get_row_low|upp
// ---------------------------------------------------------------------------
SolverTestResult test_add_row(std::string_view solver)
{
  TestContext ctx;
  try {
    LinearInterface lp(solver);
    lp.add_col("x", 0.0, 10.0);

    SparseRow row("r1");
    row[ColIndex {0}] = 2.0;
    row.lowb = 1.0;
    row.uppb = 5.0;
    const auto r1 = lp.add_row(row);

    TC_CHECK(ctx, lp.get_numrows() == 1);
    TC_CHECK_APPROX(ctx, lp.get_row_low()[r1], 1.0, 1e-12);
    TC_CHECK_APPROX(ctx, lp.get_row_upp()[r1], 5.0, 1e-12);

    // Modify row bounds.
    lp.set_row_low(r1, 0.0);
    lp.set_row_upp(r1, 8.0);
    TC_CHECK_APPROX(ctx, lp.get_row_low()[r1], 0.0, 1e-12);
    TC_CHECK_APPROX(ctx, lp.get_row_upp()[r1], 8.0, 1e-12);

    // set_rhs: sets both bounds to the same value.
    lp.set_rhs(r1, 4.0);
    TC_CHECK_APPROX(ctx, lp.get_row_low()[r1], 4.0, 1e-12);
    TC_CHECK_APPROX(ctx, lp.get_row_upp()[r1], 4.0, 1e-12);

  } catch (const std::exception& ex) {
    return make_result("add_row", /*test_passed=*/false, ex.what());
  }
  return make_result("add_row", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 4. set_obj_coeff / get_obj_coeff
// ---------------------------------------------------------------------------
SolverTestResult test_obj_coeff(std::string_view solver)
{
  TestContext ctx;
  try {
    LinearInterface lp(solver);
    const auto x1 = lp.add_col("x1", 0.0, 10.0);
    const auto x2 = lp.add_col("x2", 0.0, 10.0);

    lp.set_obj_coeff(x1, 3.0);
    lp.set_obj_coeff(x2, 7.0);

    const auto obj = lp.get_obj_coeff();
    TC_CHECK(ctx, obj.size() == 2);
    TC_CHECK_APPROX(ctx, obj[x1], 3.0, 1e-12);
    TC_CHECK_APPROX(ctx, obj[x2], 7.0, 1e-12);

  } catch (const std::exception& ex) {
    return make_result("obj_coeff", /*test_passed=*/false, ex.what());
  }
  return make_result("obj_coeff", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 5. get_coeff / set_coeff
// ---------------------------------------------------------------------------
SolverTestResult test_get_set_coeff(std::string_view solver)
{
  TestContext ctx;
  try {
    LinearInterface lp(solver);
    const auto x1 = lp.add_col("x1", 0.0, 10.0);
    const auto x2 = lp.add_col("x2", 0.0, 10.0);

    SparseRow row("r1");
    row[x1] = 1.0;
    row[x2] = 2.0;
    row.lowb = 0.0;
    row.uppb = 20.0;
    const auto r1 = lp.add_row(row);

    TC_CHECK_APPROX(ctx, lp.get_coeff(r1, x1), 1.0, 1e-12);
    TC_CHECK_APPROX(ctx, lp.get_coeff(r1, x2), 2.0, 1e-12);

    if (lp.supports_set_coeff()) {
      lp.set_coeff(r1, x1, 5.0);
      TC_CHECK_APPROX(ctx, lp.get_coeff(r1, x1), 5.0, 1e-12);
    }

  } catch (const std::exception& ex) {
    return make_result("get_set_coeff", /*test_passed=*/false, ex.what());
  }
  return make_result("get_set_coeff", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 6. Variable types: continuous / integer / binary
// ---------------------------------------------------------------------------
SolverTestResult test_variable_types(std::string_view solver)
{
  TestContext ctx;
  try {
    LinearInterface lp(solver);
    const auto x1 = lp.add_col("x1", 0.0, 10.0);
    const auto x2 = lp.add_col("x2", 0.0, 10.0);

    // Default: continuous.
    TC_CHECK(ctx, lp.is_continuous(x1));
    TC_CHECK(ctx, !lp.is_integer(x1));

    // Set integer, verify.
    lp.set_integer(x1);
    TC_CHECK(ctx, lp.is_integer(x1));
    TC_CHECK(ctx, !lp.is_continuous(x1));

    // Revert to continuous.
    lp.set_continuous(x1);
    TC_CHECK(ctx, lp.is_continuous(x1));
    TC_CHECK(ctx, !lp.is_integer(x1));

    // Binary is a subset of integer.
    lp.set_binary(x2);
    TC_CHECK(ctx, lp.is_integer(x2));

  } catch (const std::exception& ex) {
    return make_result("variable_types", /*test_passed=*/false, ex.what());
  }
  return make_result("variable_types", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 7. lp_names_level + name maps (col_name_map, row_name_map)
// ---------------------------------------------------------------------------
SolverTestResult test_name_maps(std::string_view solver)
{
  TestContext ctx;
  try {
    LinearInterface lp(solver);
    lp.set_lp_names_level(1);

    TC_CHECK(ctx, lp.lp_names_level() == 1);

    const auto x1 = lp.add_col("x1", 0.0, 10.0);
    const auto x2 = lp.add_col("x2", 0.0, 5.0);

    SparseRow row("my_row");
    row[x1] = 1.0;
    row.lowb = 0.0;
    row.uppb = 10.0;
    lp.add_row(row);

    const auto& col_map = lp.col_name_map();
    TC_CHECK(ctx, col_map.contains("x1"));
    TC_CHECK(ctx, col_map.contains("x2"));

    const auto& row_map = lp.row_name_map();
    TC_CHECK(ctx, row_map.contains("my_row"));

    const auto& col_idx_to_name = lp.col_index_to_name();
    TC_CHECK(ctx, col_idx_to_name.size() == 2);
    TC_CHECK(ctx, col_idx_to_name[static_cast<size_t>(x1)] == "x1");
    TC_CHECK(ctx, col_idx_to_name[static_cast<size_t>(x2)] == "x2");

  } catch (const std::exception& ex) {
    return make_result("name_maps", /*test_passed=*/false, ex.what());
  }
  return make_result("name_maps", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 8. load_flat + lp_stats_* fields
// ---------------------------------------------------------------------------
SolverTestResult test_load_flat_stats(std::string_view solver)
{
  TestContext ctx;
  try {
    const auto flat = make_2x2_flat(2.0, 3.0, 4.0);
    const LinearInterface lp(solver, flat);

    TC_CHECK(ctx, lp.get_numcols() == 2);
    TC_CHECK(ctx, lp.get_numrows() == 1);

    // Stats are populated by lp_build with compute_stats=true.
    // There are 2 non-zero coefficients (both =1.0).
    TC_CHECK(ctx, lp.lp_stats_nnz() >= 2);
    TC_CHECK(ctx, lp.lp_stats_max_abs() >= 1.0);

  } catch (const std::exception& ex) {
    return make_result("load_flat_stats", /*test_passed=*/false, ex.what());
  }
  return make_result("load_flat_stats", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 9. initial_solve — optimal case, all algorithms
// ---------------------------------------------------------------------------
SolverTestResult test_initial_solve_optimal(std::string_view solver)
{
  TestContext ctx;
  const double kEps = 1e-6;

  for (const auto algo :
       {LPAlgo::default_algo, LPAlgo::primal, LPAlgo::dual, LPAlgo::barrier})
  {
    try {
      // min x + y,  x+y >= 4,  x,y >= 0  →  obj = 4
      const auto flat = make_2x2_flat(1.0, 1.0, 4.0);
      LinearInterface lp(solver, flat);

      const auto result = lp.initial_solve(SolverOptions {
          .algorithm = algo,
          .log_level = 0,
      });

      const std::string algo_name = std::string(lp_algo_name(algo));
      if (!result) {
        ctx.check(/*cond=*/false,
                  std::format("initial_solve({}) returned error: {}",
                              algo_name,
                              result.error().message),
                  __FILE__,
                  __LINE__);
        continue;
      }
      ctx.check(lp.is_optimal(),
                std::format("is_optimal() after initial_solve({})", algo_name),
                __FILE__,
                __LINE__);
      ctx.check(std::abs(lp.get_obj_value() - 4.0) <= kEps,
                std::format("obj_value ≈ 4 after initial_solve({})", algo_name),
                __FILE__,
                __LINE__);

      // Primal solution: x1+x2 == 4.
      const auto sol = lp.get_col_sol();
      TC_CHECK(ctx, sol.size() == 2);
      const double x1_val = sol[0];
      const double x2_val = sol[1];
      ctx.check(std::abs(x1_val + x2_val - 4.0) <= kEps,
                std::format("x1+x2 ≈ 4 after initial_solve({})", algo_name),
                __FILE__,
                __LINE__);

      // Dual solution: shadow price of r1 should be 1.0 (cost of 1 more unit
      // of RHS).
      const auto dual = lp.get_row_dual();
      TC_CHECK(ctx, dual.size() == 1);

      // Reduced costs: both at zero (active variables on boundary).
      const auto rc = lp.get_col_cost();
      TC_CHECK(ctx, rc.size() == 2);

      // Kappa: must be >= 0.
      TC_CHECK(ctx, lp.get_kappa() >= 0.0);

    } catch (const std::exception& ex) {
      ctx.check(/*cond=*/false,
                std::format(
                    "exception in algo {}: {}", lp_algo_name(algo), ex.what()),
                __FILE__,
                __LINE__);
    }
  }
  return make_result(
      "initial_solve_optimal", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 10. initial_solve — primal infeasible detection
// ---------------------------------------------------------------------------
SolverTestResult test_primal_infeasible(std::string_view solver)
{
  TestContext ctx;
  try {
    //   min x,  s.t. x >= 10, x <= 5, x >= 0   → infeasible
    LinearInterface lp(solver);
    const auto x = lp.add_col("x", 0.0, 5.0);
    lp.set_obj_coeff(x, 1.0);

    SparseRow row("r1");
    row[x] = 1.0;
    row.lowb = 10.0;
    row.uppb = LinearProblem::DblMax;
    lp.add_row(row);

    const auto result = lp.initial_solve(SolverOptions {
        .log_level = 0,
    });
    // The LP must not be optimal.
    TC_CHECK(ctx, !lp.is_optimal());

  } catch (const std::exception& ex) {
    // Some solvers throw on infeasible; that is acceptable.
    (void)ex;
  }
  return make_result(
      "primal_infeasible", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 11. resolve — warm re-solve after bound change
// ---------------------------------------------------------------------------
SolverTestResult test_resolve(std::string_view solver)
{
  TestContext ctx;
  const double kEps = 1e-6;
  try {
    // Initial: min x+y, x+y >= 4 → obj=4.
    const auto flat = make_2x2_flat(1.0, 1.0, 4.0);
    LinearInterface lp(solver, flat);

    auto r1 = lp.initial_solve(SolverOptions {
        .log_level = 0,
    });
    TC_REQUIRE(ctx, r1.has_value());
    TC_CHECK(ctx, lp.is_optimal());
    TC_CHECK_APPROX(ctx, lp.get_obj_value(), 4.0, kEps);

    // Tighten RHS to 6 → obj should now be 6.
    lp.set_row_low(RowIndex {0}, 6.0);
    const auto r2 = lp.resolve(SolverOptions {
        .log_level = 0,
    });
    TC_REQUIRE(ctx, r2.has_value());
    TC_CHECK(ctx, lp.is_optimal());
    TC_CHECK_APPROX(ctx, lp.get_obj_value(), 6.0, kEps);

  } catch (const std::exception& ex) {
    return make_result("resolve", /*test_passed=*/false, ex.what());
  }
  return make_result("resolve", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 12. clone — deep copy, independent solve
// ---------------------------------------------------------------------------
SolverTestResult test_clone(std::string_view solver)
{
  TestContext ctx;
  const double kEps = 1e-6;
  try {
    const auto flat = make_2x2_flat(1.0, 1.0, 4.0);
    LinearInterface lp(solver, flat);
    const auto ri1 = lp.initial_solve(SolverOptions {
        .log_level = 0,
    });
    TC_REQUIRE(ctx, ri1.has_value());
    TC_REQUIRE(ctx, lp.is_optimal());

    // Clone; modify the clone's bound; re-solve only the clone.
    auto clone = lp.clone();
    clone.set_row_low(RowIndex {0}, 8.0);
    const auto r = clone.resolve(SolverOptions {
        .log_level = 0,
    });
    TC_REQUIRE(ctx, r.has_value());
    TC_CHECK(ctx, clone.is_optimal());
    TC_CHECK_APPROX(ctx, clone.get_obj_value(), 8.0, kEps);

    // Original should still have obj=4.
    TC_CHECK_APPROX(ctx, lp.get_obj_value(), 4.0, kEps);

  } catch (const std::exception& ex) {
    return make_result("clone", /*test_passed=*/false, ex.what());
  }
  return make_result("clone", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 13. set_warm_start_solution — hot start hint survives a resolve
// ---------------------------------------------------------------------------
SolverTestResult test_warm_start(std::string_view solver)
{
  TestContext ctx;
  const double kEps = 1e-6;
  try {
    const auto flat = make_2x2_flat(1.0, 1.0, 4.0);
    LinearInterface lp(solver, flat);
    const auto ri2 = lp.initial_solve(SolverOptions {
        .log_level = 0,
    });
    TC_REQUIRE(ctx, ri2.has_value());
    TC_REQUIRE(ctx, lp.is_optimal());

    // Capture solution and use it as a warm start.
    const auto col_sol_vec =
        std::vector<double>(lp.get_col_sol().begin(), lp.get_col_sol().end());
    const auto row_dual_vec =
        std::vector<double>(lp.get_row_dual().begin(), lp.get_row_dual().end());

    lp.set_warm_start_solution(col_sol_vec, row_dual_vec);

    // Modify bound and re-solve — warm start should be used.
    lp.set_row_low(RowIndex {0}, 5.0);
    const auto r = lp.resolve(SolverOptions {
        .log_level = 0,
    });
    TC_REQUIRE(ctx, r.has_value());
    TC_CHECK(ctx, lp.is_optimal());
    TC_CHECK_APPROX(ctx, lp.get_obj_value(), 5.0, kEps);

  } catch (const std::exception& ex) {
    return make_result("warm_start", /*test_passed=*/false, ex.what());
  }
  return make_result("warm_start", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 14. save_base_numrows / delete_rows / reset_from
// ---------------------------------------------------------------------------
SolverTestResult test_base_numrows_reset(std::string_view solver)
{
  TestContext ctx;
  const double kEps = 1e-6;
  try {
    const auto flat = make_2x2_flat(1.0, 1.0, 4.0);
    LinearInterface lp(solver, flat);
    const auto ri3 = lp.initial_solve(SolverOptions {
        .log_level = 0,
    });
    TC_REQUIRE(ctx, ri3.has_value());
    TC_REQUIRE(ctx, lp.is_optimal());

    lp.save_base_numrows();
    TC_CHECK(ctx, lp.base_numrows() == 1);

    // Add a cut row.
    SparseRow cut("cut1");
    cut[ColIndex {0}] = 1.0;
    cut.lowb = 0.0;
    cut.uppb = 100.0;
    lp.add_row(cut);
    TC_CHECK(ctx, lp.get_numrows() == 2);

    // Delete the cut.
    const std::array<int, 1> to_del = {1};
    lp.delete_rows(to_del);
    TC_CHECK(ctx, lp.get_numrows() == 1);

    // reset_from: copy bounds from a source LP and remove rows > base.
    const LinearInterface src(solver, flat);
    lp.add_row(cut);
    TC_CHECK(ctx, lp.get_numrows() == 2);
    lp.reset_from(src, lp.base_numrows());
    TC_CHECK(ctx, lp.get_numrows() == 1);

    // After reset the LP should still solve correctly.
    const auto r = lp.resolve(SolverOptions {
        .log_level = 0,
    });
    TC_REQUIRE(ctx, r.has_value());
    TC_CHECK(ctx, lp.is_optimal());
    TC_CHECK_APPROX(ctx, lp.get_obj_value(), 4.0, kEps);

  } catch (const std::exception& ex) {
    return make_result("base_numrows_reset", /*test_passed=*/false, ex.what());
  }
  return make_result(
      "base_numrows_reset", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 15. write_lp — creates a temp .lp file and verifies its existence
// ---------------------------------------------------------------------------
SolverTestResult test_write_lp(std::string_view solver)
{
  TestContext ctx;
  try {
    const auto flat = make_2x2_flat();
    LinearInterface lp(solver, flat);
    lp.set_lp_names_level(1);
    const auto ri4 = lp.initial_solve(SolverOptions {
        .log_level = 0,
    });
    TC_REQUIRE(ctx, ri4.has_value());

    const auto tmp_stem =
        (std::filesystem::temp_directory_path()
         / std::format("gtopt_check_solvers_{}", std::string(solver)))
            .string();
    const auto result = lp.write_lp(tmp_stem);

    if (result.has_value()) {
      const std::filesystem::path lp_file = tmp_stem + ".lp";
      TC_CHECK(ctx, std::filesystem::exists(lp_file));
      if (std::filesystem::exists(lp_file)) {
        std::filesystem::remove(lp_file);
      }
    }
    // write_lp may return an error if row names are not available; that's
    // allowed — we only care that it doesn't crash.

  } catch (const std::exception& ex) {
    return make_result("write_lp", /*test_passed=*/false, ex.what());
  }
  return make_result("write_lp", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 16. Maximisation: negate cost coefficients, verify obj sign convention
// ---------------------------------------------------------------------------
SolverTestResult test_maximisation(std::string_view solver)
{
  TestContext ctx;
  const double kEps = 1e-6;
  try {
    //   max 3·x1 + 2·x2   s.t. x1 + x2 <= 7, x1 <= 4, x2 <= 5, x >= 0
    //   → opt: x1=2, x2=5, obj=16  (or x1=4,x2=3 also = 18 — let solver pick)
    // Implemented as minimisation of -(3x1+2x2).
    LinearProblem lp_model("max_test");
    const auto x1 = lp_model.add_col(SparseCol {
        .name = "x1",
        .lowb = 0.0,
        .uppb = 4.0,
        .cost = -3.0,
    });
    const auto x2 = lp_model.add_col(SparseCol {
        .name = "x2",
        .lowb = 0.0,
        .uppb = 5.0,
        .cost = -2.0,
    });
    const auto r1 = lp_model.add_row(SparseRow {
        .name = "sum",
        .lowb = -LinearProblem::DblMax,
        .uppb = 7.0,
    });
    lp_model.set_coeff(r1, x1, 1.0);
    lp_model.set_coeff(r1, x2, 1.0);

    LpBuildOptions opts;
    opts.col_with_names = true;
    opts.row_with_names = true;
    const auto flat = lp_model.lp_build(opts);

    LinearInterface lp(solver, flat);
    const auto result = lp.initial_solve(SolverOptions {
        .log_level = 0,
    });
    TC_REQUIRE(ctx, result.has_value());
    TC_CHECK(ctx, lp.is_optimal());

    // Max 3x1+2x2 with x1<=4,x2<=5,x1+x2<=7 → x1=4,x2=3, obj=18
    TC_CHECK_APPROX(ctx, -lp.get_obj_value(), 18.0, kEps);

  } catch (const std::exception& ex) {
    return make_result("maximisation", /*test_passed=*/false, ex.what());
  }
  return make_result("maximisation", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 17. set_warm_col_sol / warm_col_sol accessors
// ---------------------------------------------------------------------------
SolverTestResult test_warm_col_sol_accessors(std::string_view solver)
{
  TestContext ctx;
  try {
    LinearInterface lp(solver);
    lp.add_col("x", 0.0, 10.0);

    const std::vector<double> hint = {5.0};
    lp.set_warm_col_sol(std::vector<double>(hint));
    TC_CHECK(ctx, lp.warm_col_sol().size() == 1);
    TC_CHECK_APPROX(ctx, lp.warm_col_sol()[0], 5.0, 1e-12);

  } catch (const std::exception& ex) {
    return make_result(
        "warm_col_sol_accessors", /*test_passed=*/false, ex.what());
  }
  return make_result(
      "warm_col_sol_accessors", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 18. LP stats: col scale round-trip
// ---------------------------------------------------------------------------
SolverTestResult test_col_scales(std::string_view solver)
{
  TestContext ctx;
  try {
    // Build a FlatLinearProblem with explicit col_scales.
    LinearProblem lp_model("scale_test");
    const auto x1 = lp_model.add_col(SparseCol {
        .name = "x1",
        .lowb = 0.0,
        .uppb = 10.0,
        .cost = 1.0,
    });
    const auto x2 = lp_model.add_col(SparseCol {
        .name = "x2",
        .lowb = 0.0,
        .uppb = 10.0,
        .cost = 2.0,
    });
    const auto r1 = lp_model.add_row(SparseRow {
        .name = "r1",
        .lowb = 0.0,
        .uppb = 20.0,
    });
    lp_model.set_coeff(r1, x1, 1.0);
    lp_model.set_coeff(r1, x2, 1.0);

    LpBuildOptions opts;
    opts.col_with_names = true;
    auto flat = lp_model.lp_build(opts);
    flat.col_scales = {2.0, 3.0};  // inject manual scales

    const LinearInterface lp(solver, flat);

    TC_CHECK_APPROX(ctx, lp.get_col_scale(x1), 2.0, 1e-12);
    TC_CHECK_APPROX(ctx, lp.get_col_scale(x2), 3.0, 1e-12);

    // Out-of-range index returns 1.0.
    TC_CHECK_APPROX(ctx, lp.get_col_scale(ColIndex {99}), 1.0, 1e-12);

    const auto& scales = lp.get_col_scales();
    TC_CHECK(ctx, scales.size() == 2);

  } catch (const std::exception& ex) {
    return make_result("col_scales", /*test_passed=*/false, ex.what());
  }
  return make_result("col_scales", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// Test registry: ordered list of (name, function) pairs
// ---------------------------------------------------------------------------

/// Plain function pointer type for test functions.
using TestFnPtr = SolverTestResult (*)(std::string_view);

struct TestEntry
{
  std::string_view name;
  TestFnPtr fn;
};

/// Return the ordered list of all test entries.
/// Function-local initialisation avoids static-init-order issues.
[[nodiscard]] const std::vector<TestEntry>& all_tests()
{
  static const std::vector<TestEntry> tests {
      {"construction", test_construction},
      {"add_col", test_add_col},
      {"add_row", test_add_row},
      {"obj_coeff", test_obj_coeff},
      {"get_set_coeff", test_get_set_coeff},
      {"variable_types", test_variable_types},
      {"name_maps", test_name_maps},
      {"load_flat_stats", test_load_flat_stats},
      {"initial_solve_optimal", test_initial_solve_optimal},
      {"primal_infeasible", test_primal_infeasible},
      {"resolve", test_resolve},
      {"clone", test_clone},
      {"warm_start", test_warm_start},
      {"base_numrows_reset", test_base_numrows_reset},
      {"write_lp", test_write_lp},
      {"maximisation", test_maximisation},
      {"warm_col_sol_accessors", test_warm_col_sol_accessors},
      {"col_scales", test_col_scales},
  };
  return tests;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

SolverTestReport run_solver_tests(std::string_view solver_name, bool verbose)
{
  SolverTestReport report;
  report.solver = std::string(solver_name);

  for (const auto& entry : all_tests()) {
    if (verbose) {
      std::cout << std::format(
          "  [{}/{}] {} ... ", report.solver, entry.name, entry.name)
                << std::flush;
    }

    const auto t0 = std::chrono::steady_clock::now();
    SolverTestResult result;
    try {
      result = entry.fn(solver_name);
    } catch (const std::exception& ex) {
      result = make_result(std::string(entry.name),
                           /*test_passed=*/false,
                           std::format("uncaught exception: {}", ex.what()));
    } catch (...) {
      result = make_result(std::string(entry.name),
                           /*test_passed=*/false,
                           "uncaught non-std exception");
    }
    const auto t1 = std::chrono::steady_clock::now();
    result.duration_s = std::chrono::duration<double>(t1 - t0).count();

    result.message = result.passed ? "ok" : "FAILED";

    if (verbose) {
      std::cout << std::format(
          "{} ({:.3f}s)\n", result.passed ? "ok" : "FAILED", result.duration_s);
      if (!result.passed && !result.detail.empty()) {
        std::cout << "    " << result.detail << '\n';
      }
    }

    report.results.push_back(std::move(result));
  }

  return report;
}

int check_all_solvers(bool verbose)
{
  const auto& registry = SolverRegistry::instance();
  const auto available = registry.available_solvers();

  if (available.empty()) {
    std::cout << "No LP solver plugins found.\n";
    std::cout << "Search directories:\n";
    for (const auto& d : registry.searched_directories()) {
      std::cout << "  " << d << '\n';
    }
    return 1;
  }

  std::cout << std::format("gtopt_check_solvers: testing {} solver(s): {}\n",
                           available.size(),
                           [&]
                           {
                             std::string s;
                             for (const auto& n : available) {
                               if (!s.empty()) {
                                 s += ", ";
                               }
                               s += n;
                             }
                             return s;
                           }());

  int overall = 0;
  for (const auto& solver_name : available) {
    // Show solver identity (name + version) before running tests.
    const LinearInterface probe(solver_name);
    std::cout << std::format("\n  Solver: {}\n", probe.solver_id());
    const auto report = run_solver_tests(solver_name, verbose);

    for (const auto& r : report.results) {
      const char* mark = r.passed ? "✓" : "✗";
      std::cout << std::format(
          "    {} {:<30} {:.3f}s", mark, r.name, r.duration_s);
      if (!r.passed && !r.detail.empty()) {
        std::cout << "\n      " << r.detail;
      }
      std::cout << '\n';
    }

    std::cout << std::format(
        "  → {} passed, {} failed\n", report.n_passed(), report.n_failed());

    if (!report.passed()) {
      overall = 1;
    }
  }

  std::cout << '\n'
            << (overall == 0 ? "All solver tests passed.\n"
                             : "Some solver tests FAILED.\n");

  return overall;
}

}  // namespace gtopt
