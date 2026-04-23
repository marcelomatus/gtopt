/**
 * @file      test_flatten_col_scale.cpp
 * @brief     Tests that flatten() correctly applies col_scale to matrix
 *            coefficients and objective, and preserves infinity bounds.
 * @date      2026-04-05
 * @copyright BSD-3-Clause
 *
 * Verifies the core invariant introduced by the col_scale encapsulation:
 *   LP_coeff  = phys_coeff × col_scale / row_scale
 *   LP_cost   = phys_cost  × col_scale / scale_objective
 *   LP_bound  = phys_bound / col_scale  (infinity preserved)
 */

#include <limits>
#include <numbers>

#include <doctest/doctest.h>
#include <gtopt/linear_problem.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// Tests check raw LP coefficients without row equilibration.
inline LpMatrixOptions no_eq_opts()
{
  LpMatrixOptions o;
  o.equilibration_method = LpEquilibrationMethod::none;
  return o;
}

// ── Helper: find matval entry for a given (row, col) in CSC flat LP ─────────

/// Return the matrix coefficient for (row_idx, col_idx) in the flat LP,
/// or 0.0 if the entry does not exist.
[[nodiscard]] double find_matval(const FlatLinearProblem& flat,
                                 int row_idx,
                                 int col_idx)
{
  const auto beg = flat.matbeg[col_idx];
  const auto end = (col_idx + 1 < flat.ncols)
      ? flat.matbeg[col_idx + 1]
      : static_cast<int>(flat.matind.size());
  for (auto k = beg; k < end; ++k) {
    if (flat.matind[k] == row_idx) {
      return flat.matval[k];
    }
  }
  return 0.0;
}

// ═══════════════════════════════════════════════════════════════════════════
// 1. Matrix coefficient scaling: phys_coeff × col_scale
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(
    "flatten col_scale — matrix coefficients multiplied by col_scale")  // NOLINT
{
  LinearProblem lp("col_scale_coeff_test");

  // Column with scale=1000 (e.g. reservoir energy)
  const auto c_scaled = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 2000.0,
      .scale = 1000.0,
  });

  // Column with scale=1.0 (e.g. generator output)
  const auto c_plain = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 300.0,
  });

  // Row: 1.0 × vol + 5.0 × gen = 100 (physical coefficients)
  auto row = SparseRow {};
  row[c_scaled] = 1.0;  // physical: 1 unit of energy
  row[c_plain] = 5.0;  // physical: 5 units of generation
  row.equal(100.0);
  const auto r = lp.add_row(std::move(row));

  const auto flat = lp.flatten(no_eq_opts());

  // LP coefficient for vol: phys_coeff × col_scale = 1.0 × 1000 = 1000
  CHECK(find_matval(flat, static_cast<int>(r), static_cast<int>(c_scaled))
        == doctest::Approx(1000.0));

  // LP coefficient for gen: phys_coeff × col_scale = 5.0 × 1.0 = 5.0
  CHECK(find_matval(flat, static_cast<int>(r), static_cast<int>(c_plain))
        == doctest::Approx(5.0));

  // Row bounds unchanged (physical)
  CHECK(flat.rowlb[static_cast<int>(r)] == doctest::Approx(100.0));
  CHECK(flat.rowub[static_cast<int>(r)] == doctest::Approx(100.0));
}

