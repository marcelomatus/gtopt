// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_ensure_duals.cpp
 * @brief     Tests for LinearInterface::ensure_duals (lazy crossover)
 * @date      2026-04-05
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_options.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("LinearInterface - ensure_duals no-op before solve")  // NOLINT
{
  // get_row_dual_raw() on an empty LP (no rows, no solve) should be safe
  // because ensure_duals() is a no-op when m_last_solver_options_ has the
  // default algorithm (not barrier) and crossover=true.
  LinearInterface li;
  const auto duals = li.get_row_dual_raw();
  CHECK(duals.empty());
}

TEST_CASE("LinearInterface - ensure_duals no-op for simplex solve")  // NOLINT
{
  // Simplex solve: ensure_duals() should be a no-op (algorithm != barrier).
  // Duals should be available directly from the solve.
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.lowb = 5.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto res = li.initial_solve(SolverOptions {
      .algorithm = LPAlgo::dual,
      .log_level = 0,
  });
  REQUIRE(res.has_value());

  const auto duals = li.get_row_dual_raw();
  CHECK(duals.size() == 1);
  // The binding constraint x1+x2 >= 5 should have a non-zero dual
  CHECK(duals[0] != doctest::Approx(0.0));
}

TEST_CASE(
    "LinearInterface - get_row_dual after adding row and resolving")  // NOLINT
{
  // Solve, add a row, resolve, then get duals — duals should reflect
  // the new constraint set.
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row1;
  row1[x1] = 1.0;
  row1[x2] = 1.0;
  row1.lowb = 5.0;
  row1.uppb = LinearProblem::DblMax;
  li.add_row(row1);

  auto res1 = li.initial_solve(SolverOptions {
      .log_level = 0,
  });
  REQUIRE(res1.has_value());
  CHECK(li.get_numrows() == 1);

  // Add a new binding constraint: x1 <= 3
  SparseRow row2;
  row2[x1] = 1.0;
  row2.uppb = 3.0;
  li.add_row(row2);
  CHECK(li.get_numrows() == 2);

  // Resolve
  auto res2 = li.resolve(SolverOptions {
      .log_level = 0,
  });
  REQUIRE(res2.has_value());

  const auto duals = li.get_row_dual_raw();
  CHECK(duals.size() == 2);
  // Optimal: x1=0, x2=5 (minimize 2x1+x2 ≥ 5, x1 ≤ 3)
  CHECK(li.get_col_sol()[x2] == doctest::Approx(5.0));
}

TEST_CASE("LinearInterface - get_row_dual on infeasible problem")  // NOLINT
{
  // After an infeasible solve, get_row_dual_raw() should still be safe
  // to call (returns whatever the solver has — likely zeros).
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 5.0,
      .cost = 1.0,
  });

  // Infeasible: x1 >= 10 but x1 <= 5
  SparseRow row;
  row[x1] = 1.0;
  row.lowb = 10.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto result = li.initial_solve(SolverOptions {
      .log_level = 0,
  });
  REQUIRE_FALSE(result.has_value());

  // Should not crash — just returns solver's internal dual state
  const auto duals = li.get_row_dual_raw();
  CHECK(duals.size() == 1);
}

TEST_CASE(
    "LinearInterface - ensure_duals with barrier crossover=true")  // NOLINT
{
  // Barrier with crossover=true should already have duals — ensure_duals()
  // should be a no-op.
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.lowb = 5.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto res = li.initial_solve(SolverOptions {
      .algorithm = LPAlgo::barrier,
      .log_level = 0,
      .crossover = true,
  });
  REQUIRE(res.has_value());

  const auto duals = li.get_row_dual_raw();
  CHECK(duals.size() == 1);
  // Binding constraint should have a non-zero dual
  CHECK(duals[0] != doctest::Approx(0.0));
}

TEST_CASE(
    "LinearInterface - lazy crossover with barrier crossover=false")  // NOLINT
{
  // Barrier with crossover=false: calling get_row_dual_raw() should
  // trigger ensure_duals() which re-solves with crossover=true.
  // The primal solution should remain the same; duals should be valid.
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.lowb = 5.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto res = li.initial_solve(SolverOptions {
      .algorithm = LPAlgo::barrier,
      .log_level = 0,
      .crossover = false,
  });
  REQUIRE(res.has_value());

  // Primal should be feasible before requesting duals
  CHECK(li.get_col_sol()[x1] == doctest::Approx(0.0).epsilon(0.01));

  // This call triggers lazy crossover
  const auto duals = li.get_row_dual_raw();
  CHECK(duals.size() == 1);
  // After crossover, duals should be proper vertex duals
  CHECK(duals[0] != doctest::Approx(0.0));

  // Primal solution should still be correct after crossover
  const auto sol = li.get_col_sol();
  CHECK(sol[x1] == doctest::Approx(0.0).epsilon(0.01));
  CHECK(sol[x2] == doctest::Approx(5.0).epsilon(0.01));

  // Second call to get_row_dual_raw() should NOT re-trigger crossover
  // (m_last_solver_options_.crossover was updated to true)
  const auto duals2 = li.get_row_dual_raw();
  CHECK(duals2[0] == doctest::Approx(duals[0]));
}

TEST_CASE(
    "LinearInterface - lazy crossover preserves solution after "
    "resolve")  // NOLINT
{
  // Solve with barrier+crossover, resolve with barrier w/o crossover,
  // then request duals (triggers lazy crossover on the resolve's solution).
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.lowb = 5.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  // Initial solve with crossover (normal)
  auto res1 = li.initial_solve(SolverOptions {
      .algorithm = LPAlgo::barrier,
      .log_level = 0,
      .crossover = true,
  });
  REQUIRE(res1.has_value());
  const auto initial_duals = li.get_row_dual_raw();
  CHECK(initial_duals[0] != doctest::Approx(0.0));

  // Tighten constraint: x1 + x2 >= 8
  li.set_row_low(
      RowIndex {
          0,
      },
      8.0);

  // Resolve without crossover
  auto res2 = li.resolve(SolverOptions {
      .algorithm = LPAlgo::barrier,
      .log_level = 0,
      .crossover = false,
  });
  REQUIRE(res2.has_value());

  // Request duals (triggers lazy crossover)
  const auto duals = li.get_row_dual_raw();
  CHECK(duals.size() == 1);
  CHECK(duals[0] != doctest::Approx(0.0));

  // Verify updated primal: x1=0, x2=8
  CHECK(li.get_col_sol()[x2] == doctest::Approx(8.0).epsilon(0.01));
}
