// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_solver_obj_offset_per_solver.cpp
 * @brief     Per-solver tests for the native objective-offset channel
 *            (SolverBackend::set_obj_offset via
 * LinearInterface::add_obj_constant).
 * @date      2026-06-30
 * @author    claude
 * @copyright BSD-3-Clause
 *
 * `LinearInterface::add_obj_constant(c)` accumulates the constant in
 * `m_obj_constant_raw_` and pushes the running total to the backend's NATIVE
 * objective offset:
 *   - CPLEX   : CPXchgobjoffset
 *   - HiGHS   : HighsLp::offset_ / changeObjectiveOffset
 *   - Gurobi  : GRB_DBL_ATTR_OBJCON
 *   - MindOpt : MDO_DBL_ATTR_OBJ_CON
 *   - cuOpt   : objective_offset (cuOptCreateRangedProblem)
 *   - SCIP    : SCIPaddOrigObjoffset
 *   - OSI/CLP : ClpModel::setObjectiveOffset (negated — CLP subtracts)
 * `get_obj_value_raw()` no longer re-adds the constant gtopt-side, so a
 * reported objective that includes `c` proves the constant reached the solver.
 *
 * Closed-form fixture (LP-relaxation optimum == MIP optimum):
 *   min  10 x + 1 y      x >= 0 (continuous),  y in [0, 3]
 *   s.t.  x + y >= 5      ⇒  optimum y = 3, x = 2, obj = 23.
 * The argmin is offset-invariant, so a constant only shifts the reported value.
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_enums.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace test_solver_obj_offset  // NOLINT(cert-dcl59-cpp,google-build-namespaces)
{
/// Build the closed-form fixture into `lp`; return the two column indices.
/// When `integer_y` is true, `y` is declared integer (MIP path).
struct Fixture
{
  ColIndex x;
  ColIndex y;
};

[[nodiscard]] Fixture build_fixture(LinearInterface& lp, bool integer_y)
{
  const auto x = lp.add_col(SparseCol {
      .lowb = 0.0,
      .cost = 10.0,
  });
  const auto y = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 3.0,
      .cost = 1.0,
  });
  if (integer_y) {
    lp.set_integer(y);
  }
  SparseRow row;
  row[x] = 1.0;
  row[y] = 1.0;
  row.greater_equal(5.0);
  [[maybe_unused]] const auto row_idx = lp.add_row(row);
  return Fixture {.x = x, .y = y};
}
}  // namespace test_solver_obj_offset

TEST_CASE(  // NOLINT
    "add_obj_constant - native offset shifts LP objective by exactly c "
    "(per solver)")
{
  using namespace test_solver_obj_offset;  // NOLINT(google-build-namespaces)
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  const double c = 137.5;
  const SolverOptions opts;
  bool tested_any = false;

  for (const auto& name : reg.available_solvers()) {
    tested_any = true;
    CAPTURE(name);  // logs the failing plugin under ctest

    // ── Baseline: no offset ──────────────────────────────────────────
    double baseline_obj = 0.0;
    double baseline_x = 0.0;
    double baseline_y = 0.0;
    {
      LinearInterface lp(name);
      const auto f = build_fixture(lp, /*integer_y=*/false);
      REQUIRE(lp.initial_solve(opts).has_value());
      REQUIRE(lp.is_optimal());
      baseline_obj = lp.get_obj_value();
      const auto sol = lp.get_col_sol();
      baseline_x = sol[f.x];
      baseline_y = sol[f.y];
      CHECK(baseline_obj == doctest::Approx(23.0).epsilon(1e-6));
    }

    // ── With offset applied via add_obj_constant on the LIVE backend ──
    LinearInterface lp(name);
    const auto f = build_fixture(lp, /*integer_y=*/false);
    REQUIRE(lp.initial_solve(opts).has_value());
    REQUIRE(lp.is_optimal());

    lp.add_obj_constant(c);
    REQUIRE(lp.resolve(opts).has_value());
    REQUIRE(lp.is_optimal());

    // (1) Reported objective shifts by exactly c — since the gtopt-side
    //     re-add is gone, this proves the offset reached the solver.
    CHECK(lp.get_obj_value()
          == doctest::Approx(baseline_obj + c).epsilon(1e-6));

    // (2) The argmin is unchanged (a constant does not move the optimum).
    {
      const auto sol = lp.get_col_sol();
      CHECK(sol[f.x] == doctest::Approx(baseline_x).epsilon(1e-6));
      CHECK(sol[f.y] == doctest::Approx(baseline_y).epsilon(1e-6));
    }

    // (3) clone() preserves the offset: the cloned backend re-solves to the
    //     same offset-inclusive objective without any further add_obj_constant.
    {
      auto cloned = lp.clone();
      REQUIRE(cloned.resolve(opts).has_value());
      REQUIRE(cloned.is_optimal());
      CHECK(cloned.get_obj_value()
            == doctest::Approx(baseline_obj + c).epsilon(1e-6));
    }
  }

  if (!tested_any) {
    MESSAGE("no solver plugin loaded — skipping per-solver obj-offset test");
  }
}