TEST_CASE(
    "flatten col_scale — multiple columns with different scales")  // NOLINT
{
  LinearProblem lp("multi_scale_test");

  // Simulate mixed-scale problem: theta (0.0001), energy (100000), flow (1.0)
  const auto c_theta = lp.add_col(SparseCol {
      .lowb = -std::numbers::pi,
      .uppb = +std::numbers::pi,
      .scale = 0.0001,
  });
  const auto c_energy = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 5000.0,
      .scale = 100000.0,
  });
  const auto c_flow = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 500.0,
  });

  // Kirchhoff-like row: 1.0 × theta + 0.0 × energy + 50.0 × flow = 0
  auto kirchhoff = SparseRow {};
  kirchhoff[c_theta] = 1.0;
  kirchhoff[c_flow] = 50.0;
  kirchhoff.equal(0.0);
  const auto r_k = lp.add_row(std::move(kirchhoff));

  // Energy balance: 1.0 × energy - 3.6 × flow = 0
  auto balance = SparseRow {};
  balance[c_energy] = 1.0;
  balance[c_flow] = -3.6;
  balance.equal(0.0);
  const auto r_e = lp.add_row(std::move(balance));

  const auto flat = lp.flatten(no_eq_opts());

  // Kirchhoff: theta coeff = 1.0 × 0.0001, flow coeff = 50.0 × 1.0
  CHECK(find_matval(flat, static_cast<int>(r_k), static_cast<int>(c_theta))
        == doctest::Approx(0.0001));
  CHECK(find_matval(flat, static_cast<int>(r_k), static_cast<int>(c_flow))
        == doctest::Approx(50.0));

  // Energy balance: energy coeff = 1.0 × 100000, flow coeff = -3.6 × 1.0
  CHECK(find_matval(flat, static_cast<int>(r_e), static_cast<int>(c_energy))
        == doctest::Approx(100000.0));
  CHECK(find_matval(flat, static_cast<int>(r_e), static_cast<int>(c_flow))
        == doctest::Approx(-3.6));
}

TEST_CASE("flatten col_scale — combined with row_scale")  // NOLINT
{
  LinearProblem lp("col_row_scale_test");

  const auto c = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .scale = 500.0,
  });

  // Row with row_scale = 2.0: LP_coeff = phys_coeff × col_scale / row_scale
  auto row = SparseRow {};
  row[c] = 4.0;
  row.scale = 2.0;
  row.equal(80.0);
  const auto r = lp.add_row(std::move(row));

  const auto flat = lp.flatten(no_eq_opts());

  // LP coeff = 4.0 × 500.0 / 2.0 = 1000.0
  CHECK(find_matval(flat, static_cast<int>(r), static_cast<int>(c))
        == doctest::Approx(1000.0));

  // Row bounds scaled by row_scale: 80.0 / 2.0 = 40.0
  CHECK(flat.rowlb[static_cast<int>(r)] == doctest::Approx(40.0));
  CHECK(flat.rowub[static_cast<int>(r)] == doctest::Approx(40.0));
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. Objective scaling: phys_cost × col_scale / scale_objective
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("flatten col_scale — objective multiplied by col_scale")  // NOLINT
{
  LinearProblem lp("col_scale_obj_test");

  // Column with physical cost=10.0 and scale=500.0
  const auto c = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .cost = 10.0,
      .scale = 500.0,
  });

  // Dummy row to keep the LP non-trivial
  auto row = SparseRow {};
  row[c] = 1.0;
  row.less_equal(50.0);
  [[maybe_unused]] const auto r = lp.add_row(std::move(row));

  const auto flat = lp.flatten(no_eq_opts());

  // LP obj = 10.0 × 500.0 = 5000.0 (no scale_objective by default)
  CHECK(flat.objval[static_cast<int>(c)] == doctest::Approx(5000.0));
}

TEST_CASE("flatten col_scale — objective with scale_objective")  // NOLINT
{
  LinearProblem lp("col_scale_obj_sobj_test");

  const auto c = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .cost = 10.0,
      .scale = 500.0,
  });

  auto row = SparseRow {};
  row[c] = 1.0;
  row.less_equal(50.0);
  [[maybe_unused]] const auto r = lp.add_row(std::move(row));

  auto opts = no_eq_opts();
  opts.scale_objective = 1000.0;
  const auto flat = lp.flatten(opts);

  // LP obj = 10.0 × 500.0 / 1000.0 = 5.0
  CHECK(flat.objval[static_cast<int>(c)] == doctest::Approx(5.0));
}

TEST_CASE("flatten col_scale=1 — objective unchanged")  // NOLINT
{
  LinearProblem lp("col_scale_1_obj_test");

  const auto c = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 300.0,
      .cost = 20.0,
  });

  auto row = SparseRow {};
  row[c] = 1.0;
  row.less_equal(300.0);
  [[maybe_unused]] const auto _ = lp.add_row(std::move(row));

  auto opts = no_eq_opts();
  opts.scale_objective = 1000.0;
  const auto flat = lp.flatten(opts);

  // LP obj = 20.0 × 1.0 / 1000.0 = 0.02
  CHECK(flat.objval[static_cast<int>(c)] == doctest::Approx(0.02));
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. Column bounds: phys_bound / col_scale (infinity preserved)
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("flatten col_scale — bounds divided by col_scale")  // NOLINT
{
  LinearProblem lp("col_scale_bounds_test");

  const auto c = lp.add_col(SparseCol {
      .lowb = 100.0,
      .uppb = 5000.0,
      .scale = 1000.0,
  });

  auto row = SparseRow {};
  row[c] = 1.0;
  row.less_equal(5000.0);
  [[maybe_unused]] const auto _ = lp.add_row(std::move(row));

  const auto flat = lp.flatten(no_eq_opts());

  // LP bounds = physical / scale
  CHECK(flat.collb[static_cast<int>(c)] == doctest::Approx(0.1));  // 100/1000
  CHECK(flat.colub[static_cast<int>(c)] == doctest::Approx(5.0));  // 5000/1000
}

