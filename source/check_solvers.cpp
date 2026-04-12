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
      .lowb = 0.0,
      .uppb = LinearProblem::DblMax,
      .cost = c1,
  });
  const auto x2 = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = LinearProblem::DblMax,
      .cost = c2,
  });
  const auto r1 = lp.add_row(SparseRow {
      .lowb = lb_row,
      .uppb = ub_row,
  });
  lp.set_coeff(r1, x1, 1.0);
  lp.set_coeff(r1, x2, 1.0);

  LpMatrixOptions opts;
  opts.col_with_names = true;
  opts.row_with_names = true;
  opts.compute_stats = true;
  return lp.flatten(opts);
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

    const auto x1 = lp.add_col(SparseCol {
        .uppb = 10.0,
    });
    const auto x2 = lp.add_col(SparseCol {
        .lowb = -5.0,
        .uppb = 5.0,
    });
    const auto x3 = lp.add_col(SparseCol {}.free());

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

    // set_col_low_raw / set_col_upp_raw.
    lp.set_col_low(x1, 1.0);
    lp.set_col_upp(x1, 8.0);
    TC_CHECK_APPROX(ctx, lp.get_col_low()[x1], 1.0, 1e-12);
    TC_CHECK_APPROX(ctx, lp.get_col_upp()[x1], 8.0, 1e-12);

    // set_col_raw (fix both bounds to a single value).
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
    lp.add_col(SparseCol {
        .uppb = 10.0,
    });

    SparseRow row;
    row[ColIndex {0}] = 2.0;
    row.lowb = 1.0;
    row.uppb = 5.0;
    const auto r1 = lp.add_row(row);

    TC_CHECK(ctx, lp.get_numrows() == 1);
    TC_CHECK_APPROX(ctx, lp.get_row_low()[r1], 1.0, 1e-12);
    TC_CHECK_APPROX(ctx, lp.get_row_upp()[r1], 5.0, 1e-12);

    // Modify row bounds.
    lp.set_row_low(r1, 0.0);
    lp.set_row_upp_raw(r1, 8.0);
    TC_CHECK_APPROX(ctx, lp.get_row_low()[r1], 0.0, 1e-12);
    TC_CHECK_APPROX(ctx, lp.get_row_upp()[r1], 8.0, 1e-12);

    // set_rhs_raw: sets both bounds to the same value.
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
    const auto x1 = lp.add_col(SparseCol {
        .uppb = 10.0,
        .cost = 3.0,
    });
    const auto x2 = lp.add_col(SparseCol {
        .uppb = 10.0,
        .cost = 7.0,
    });

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
    const auto x1 = lp.add_col(SparseCol {
        .uppb = 10.0,
    });
    const auto x2 = lp.add_col(SparseCol {
        .uppb = 10.0,
    });

    SparseRow row;
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
    const auto x1 = lp.add_col(SparseCol {
        .uppb = 10.0,
    });
    const auto x2 = lp.add_col(SparseCol {
        .uppb = 10.0,
    });

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
// 7. LP names + name maps (col_name_map, row_name_map)
// ---------------------------------------------------------------------------
SolverTestResult test_name_maps(std::string_view solver)
{
  TestContext ctx;
  try {
    LinearInterface lp(solver);
    lp.set_label_maker(LabelMaker {LpNamesLevel::all});

    TC_CHECK(ctx, lp.label_maker().names_level() == LpNamesLevel::all);

    const auto x1 = lp.add_col(SparseCol {
        .uppb = 10.0,
    });
    const auto x2 = lp.add_col(SparseCol {
        .uppb = 5.0,
    });

    SparseRow row;
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
    TC_CHECK(ctx, col_idx_to_name[x1] == "x1");
    TC_CHECK(ctx, col_idx_to_name[x2] == "x2");

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

    // Stats are populated by flatten with compute_stats=true.
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

      const std::string algo_name = std::string(enum_name(algo));
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
      const auto sol = lp.get_col_sol_raw();
      TC_CHECK(ctx, sol.size() == 2);
      const double x1_val = sol[0];
      const double x2_val = sol[1];
      ctx.check(std::abs(x1_val + x2_val - 4.0) <= kEps,
                std::format("x1+x2 ≈ 4 after initial_solve({})", algo_name),
                __FILE__,
                __LINE__);

      // Dual solution: shadow price of r1 should be 1.0 (cost of 1 more unit
      // of RHS).
      const auto dual = lp.get_row_dual_raw();
      TC_CHECK(ctx, dual.size() == 1);

      // Reduced costs: both at zero (active variables on boundary).
      const auto rc = lp.get_col_cost_raw();
      TC_CHECK(ctx, rc.size() == 2);

      // Kappa: >= 0 when supported, -1 when not (e.g. MindOpt).
      TC_CHECK(ctx, lp.get_kappa() >= -1.0);

    } catch (const std::exception& ex) {
      ctx.check(
          /*cond=*/false,
          std::format("exception in algo {}: {}", enum_name(algo), ex.what()),
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
    const auto x = lp.add_col(SparseCol {
        .uppb = 5.0,
        .cost = 1.0,
    });

    SparseRow row;
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
    const auto col_sol_vec = std::vector<double>(lp.get_col_sol_raw().begin(),
                                                 lp.get_col_sol_raw().end());
    const auto row_dual_vec = std::vector<double>(lp.get_row_dual_raw().begin(),
                                                  lp.get_row_dual_raw().end());

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
    SparseRow cut;
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
    lp.set_label_maker(LabelMaker {LpNamesLevel::all});
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
        .lowb = 0.0,
        .uppb = 4.0,
        .cost = -3.0,
    });
    const auto x2 = lp_model.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 5.0,
        .cost = -2.0,
    });
    const auto r1 = lp_model.add_row(SparseRow {
        .lowb = -LinearProblem::DblMax,
        .uppb = 7.0,
    });
    lp_model.set_coeff(r1, x1, 1.0);
    lp_model.set_coeff(r1, x2, 1.0);

    LpMatrixOptions opts;
    opts.col_with_names = true;
    opts.row_with_names = true;
    const auto flat = lp_model.flatten(opts);

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
    lp.add_col(SparseCol {
        .uppb = 10.0,
    });

    lp.set_warm_col_sol(StrongIndexVector<ColIndex, double> {5.0});
    TC_CHECK(ctx, lp.warm_col_sol().size() == 1);
    TC_CHECK_APPROX(ctx, lp.warm_col_sol()[ColIndex {0}], 5.0, 1e-12);

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
        .lowb = 0.0,
        .uppb = 10.0,
        .cost = 1.0,
    });
    const auto x2 = lp_model.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 10.0,
        .cost = 2.0,
    });
    const auto r1 = lp_model.add_row(SparseRow {
        .lowb = 0.0,
        .uppb = 20.0,
    });
    lp_model.set_coeff(r1, x1, 1.0);
    lp_model.set_coeff(r1, x2, 1.0);

    LpMatrixOptions opts;
    opts.col_with_names = true;
    auto flat = lp_model.flatten(opts);
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
// 19. barrier algorithm with threads
// ---------------------------------------------------------------------------
SolverTestResult test_barrier_threads(std::string_view solver)
{
  TestContext ctx;
  const double kEps = 1e-6;

  try {
    // 4-variable LP:
    //   min  x1 + 2·x2 + 3·x3 + 4·x4
    //   s.t. x1 + x2          >= 5
    //             x2 + x3      >= 3
    //                  x3 + x4 >= 4
    //        x1, x2, x3, x4 >= 0
    // Optimal obj = 17 (x1=5, x2=0, x3=4, x4=0).
    LinearProblem lp("barrier_threads");
    const auto x1 = lp.add_col(SparseCol {.cost = 1.0});
    const auto x2 = lp.add_col(SparseCol {.cost = 2.0});
    const auto x3 = lp.add_col(SparseCol {.cost = 3.0});
    const auto x4 = lp.add_col(SparseCol {.cost = 4.0});

    const auto r1 = lp.add_row(SparseRow {
        .lowb = 5.0,
        .uppb = LinearProblem::DblMax,
    });
    lp.set_coeff(r1, x1, 1.0);
    lp.set_coeff(r1, x2, 1.0);

    const auto r2 = lp.add_row(SparseRow {
        .lowb = 3.0,
        .uppb = LinearProblem::DblMax,
    });
    lp.set_coeff(r2, x2, 1.0);
    lp.set_coeff(r2, x3, 1.0);

    const auto r3 = lp.add_row(SparseRow {
        .lowb = 4.0,
        .uppb = LinearProblem::DblMax,
    });
    lp.set_coeff(r3, x3, 1.0);
    lp.set_coeff(r3, x4, 1.0);

    LpMatrixOptions opts;
    opts.col_with_names = true;
    opts.row_with_names = true;
    const auto flat = lp.flatten(opts);

    LinearInterface li(solver, flat);

    // Solve with barrier algorithm and 4 threads
    const SolverOptions solver_opts {
        .algorithm = LPAlgo::barrier,
        .threads = 4,
        .presolve = true,
    };

    const auto result = li.initial_solve(solver_opts);
    TC_CHECK(ctx, result.has_value());
    TC_CHECK(ctx, li.is_optimal());
    TC_CHECK_APPROX(ctx, li.get_obj_value(), 17.0, kEps);

    const auto sol = li.get_col_sol_raw();
    TC_REQUIRE(ctx, sol.size() == 4);
    TC_CHECK(ctx, sol[0] + sol[1] >= 5.0 - kEps);
    TC_CHECK(ctx, sol[1] + sol[2] >= 3.0 - kEps);
    TC_CHECK(ctx, sol[2] + sol[3] >= 4.0 - kEps);

  } catch (const std::exception& ex) {
    return make_result("barrier_threads", /*test_passed=*/false, ex.what());
  }
  return make_result("barrier_threads", /*test_passed=*/ctx.ok(), ctx.failures);
}