TEST_CASE(  // NOLINT
    "add_obj_constant - native offset reaches the MIP reported objective "
    "(per MIP solver)")
{
  using namespace test_solver_obj_offset;  // NOLINT(google-build-namespaces)
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  const double c = 500.0;
  const SolverOptions opts;
  bool tested_any = false;

  for (const auto& name : reg.available_solvers()) {
    if (!reg.supports_mip(name)) {
      continue;  // LP-only backend — covered by the LP test above
    }
    tested_any = true;
    CAPTURE(name);

    LinearInterface lp(name);
    const auto f = build_fixture(lp, /*integer_y=*/true);
    REQUIRE(lp.initial_solve(opts).has_value());
    REQUIRE(lp.is_optimal());
    REQUIRE(lp.has_integer_cols());
    const double mip_obj = lp.get_obj_value();
    CHECK(mip_obj == doctest::Approx(23.0).epsilon(1e-6));

    // Apply the native offset and re-solve the MIP.  The offset must move the
    // solver's reported optimal objective by exactly c — the property CPLEX's
    // MIP relative-gap is computed against — while the integer optimum holds.
    lp.add_obj_constant(c);
    REQUIRE(lp.resolve(opts).has_value());
    REQUIRE(lp.is_optimal());
    CHECK(lp.get_obj_value() == doctest::Approx(mip_obj + c).epsilon(1e-6));
    {
      const auto sol = lp.get_col_sol();
      CHECK(sol[f.y] == doctest::Approx(3.0).epsilon(1e-6));
      CHECK(sol[f.x] == doctest::Approx(2.0).epsilon(1e-6));
    }
  }

  if (!tested_any) {
    MESSAGE(
        "no MIP-capable solver plugin loaded — skipping MIP obj-offset "
        "test");
  }
}

TEST_CASE(  // NOLINT
    "add_obj_constant - successive calls accumulate; clone carries the sum")
{
  using namespace test_solver_obj_offset;  // NOLINT(google-build-namespaces)
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  const auto solvers = reg.available_solvers();
  if (solvers.empty()) {
    MESSAGE("no solver plugin loaded — skipping obj-offset accumulation test");
    return;
  }

  const auto name = solvers.front();
  CAPTURE(name);
  const SolverOptions opts;

  LinearInterface lp(name);
  const auto f = build_fixture(lp, /*integer_y=*/false);
  REQUIRE(lp.initial_solve(opts).has_value());
  REQUIRE(lp.is_optimal());
  const double base = lp.get_obj_value();
  CHECK(base == doctest::Approx(23.0).epsilon(1e-6));

  // Two successive constants accumulate: the objective shifts by their SUM,
  // proving each add_obj_constant pushes the running total (not a delta that
  // overwrites the previous).
  const double c1 = 40.0;
  const double c2 = -15.0;  // include a negative to exercise sign handling
  lp.add_obj_constant(c1);
  lp.add_obj_constant(c2);
  REQUIRE(lp.resolve(opts).has_value());
  REQUIRE(lp.is_optimal());
  CHECK(lp.get_obj_value() == doctest::Approx(base + c1 + c2).epsilon(1e-6));

  // The argmin is still offset-invariant.
  {
    const auto sol = lp.get_col_sol();
    CHECK(sol[f.x] == doctest::Approx(2.0).epsilon(1e-6));
    CHECK(sol[f.y] == doctest::Approx(3.0).epsilon(1e-6));
  }

  // clone() carries the accumulated sum.
  auto cloned = lp.clone();
  REQUIRE(cloned.resolve(opts).has_value());
  REQUIRE(cloned.is_optimal());
  CHECK(cloned.get_obj_value()
        == doctest::Approx(base + c1 + c2).epsilon(1e-6));
}
