// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_basis.cpp
 * @brief     Tests for the simplex-basis warm-start primitive
 * @date      2026-06-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Covers the solver-agnostic `Basis` / `reconcile_basis` helper (no solver
 * needed) and a backend round-trip through `LinearInterface::get_basis` /
 * `set_basis`, gated on the active backend actually supporting a basis
 * (CPLEX / HiGHS / OSI-CLP / Gurobi / MindOpt do; cuOpt PDLP and SCIP return
 * nullopt — the round-trip self-skips there).
 */

#include <doctest/doctest.h>
#include <gtopt/basis.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_options.hpp>

using namespace gtopt;

// Unique outer namespace (not anonymous) so the file-local helper survives
// the unity build without colliding with other test files.
namespace basis_test  // NOLINT(misc-use-anonymous-namespace)
{

namespace
{
/// Build the canonical tiny LP: maximize 3x1 + 2x2 (as minimize -3x1 - 2x2)
/// s.t. x1 + 2x2 <= 10, 0 <= x1 <= 4, 0 <= x2 <= 3.  Optimum x1=4, x2=3,
/// objective (max) 18.  Two columns, one row.
[[nodiscard]] LinearInterface make_tiny_lp()
{
  LinearInterface lp;
  const auto x1 = lp.add_col(SparseCol {.uppb = 4.0, .cost = -3.0});
  const auto x2 = lp.add_col(SparseCol {.uppb = 3.0, .cost = -2.0});
  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 2.0;
  row.uppb = 10.0;
  (void)lp.add_row(row);
  return lp;
}
}  // namespace

TEST_CASE("Basis - reconcile grows and truncates")  // NOLINT
{
  SUBCASE("grow appends nonbasic cols and basic rows")
  {
    Basis basis;
    basis.col_status = {BasisStatus::basic, BasisStatus::at_lower};
    basis.row_status = {BasisStatus::at_upper};

    reconcile_basis(basis, 4, 3);

    CHECK(basis.num_cols() == 4);
    CHECK(basis.num_rows() == 3);
    // Preserved prefix.
    CHECK(basis.col_status[0] == BasisStatus::basic);
    CHECK(basis.col_status[1] == BasisStatus::at_lower);
    CHECK(basis.row_status[0] == BasisStatus::at_upper);
    // Appended columns nonbasic at lower; appended rows basic (slack in).
    CHECK(basis.col_status[2] == BasisStatus::at_lower);
    CHECK(basis.col_status[3] == BasisStatus::at_lower);
    CHECK(basis.row_status[1] == BasisStatus::basic);
    CHECK(basis.row_status[2] == BasisStatus::basic);
  }

  SUBCASE("shrink truncates surplus statuses")
  {
    Basis basis;
    basis.col_status = {
        BasisStatus::basic, BasisStatus::at_upper, BasisStatus::free};
    basis.row_status = {BasisStatus::basic, BasisStatus::at_lower};

    reconcile_basis(basis, 1, 0);

    CHECK(basis.num_cols() == 1);
    CHECK(basis.num_rows() == 0);
    CHECK(basis.col_status[0] == BasisStatus::basic);
  }

  SUBCASE("empty / clear")
  {
    Basis basis;
    CHECK(basis.empty());
    basis.col_status = {BasisStatus::basic};
    CHECK_FALSE(basis.empty());
    basis.clear();
    CHECK(basis.empty());
    CHECK(basis.num_cols() == 0);
    CHECK(basis.num_rows() == 0);
  }
}

TEST_CASE("Basis - backend get/set round-trip")  // NOLINT
{
  // Solve cold; capture the optimal basis.
  auto lp = make_tiny_lp();
  const SolverOptions cold;
  const auto cold_result = lp.initial_solve(cold);
  REQUIRE(cold_result.has_value());
  REQUIRE(cold_result.value() == 0);
  const double cold_obj = -lp.get_obj_value_raw();
  REQUIRE(cold_obj == doctest::Approx(18.0));

  const auto captured = lp.get_basis();
  if (!captured.has_value()) {
    // cuOpt (PDLP, no basis) / SCIP, or a barrier-without-crossover solve —
    // basis not exposed; the primitive degrades to a benign skip.  Nothing
    // more to assert on this backend.
    WARN_MESSAGE(captured.has_value(),
                 "active backend exposes no basis; round-trip skipped");
    return;
  }

  CHECK(captured->num_cols() == static_cast<std::size_t>(lp.get_numcols()));
  CHECK(captured->num_rows() == static_cast<std::size_t>(lp.get_numrows()));

  SUBCASE("seed an identical LP via dual simplex")
  {
    auto warm_lp = make_tiny_lp();
    SolverOptions warm;
    warm.advanced_basis = true;
    warm.algorithm = LPAlgo::dual;

    REQUIRE(warm_lp.set_basis(*captured));
    const auto warm_result = warm_lp.initial_solve(warm);
    REQUIRE(warm_result.has_value());
    CHECK(warm_result.value() == 0);
    CHECK(-warm_lp.get_obj_value_raw() == doctest::Approx(18.0));
  }

  SUBCASE("seed an LP with an appended cut row (reconcile path)")
  {
    // Mirror the SDDP case: the re-solve LP has one extra (cut) row that the
    // captured basis predates.  reconcile_basis must pad it basic.
    auto cut_lp = make_tiny_lp();
    // A non-binding cut: x1 + x2 <= 100 (leaves the optimum at 18).
    SparseRow cut;
    cut[ColIndex {0}] = 1.0;
    cut[ColIndex {1}] = 1.0;
    cut.uppb = 100.0;
    (void)cut_lp.add_row(cut);
    REQUIRE(cut_lp.get_numrows() == 2);

    SolverOptions warm;
    warm.advanced_basis = true;
    warm.algorithm = LPAlgo::dual;

    // captured has 1 row; set_basis reconciles to the current 2 rows.
    REQUIRE(cut_lp.set_basis(*captured));
    const auto warm_result = cut_lp.initial_solve(warm);
    REQUIRE(warm_result.has_value());
    CHECK(warm_result.value() == 0);
    CHECK(-cut_lp.get_obj_value_raw() == doctest::Approx(18.0));
  }
}

}  // namespace basis_test