TEST_CASE("flatten col_scale — DblMax bounds preserved as infinity")  // NOLINT
{
  LinearProblem lp("col_scale_inf_test");

  // With default m_infinity_ = DblMax, DblMax stays as-is
  const auto c = lp.add_col(SparseCol {
      .lowb = -DblMax,
      .uppb = DblMax,
      .scale = 1000.0,
  });

  auto row = SparseRow {};
  row[c] = 1.0;
  row.less_equal(100.0);
  [[maybe_unused]] const auto _ = lp.add_row(std::move(row));

  const auto flat = lp.flatten(no_eq_opts());

  // Infinity bounds must NOT be divided by col_scale
  CHECK(flat.collb[static_cast<int>(c)] <= -1e300);
  CHECK(flat.colub[static_cast<int>(c)] >= 1e300);
}

TEST_CASE(
    "flatten col_scale — solver infinity preserved after set_infinity")  // NOLINT
{
  LinearProblem lp("col_scale_solver_inf_test");

  // Simulate solver infinity (e.g. HiGHS uses 1e30)
  constexpr double solver_inf = 1e30;
  lp.set_infinity(solver_inf);

  // DblMax gets normalized to solver_inf in add_col
  const auto c = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = DblMax,
      .scale = 100000.0,
  });

  auto row = SparseRow {};
  row[c] = 1.0;
  row.less_equal(100.0);
  [[maybe_unused]] const auto _ = lp.add_row(std::move(row));

  const auto flat = lp.flatten(no_eq_opts());

  // Lower bound = 0.0 / 100000 = 0.0
  CHECK(flat.collb[static_cast<int>(c)] == doctest::Approx(0.0));

  // Upper bound was DblMax → normalized to solver_inf → NOT divided by scale
  CHECK(flat.colub[static_cast<int>(c)] == doctest::Approx(solver_inf));
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. Physical invariance: the LP represents the same physical problem
//    regardless of col_scale
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("flatten col_scale — physical constraint invariant")  // NOLINT
{
  // The same physical constraint "2x + 3y = 10" should produce
  // different LP coefficients but the same physical solution
  // when x and y have different col_scales.

  for (const auto& [sx, sy] :
       {
           std::pair {1.0, 1.0},
           std::pair {100.0, 1.0},
           std::pair {1.0, 50.0},
           std::pair {100.0, 50.0},
       })
  {
    LinearProblem lp("invariance_test");

    const auto cx = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 10.0,
        .cost = 1.0,
        .scale = sx,
    });
    const auto cy = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 10.0,
        .cost = 2.0,
        .scale = sy,
    });

    auto row = SparseRow {};
    row[cx] = 2.0;
    row[cy] = 3.0;
    row.equal(10.0);
    [[maybe_unused]] const auto _ = lp.add_row(std::move(row));

    const auto flat = lp.flatten(no_eq_opts());

    // LP coefficients: phys × scale
    CHECK(find_matval(flat, 0, static_cast<int>(cx))
          == doctest::Approx(2.0 * sx));
    CHECK(find_matval(flat, 0, static_cast<int>(cy))
          == doctest::Approx(3.0 * sy));

    // LP bounds: phys / scale
    CHECK(flat.colub[static_cast<int>(cx)] == doctest::Approx(10.0 / sx));
    CHECK(flat.colub[static_cast<int>(cy)] == doctest::Approx(10.0 / sy));

    // LP obj: phys_cost × scale
    CHECK(flat.objval[static_cast<int>(cx)] == doctest::Approx(1.0 * sx));
    CHECK(flat.objval[static_cast<int>(cy)] == doctest::Approx(2.0 * sy));

    // Physical constraint: substituting LP_val = phys_val / scale
    // (2.0×sx) × (x_phys/sx) + (3.0×sy) × (y_phys/sy) = 10.0
    // → 2.0 × x_phys + 3.0 × y_phys = 10.0  ✓
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// 5. DblMax / infinity consistency
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("LinearProblem DblMax equals numeric_limits max")  // NOLINT
{
  CHECK(LinearProblem::DblMax == std::numeric_limits<double>::max());
  CHECK(DblMax == std::numeric_limits<double>::max());
}

