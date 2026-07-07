// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_fixedmilp_per_solver.cpp
 * @brief     Per-solver tests for the warm MIP→dual fast path
 *            (SolverBackend::fix_mip_and_resolve_duals).
 * @date      2026-06-04
 * @author    claude
 * @copyright BSD-3-Clause
 *
 * `LinearInterface::fix_integers_and_resolve` delegates to the common
 * backend virtual `fix_mip_and_resolve_duals`, which fixes every integer
 * column to its incumbent value and re-solves as an LP to recover the
 * committed-solution row duals.  CPLEX overrides it with the single-call
 * `CPXPROB_FIXEDMILP` warm dual-simplex path; the other plugins inherit the
 * portable pin → relax → resolve default.  Both MUST produce the SAME
 * committed-solution primal + duals.
 *
 * This test builds one tiny MIP and runs the fix-and-recover pass on EVERY
 * loaded MIP-capable plugin (CPLEX / HiGHS / CBC-OSI / Gurobi / MindOpt),
 * asserting each agrees on the incumbent and recovers the same shadow price.
 * The closed-form fixture is non-degenerate so the dual is solver-independent:
 *
 *   min  10 x + 1 y      x >= 0 (continuous),  y in [0, 3] (integer)
 *   s.t.  x + y >= 5
 *
 *   MIP optimum: y = 3 (cheapest unit), x = 2, obj = 23.
 *   Fixing y = 3 and relaxing: x = 2 is basic (>0), the row binds, so the
 *   row shadow price is exactly 10 ($/unit of RHS) on every backend.
 */

#include <cmath>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;

TEST_CASE(
    "fix_mip_and_resolve_duals - committed duals per solver plugin")  // NOLINT
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  bool tested_any = false;
  for (const auto& name : reg.available_solvers()) {
    if (!reg.supports_mip(name)) {
      continue;  // LP-only backend (e.g. clp) — fix-integers is a no-op there
    }
    tested_any = true;
    CAPTURE(name);  // logs the failing plugin name under ctest

    LinearInterface lp(name);
    const auto x = lp.add_col(SparseCol {
        .lowb = 0.0,
        .cost = 10.0,
    });
    const auto y = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 3.0,
        .cost = 1.0,
    });
    lp.set_integer(y);

    SparseRow row;
    row[x] = 1.0;
    row[y] = 1.0;
    row.greater_equal(5.0);
    const auto row_idx = lp.add_row(row);

    const SolverOptions opts;
    auto solved = lp.initial_solve(opts);
    REQUIRE(solved.has_value());
    REQUIRE(solved.value() == 0);  // 0 = optimal
    REQUIRE(lp.is_optimal());
    REQUIRE(lp.has_integer_cols());

    // MIP optimum: y = 3, x = 2, obj = 23.
    {
      const auto sol = lp.get_col_sol();
      CHECK(sol[y] == doctest::Approx(3.0).epsilon(1e-6));
      CHECK(sol[x] == doctest::Approx(2.0).epsilon(1e-6));
    }
    CHECK(lp.get_obj_value() == doctest::Approx(23.0).epsilon(1e-6));

    // ── The fast path: fix integers to the incumbent and recover duals.
    // CPLEX → CPXPROB_FIXEDMILP warm dual simplex; others → portable default.
    auto fix = lp.fix_integers_and_resolve(opts);
    REQUIRE(fix.has_value());
    CHECK(fix->fixed_columns == 1);  // the single integer column y
    REQUIRE(fix->status.has_value());
    CHECK(*fix->status == 0);
    REQUIRE(lp.is_optimal());

    // The integer column is now continuous (relaxed) but pinned by bounds.
    CHECK(!lp.has_integer_cols());
    CHECK(!lp.is_integer(y));

    // Primal is unchanged — the committed (MIP-optimal) solution.
    {
      const auto sol = lp.get_col_sol();
      CHECK(sol[y] == doctest::Approx(3.0).epsilon(1e-6));
      CHECK(sol[x] == doctest::Approx(2.0).epsilon(1e-6));
    }
    CHECK(lp.get_obj_value() == doctest::Approx(23.0).epsilon(1e-6));

    // Duals are now well-defined (a MIP exposes none).  The row binds and
    // x is basic, so the shadow price is exactly 10 on every backend.
    const auto duals = lp.get_row_dual();
    REQUIRE(duals.size() >= 1);
    CHECK(std::isfinite(duals[row_idx]));
    CHECK(std::abs(duals[row_idx]) == doctest::Approx(10.0).epsilon(1e-4));
  }

  if (!tested_any) {
    MESSAGE("no MIP-capable solver plugin loaded — skipping per-solver test");
  }
}
