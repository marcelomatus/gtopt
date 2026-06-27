// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_mip_start_per_solver.cpp
 * @brief     Per-solver tests for the integer initial-solution injection
 *            (SolverBackend::set_mip_start).
 * @date      2026-06-27
 * @author    claude
 * @copyright BSD-3-Clause
 *
 * `LinearInterface::set_mip_start` delegates to the backend virtual
 * `set_mip_start`, which hands a dense raw-space column vector to the
 * solver's native MIP-start channel:
 *   - CPLEX   : CPXaddmipstarts
 *   - HiGHS   : Highs::setSolution
 *   - Gurobi  : GRB_DBL_ATTR_START
 *   - MindOpt : MDO_DBL_ATTR_START
 *   - cuOpt   : cuOptAddMIPStart (buffer-and-replay; presolve forced off)
 *   - CBC     : CbcModel::setBestSolution (buffer-and-replay before B&B)
 * CLP is LP-only and declines (returns false), as does any backend that
 * does not override the default.
 *
 * Same closed-form fixture as test_fixedmilp_per_solver:
 *   min  10 x + 1 y      x >= 0 (continuous),  y in [0, 3] (integer)
 *   s.t.  x + y >= 5      ⇒  MIP optimum y = 3, x = 2, obj = 23.
 *
 * Each MIP-capable plugin must (a) accept a correctly-sized start at the
 * optimum, (b) reject a wrong-sized one, and (c) re-solve to the same
 * optimum with the start installed (so the per-backend replay path runs).
 */

#include <array>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_enums.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE(
    "set_mip_start - accepted + re-solves to optimum per MIP plugin")  // NOLINT
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  bool tested_any = false;
  for (const auto& name : reg.available_solvers()) {
    if (!reg.supports_mip(name)) {
      continue;  // LP-only backend (e.g. clp) — covered by the decline test
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
    [[maybe_unused]] const auto row_idx = lp.add_row(row);

    const SolverOptions opts;
    REQUIRE(lp.initial_solve(opts).has_value());  // loads + solves the MIP
    REQUIRE(lp.is_optimal());
    REQUIRE(lp.has_integer_cols());
    CHECK(lp.get_obj_value() == doctest::Approx(23.0).epsilon(1e-6));

    // A wrong-sized start is rejected (size must equal num cols == 2).
    const std::array<double, 1> bad {3.0};
    CHECK_FALSE(lp.set_mip_start(bad, MipStartEffort::check_feasibility));

    // A dense start at the MIP optimum (x=2, y=3) is accepted by every
    // MIP-capable backend.
    const std::array<double, 2> start {2.0, 3.0};
    CHECK(lp.set_mip_start(start, MipStartEffort::check_feasibility));

    // Re-solve with the start installed: the per-backend replay path runs
    // (CBC setBestSolution / cuOpt cuOptAddMIPStart / Start attribute) and the
    // optimum is unchanged.
    REQUIRE(lp.resolve(opts).has_value());
    REQUIRE(lp.is_optimal());
    CHECK(lp.get_obj_value() == doctest::Approx(23.0).epsilon(1e-6));
    {
      const auto sol = lp.get_col_sol();
      CHECK(sol[y] == doctest::Approx(3.0).epsilon(1e-6));
      CHECK(sol[x] == doctest::Approx(2.0).epsilon(1e-6));
    }
  }

  if (!tested_any) {
    MESSAGE("no MIP-capable solver plugin loaded — skipping per-solver test");
  }
}

TEST_CASE("set_mip_start - LP-only backend (clp) declines")  // NOLINT
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  if (!reg.has_solver("clp")) {
    MESSAGE("clp plugin not loaded — skipping LP-only decline test");
    return;
  }

  LinearInterface lp("clp");
  const auto x = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0,
      .cost = -1.0,
  });
  SparseRow row;
  row[x] = 1.0;
  row.less_equal(1.0);
  [[maybe_unused]] const auto row_idx = lp.add_row(row);
  REQUIRE(lp.initial_solve(SolverOptions {}).has_value());

  // CLP is LP-only: there is no MIP-start channel, so the backend declines.
  const std::array<double, 1> start {1.0};
  CHECK_FALSE(lp.set_mip_start(start, MipStartEffort::check_feasibility));
}