TEST_CASE("LinearProblem default infinity equals DblMax")  // NOLINT
{
  const LinearProblem lp("inf_test");
  CHECK(lp.infinity() == DblMax);
}

TEST_CASE("LinearProblem set_infinity normalizes DblMax bounds")  // NOLINT
{
  LinearProblem lp("inf_norm_test");

  constexpr double solver_inf = 1e20;
  lp.set_infinity(solver_inf);
  CHECK(lp.infinity() == doctest::Approx(solver_inf));

  // add_col with DblMax bounds → normalized to solver_inf
  const auto c = lp.add_col(SparseCol {
      .lowb = -DblMax,
      .uppb = DblMax,
  });

  const auto& col = lp.col_at(c);
  CHECK(col.lowb == doctest::Approx(-solver_inf));
  CHECK(col.uppb == doctest::Approx(+solver_inf));
}

TEST_CASE("LinearProblem set_infinity normalizes row DblMax bounds")  // NOLINT
{
  LinearProblem lp("inf_row_norm_test");

  constexpr double solver_inf = 1e20;
  lp.set_infinity(solver_inf);

  const auto c = lp.add_col(SparseCol {});

  // Row with only upper bound (>= constraint uses -DblMax for lowb)
  auto row = SparseRow {};
  row[c] = 1.0;
  row.less_equal(100.0);  // lowb = -DblMax, uppb = 100
  const auto r = lp.add_row(std::move(row));

  const auto& stored = lp.row_at(r);
  CHECK(stored.lowb == doctest::Approx(-solver_inf));
  CHECK(stored.uppb == doctest::Approx(100.0));
}

TEST_CASE(
    "flatten col_scale — finite bounds not affected by set_infinity")  // NOLINT
{
  LinearProblem lp("inf_finite_test");

  constexpr double solver_inf = 1e20;
  lp.set_infinity(solver_inf);

  const auto c = lp.add_col(SparseCol {
      .lowb = -50.0,
      .uppb = 200.0,
      .scale = 10.0,
  });

  auto row = SparseRow {};
  row[c] = 1.0;
  row.less_equal(200.0);
  [[maybe_unused]] const auto _ = lp.add_row(std::move(row));

  const auto flat = lp.flatten(no_eq_opts());

  // Finite bounds divided by scale as normal
  CHECK(flat.collb[static_cast<int>(c)] == doctest::Approx(-5.0));  // -50/10
  CHECK(flat.colub[static_cast<int>(c)] == doctest::Approx(20.0));  // 200/10
}

// ═══════════════════════════════════════════════════════════════════════════
// 6. col_scales vector in FlatLinearProblem
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("flatten col_scales vector matches SparseCol.scale")  // NOLINT
{
  LinearProblem lp("col_scales_vec_test");

  [[maybe_unused]] const auto ca = lp.add_col(SparseCol {
      .scale = 0.001,
  });
  [[maybe_unused]] const auto cb = lp.add_col(SparseCol {});  // default 1.0
  [[maybe_unused]] const auto cc = lp.add_col(SparseCol {
      .scale = 100000.0,
  });

  // Need at least one row for flatten
  auto row = SparseRow {};
  row[ColIndex {0}] = 1.0;
  row.less_equal(1.0);
  [[maybe_unused]] const auto _ = lp.add_row(std::move(row));

  const auto flat = lp.flatten(no_eq_opts());

  REQUIRE(flat.col_scales.size() == 3);
  CHECK(flat.col_scales[0] == doctest::Approx(0.001));
  CHECK(flat.col_scales[1] == doctest::Approx(1.0));
  CHECK(flat.col_scales[2] == doctest::Approx(100000.0));
}

}  // namespace