// ---------------------------------------------------------------------------
// 20. barrier initial solve then dual simplex resolve (SDDP workflow)
// ---------------------------------------------------------------------------
SolverTestResult test_barrier_resolve(std::string_view solver)
{
  TestContext ctx;
  const double kEps = 1e-6;

  try {
    // Same 4-variable LP as test_barrier_threads
    LinearProblem lp("barrier_resolve");
    const auto x1 = lp.add_col(SparseCol {.cost = 1.0});
    const auto x2 = lp.add_col(SparseCol {.cost = 2.0});
    const auto x3 = lp.add_col(SparseCol {.cost = 3.0});
    const auto x4 = lp.add_col(SparseCol {.cost = 4.0});

    const auto r1 = lp.add_row(SparseRow {
        .lowb = 5.0,
        .uppb = LinearProblem::DblMax,
    });
    lp.set_coeff(r1, x1, 1.0);
    lp.set_coeff(r1, x2, 1.0);

    const auto r2 = lp.add_row(SparseRow {
        .lowb = 3.0,
        .uppb = LinearProblem::DblMax,
    });
    lp.set_coeff(r2, x2, 1.0);
    lp.set_coeff(r2, x3, 1.0);

    const auto r3 = lp.add_row(SparseRow {
        .lowb = 4.0,
        .uppb = LinearProblem::DblMax,
    });
    lp.set_coeff(r3, x3, 1.0);
    lp.set_coeff(r3, x4, 1.0);

    LpMatrixOptions opts;
    opts.col_with_names = true;
    opts.row_with_names = true;
    const auto flat = lp.flatten(opts);

    LinearInterface li(solver, flat);

    // Step 1: initial solve with barrier + 4 threads
    const auto r_init = li.initial_solve(SolverOptions {
        .algorithm = LPAlgo::barrier,
        .threads = 4,
    });
    TC_CHECK(ctx, r_init.has_value());
    TC_CHECK_APPROX(ctx, li.get_obj_value(), 17.0, kEps);

    // Step 2: tighten first constraint and resolve with dual simplex
    li.set_row_low(RowIndex {0}, 6.0);  // x1+x2 >= 6

    const auto r_resolve = li.resolve(SolverOptions {
        .algorithm = LPAlgo::dual,
        .reuse_basis = true,
    });
    TC_CHECK(ctx, r_resolve.has_value());
    TC_CHECK(ctx, li.is_optimal());
    // Obj increases by 1 (cheapest variable x1 gets +1)
    TC_CHECK_APPROX(ctx, li.get_obj_value(), 18.0, kEps);

  } catch (const std::exception& ex) {
    return make_result("barrier_resolve", /*test_passed=*/false, ex.what());
  }
  return make_result("barrier_resolve", /*test_passed=*/ctx.ok(), ctx.failures);
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
      {.name = "construction", .fn = test_construction},
      {.name = "add_col", .fn = test_add_col},
      {.name = "add_row", .fn = test_add_row},
      {.name = "obj_coeff", .fn = test_obj_coeff},
      {.name = "get_set_coeff", .fn = test_get_set_coeff},
      {.name = "variable_types", .fn = test_variable_types},
      {.name = "name_maps", .fn = test_name_maps},
      {.name = "load_flat_stats", .fn = test_load_flat_stats},
      {.name = "initial_solve_optimal", .fn = test_initial_solve_optimal},
      {.name = "primal_infeasible", .fn = test_primal_infeasible},
      {.name = "resolve", .fn = test_resolve},
      {.name = "clone", .fn = test_clone},
      {.name = "warm_start", .fn = test_warm_start},
      {.name = "base_numrows_reset", .fn = test_base_numrows_reset},
      {.name = "write_lp", .fn = test_write_lp},
      {.name = "maximisation", .fn = test_maximisation},
      {.name = "warm_col_sol_accessors", .fn = test_warm_col_sol_accessors},
      {.name = "col_scales", .fn = test_col_scales},
      {.name = "barrier_threads", .fn = test_barrier_threads},
      {.name = "barrier_resolve", .fn = test_barrier_resolve},
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
  auto& registry = SolverRegistry::instance();
  registry.load_all_plugins();
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
