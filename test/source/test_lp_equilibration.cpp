/**
 * @file      test_lp_equilibration.cpp
 * @brief     Unit tests for the per-row equilibration primitives that
 *            will feed the post-build cut insertion path.
 * @date      2026-04-20
 * @copyright BSD-3-Clause
 */

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/lp_equilibration.hpp>
#include <gtopt/lp_matrix_enums.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
constexpr double kInfinity = std::numeric_limits<double>::infinity();
}  // namespace

// ─── equilibrate_row_in_place ────────────────────────────────────────────

TEST_CASE("equilibrate_row_in_place empty row returns 1.0")  // NOLINT
{
  std::vector<double> elements;
  double lb = -kInfinity;
  double ub = 5.0;

  const auto divisor = equilibrate_row_in_place(elements, lb, ub, kInfinity);

  CHECK(divisor == doctest::Approx(1.0));
  CHECK(ub == doctest::Approx(5.0));
  CHECK(lb == -kInfinity);
}

TEST_CASE("equilibrate_row_in_place all-zero row returns 1.0")  // NOLINT
{
  std::vector<double> elements {0.0, 0.0, 0.0};
  double lb = 2.0;
  double ub = 5.0;

  const auto divisor = equilibrate_row_in_place(elements, lb, ub, kInfinity);

  CHECK(divisor == doctest::Approx(1.0));
  for (const double v : elements) {
    CHECK(v == doctest::Approx(0.0));
  }
  CHECK(lb == doctest::Approx(2.0));
  CHECK(ub == doctest::Approx(5.0));
}

TEST_CASE("equilibrate_row_in_place normalizes to max|coeff| == 1")  // NOLINT
{
  // max|coeff| is 200.0 on the third entry → divisor 200.
  std::vector<double> elements {10.0, -50.0, 200.0, 0.5};
  double lb = 400.0;
  double ub = 600.0;

  const auto divisor = equilibrate_row_in_place(elements, lb, ub, kInfinity);

  CHECK(divisor == doctest::Approx(200.0));
  CHECK(elements[0] == doctest::Approx(10.0 / 200.0));
  CHECK(elements[1] == doctest::Approx(-50.0 / 200.0));
  CHECK(elements[2] == doctest::Approx(1.0));  // normalized
  CHECK(elements[3] == doctest::Approx(0.5 / 200.0));
  CHECK(lb == doctest::Approx(2.0));  // 400 / 200
  CHECK(ub == doctest::Approx(3.0));  // 600 / 200
}

TEST_CASE("equilibrate_row_in_place preserves ±infinity bounds")  // NOLINT
{
  std::vector<double> elements {100.0, -50.0};
  double lb = -kInfinity;
  double ub = kInfinity;

  const auto divisor = equilibrate_row_in_place(elements, lb, ub, kInfinity);

  CHECK(divisor == doctest::Approx(100.0));
  CHECK(elements[0] == doctest::Approx(1.0));
  CHECK(elements[1] == doctest::Approx(-0.5));
  CHECK(lb == -kInfinity);  // not touched
  CHECK(ub == kInfinity);  // not touched
}

TEST_CASE("equilibrate_row_in_place idempotent on normalized row")  // NOLINT
{
  // Row already has max|coeff| == 1 → short-circuit with divisor 1.
  // This protects against compounding scale on repeated calls.
  std::vector<double> elements {1.0, -0.5, 0.25};
  double lb = 0.3;
  double ub = 0.8;

  const auto divisor = equilibrate_row_in_place(elements, lb, ub, kInfinity);

  CHECK(divisor == doctest::Approx(1.0));
  CHECK(elements[0] == doctest::Approx(1.0));
  CHECK(elements[1] == doctest::Approx(-0.5));
  CHECK(elements[2] == doctest::Approx(0.25));
  CHECK(lb == doctest::Approx(0.3));
  CHECK(ub == doctest::Approx(0.8));
}

TEST_CASE("equilibrate_row_in_place honors custom infinity sentinel")  // NOLINT
{
  // Bounds at the solver's sentinel (1e30) should be preserved, not
  // scaled.  This mirrors how the bulk equilibration path in
  // linear_problem.cpp uses `m_infinity_`.
  constexpr double kInf = 1e30;
  std::vector<double> elements {100.0};
  double lb = -kInf;
  double ub = kInf;

  const auto divisor = equilibrate_row_in_place(elements, lb, ub, kInf);

  CHECK(divisor == doctest::Approx(100.0));
  CHECK(elements[0] == doctest::Approx(1.0));
  CHECK(lb == -kInf);
  CHECK(ub == kInf);
}

// ─── equilibrate_row_ruiz_in_place ──────────────────────────────────────

TEST_CASE(
    "equilibrate_row_ruiz_in_place with unit col_scales reduces to row_max")
{
  // col_scales all 1.0 → Ruiz variant must behave identically to
  // equilibrate_row_in_place.
  const std::vector<int> columns {0, 1, 2};
  std::vector<double> elements {10.0, -50.0, 200.0};
  const std::vector<double> col_scales(3, 1.0);
  double lb = 400.0;
  double ub = 600.0;

  const auto divisor = equilibrate_row_ruiz_in_place(
      columns, elements, col_scales, lb, ub, kInfinity);

  CHECK(divisor == doctest::Approx(200.0));
  CHECK(elements[0] == doctest::Approx(10.0 / 200.0));
  CHECK(elements[1] == doctest::Approx(-50.0 / 200.0));
  CHECK(elements[2] == doctest::Approx(1.0));
  CHECK(lb == doctest::Approx(2.0));
  CHECK(ub == doctest::Approx(3.0));
}

TEST_CASE("equilibrate_row_ruiz_in_place composes col_scales")  // NOLINT
{
  // col_scales {2, 10, 100} are applied first; after the multiply the
  // absolute coefficients are {10×2, -50×10, 200×100} = {20, -500, 20000},
  // so max|coeff_LP| = 20000 and the divisor is 20000.
  const std::vector<int> columns {0, 1, 2};
  std::vector<double> elements {10.0, -50.0, 200.0};
  const std::vector<double> col_scales {2.0, 10.0, 100.0};
  double lb = 1000.0;
  double ub = 4000.0;

  const auto divisor = equilibrate_row_ruiz_in_place(
      columns, elements, col_scales, lb, ub, kInfinity);

  CHECK(divisor == doctest::Approx(20000.0));
  CHECK(elements[0] == doctest::Approx(20.0 / 20000.0));
  CHECK(elements[1] == doctest::Approx(-500.0 / 20000.0));
  CHECK(elements[2] == doctest::Approx(1.0));  // 200×100 / 20000
  CHECK(lb == doctest::Approx(0.05));  // 1000 / 20000
  CHECK(ub == doctest::Approx(0.20));  // 4000 / 20000
}

TEST_CASE("equilibrate_row_ruiz_in_place treats out-of-range cols as scale 1.0")
{
  // Column index 5 is outside `col_scales`: falls back to 1.0 (same
  // contract as LinearInterface::get_col_scale).
  const std::vector<int> columns {0, 5};
  std::vector<double> elements {4.0, 10.0};
  const std::vector<double> col_scales {3.0};  // only covers col 0
  double lb = 0.0;
  double ub = 1.0;

  const auto divisor = equilibrate_row_ruiz_in_place(
      columns, elements, col_scales, lb, ub, kInfinity);

  // Effective coefs: {4×3, 10×1} = {12, 10}, divisor = 12.
  CHECK(divisor == doctest::Approx(12.0));
  CHECK(elements[0] == doctest::Approx(1.0));  // 12 / 12
  CHECK(elements[1] == doctest::Approx(10.0 / 12.0));
}

TEST_CASE(
    "equilibrate_row_ruiz_in_place empty col_scales falls back to row_max")
{
  // Empty col_scales means no column scaling was applied at build time
  // (no Ruiz, no semantic col_scale).  Every column gets the default
  // scale 1.0, so the function reduces to per-row row_max.
  const std::vector<int> columns {0, 1, 2};
  std::vector<double> elements {10.0, -50.0, 200.0};
  const std::vector<double> col_scales {};
  double lb = -kInfinity;
  double ub = 600.0;

  const auto divisor = equilibrate_row_ruiz_in_place(
      columns, elements, col_scales, lb, ub, kInfinity);

  CHECK(divisor == doctest::Approx(200.0));
  CHECK(elements[2] == doctest::Approx(1.0));
  CHECK(lb == -kInfinity);
  CHECK(ub == doctest::Approx(3.0));
}

TEST_CASE("equilibrate_row_ruiz_in_place preserves dual invariance via divisor")
{
  // Regression guard: the composite `row × divisor` on the physical
  // side must recover the pre-equilibrated state.  Simulates the
  // dual_physical = dual_LP × row_scale invariant.
  const std::vector<int> columns {0, 1};
  std::vector<double> elements {300.0, -150.0};
  const std::vector<double> col_scales {2.0, 5.0};
  double lb = 600.0;
  double ub = 1200.0;

  // Snapshot the physical-space state before equilibration.
  const std::vector<double> expected_after_col_scale {
      300.0 * 2.0,
      -150.0 * 5.0,
  };  // {600, -750}
  const double expected_divisor = 750.0;

  const auto divisor = equilibrate_row_ruiz_in_place(
      columns, elements, col_scales, lb, ub, kInfinity);

  CHECK(divisor == doctest::Approx(expected_divisor));
  // Scaling back should recover the col-scaled (not raw) coefficients,
  // since the divisor is what the caller stores in m_row_scales_ for
  // dual recovery.
  CHECK(elements[0] * divisor == doctest::Approx(expected_after_col_scale[0]));
  CHECK(elements[1] * divisor == doctest::Approx(expected_after_col_scale[1]));
  CHECK(lb * divisor == doctest::Approx(600.0));
  CHECK(ub * divisor == doctest::Approx(1200.0));
}

// ─── LinearInterface::add_row ──────────────────────────────

TEST_CASE("add_row — no col_scales, no equilibration → plain")
{
  // LP built without equilibration and without semantic col_scales:
  // the method should pass through to `add_row` unchanged.
  LinearInterface li;
  const auto c0 = li.add_col(SparseCol {
      .uppb = 10.0,
  });
  const auto c1 = li.add_col(SparseCol {
      .uppb = 10.0,
  });

  SparseRow row;
  row[c0] = 3.0;
  row[c1] = -7.0;
  row.lowb = -kInfinity;
  row.uppb = 20.0;

  const auto idx = li.add_row(row);

  // Coefficients land in the LP exactly as given.
  CHECK(li.get_coeff_raw(idx, c0) == doctest::Approx(3.0));
  CHECK(li.get_coeff_raw(idx, c1) == doctest::Approx(-7.0));
  CHECK(li.get_row_scale(idx) == doctest::Approx(1.0));
}

TEST_CASE("add_row — row_max method normalises max|coeff| to 1")
{
  // Simulate a row_max-equilibrated LP by marking the method *after*
  // adding columns, before any rows — mirrors how `load_flat` wires
  // `m_equilibration_method_` from `FlatLinearProblem` at build time.
  LinearInterface li;
  const auto c0 = li.add_col(SparseCol {
      .uppb = 1e6,
  });
  const auto c1 = li.add_col(SparseCol {
      .uppb = 1e6,
  });
  li.save_base_numrows();

  // Inject the method into m_equilibration_method_ via the setter
  // wire (add_row_equilibration happens inside add_row).
  li.set_equilibration_method(LpEquilibrationMethod::row_max);

  SparseRow row;
  row[c0] = 10.0;
  row[c1] = -250.0;  // dominant
  row.lowb = 500.0;
  row.uppb = kInfinity;

  const auto idx = li.add_row(row);

  // max|coeff| was 250 → divisor = 250; dominant coeff now ±1.
  CHECK(li.get_coeff_raw(idx, c0) == doctest::Approx(10.0 / 250.0));
  CHECK(li.get_coeff_raw(idx, c1) == doctest::Approx(-1.0));
  CHECK(li.get_row_low_raw()[idx] == doctest::Approx(500.0 / 250.0));
  // row_scale stored = divisor, so dual_phys = dual_LP × row_scale.
  CHECK(li.get_row_scale(idx) == doctest::Approx(250.0));
}

TEST_CASE("add_row — col_scales compose before row_max")
{
  LinearInterface li;
  const auto c0 = li.add_col(SparseCol {
      .uppb = 1e6,
  });
  const auto c1 = li.add_col(SparseCol {
      .uppb = 1e6,
  });
  li.save_base_numrows();
  li.set_col_scale(c0, 2.0);
  li.set_col_scale(c1, 10.0);
  li.set_equilibration_method(LpEquilibrationMethod::row_max);

  SparseRow row;
  row[c0] = 4.0;  // LP = 4 × 2 = 8
  row[c1] = -3.0;  // LP = -3 × 10 = -30  (dominant)
  row.lowb = 100.0;
  row.uppb = kInfinity;

  const auto idx = li.add_row(row);

  // After col-scaling: {8, -30}.  max|coeff| = 30 → divisor 30.
  CHECK(li.get_coeff_raw(idx, c0) == doctest::Approx(8.0 / 30.0));
  CHECK(li.get_coeff_raw(idx, c1) == doctest::Approx(-1.0));
  CHECK(li.get_row_low_raw()[idx] == doctest::Approx(100.0 / 30.0));
  CHECK(li.get_row_scale(idx) == doctest::Approx(30.0));
}

// ─── apply_row_max_equilibration (bulk, whole-matrix) ────────────────────

TEST_CASE("apply_row_max_equilibration normalizes each row's max to 1")
{
  // CSC: 2 cols × 2 rows.
  //   col0 = {row0:10, row1:100}, col1 = {row0:5, row1:2}
  // Row maxima: row0 = max(10,5) = 10; row1 = max(100,2) = 100.
  const std::vector<std::int32_t> matind {0, 1, 0, 1};
  std::vector<double> matval {10.0, 100.0, 5.0, 2.0};
  std::vector<double> rowlb {-kInfinity, 50.0};
  std::vector<double> rowub {20.0, 300.0};

  const auto scales =
      apply_row_max_equilibration(matind, matval, rowlb, rowub, kInfinity);

  REQUIRE(scales.size() == 2);
  CHECK(scales[0] == doctest::Approx(10.0));  // returned = max-abs per row
  CHECK(scales[1] == doctest::Approx(100.0));
  CHECK(matval[0] == doctest::Approx(1.0));  // 10/10
  CHECK(matval[1] == doctest::Approx(1.0));  // 100/100
  CHECK(matval[2] == doctest::Approx(0.5));  // 5/10
  CHECK(matval[3] == doctest::Approx(0.02));  // 2/100
  CHECK(rowlb[0] == -kInfinity);  // ±infinity preserved
  CHECK(rowlb[1] == doctest::Approx(0.5));  // 50/100
  CHECK(rowub[0] == doctest::Approx(2.0));  // 20/10
  CHECK(rowub[1] == doctest::Approx(3.0));  // 300/100
}

// ─── apply_ruiz_scaling (bulk, whole-matrix iterative) ───────────────────

TEST_CASE("apply_ruiz_scaling equilibrates a diagonal imbalance in one pass")
{
  // Diagonal CSC: col0 = {row0:1e6}, col1 = {row1:4}.  Each row and each
  // column has a single entry, so one Ruiz pass drives every coefficient
  // to exactly 1 (row_factor·col_factor = 1/√n · 1/√n = 1/n).
  const std::vector<std::int32_t> matbeg {0, 1, 2};
  const std::vector<std::int32_t> matind {0, 1};
  std::vector<double> matval {1.0e6, 4.0};
  std::vector<double> rowlb {-kInfinity, -kInfinity};
  std::vector<double> rowub {kInfinity, kInfinity};
  std::vector<double> collb {-kInfinity, -kInfinity};
  std::vector<double> colub {kInfinity, kInfinity};
  std::vector<double> objval {1.0, 1.0};
  std::vector<double> col_scales {1.0, 1.0};
  const std::vector<std::int32_t> colpin {};

  const auto row_scales = apply_ruiz_scaling(matbeg,
                                             matind,
                                             matval,
                                             rowlb,
                                             rowub,
                                             collb,
                                             colub,
                                             objval,
                                             col_scales,
                                             colpin,
                                             kInfinity,
                                             FastSqrtMethod::std_sqrt,
                                             10,
                                             1e-3);

  CHECK(matval[0] == doctest::Approx(1.0));
  CHECK(matval[1] == doctest::Approx(1.0));
  // cum_row_scales = √(row infinity-norm) = {√1e6, √4} = {1000, 2}.
  REQUIRE(row_scales.size() == 2);
  CHECK(row_scales[0] == doctest::Approx(1000.0));
  CHECK(row_scales[1] == doctest::Approx(2.0));
  // objval and col_scales fold col_factor = 1/√norm = {1e-3, 0.5}.
  CHECK(objval[0] == doctest::Approx(1.0e-3));
  CHECK(objval[1] == doctest::Approx(0.5));
  CHECK(col_scales[0] == doctest::Approx(1.0e-3));
  CHECK(col_scales[1] == doctest::Approx(0.5));
}

TEST_CASE("apply_ruiz_scaling max_iterations/tolerance knobs gate the work")
{
  // Coupled CSC needing many passes: col0 = {row0:100, row1:100},
  // col1 = {row0:1, row1:1}.  After one pass col1's entries sit at 0.1
  // and creep toward 1 only over further iterations — so the iteration
  // cap is observable in the result.
  const std::vector<std::int32_t> matbeg {0, 2, 4};
  const std::vector<std::int32_t> matind {0, 1, 0, 1};
  const std::vector<double> orig {100.0, 100.0, 1.0, 1.0};

  auto run = [&](int max_it, double tol, std::vector<double> mv)
  {
    std::vector<double> rowlb(2, -kInfinity);
    std::vector<double> rowub(2, kInfinity);
    std::vector<double> collb(2, -kInfinity);
    std::vector<double> colub(2, kInfinity);
    std::vector<double> objval(2, 1.0);
    std::vector<double> col_scales(2, 1.0);
    const std::vector<std::int32_t> colpin {};
    auto scales = apply_ruiz_scaling(matbeg,
                                     matind,
                                     mv,
                                     rowlb,
                                     rowub,
                                     collb,
                                     colub,
                                     objval,
                                     col_scales,
                                     colpin,
                                     kInfinity,
                                     FastSqrtMethod::std_sqrt,
                                     max_it,
                                     tol);
    return std::pair {mv, scales};
  };

  SUBCASE("max_iterations = 0 is a no-op")
  {
    auto [mv, scales] = run(0, 1e-3, orig);
    CHECK(mv == orig);  // matrix untouched
    CHECK(scales[0] == doctest::Approx(1.0));  // identity row scales
    CHECK(scales[1] == doctest::Approx(1.0));
  }

  SUBCASE("one pass leaves col1 entries at 0.1")
  {
    auto [mv, scales] = run(1, 1e-3, orig);
    CHECK(mv[2] == doctest::Approx(0.1));  // 1 · (1/√100) · (1/√1)
    CHECK(mv[3] == doctest::Approx(0.1));
  }

  SUBCASE("more iterations equilibrate col1 closer to 1")
  {
    auto [mv1, s1] = run(1, 1e-3, orig);
    auto [mv8, s8] = run(8, 1e-3, orig);
    // Eight passes drive col1 nearer to 1 than a single pass.
    CHECK(std::abs(mv8[2] - 1.0) < std::abs(mv1[2] - 1.0));
  }

  SUBCASE("huge tolerance short-circuits before any scaling")
  {
    // First norm pass sees deviation 99 < 1e30 → break before applying.
    auto [mv, scales] = run(10, 1e30, orig);
    CHECK(mv == orig);
    CHECK(scales[0] == doctest::Approx(1.0));
    CHECK(scales[1] == doctest::Approx(1.0));
  }
}

TEST_CASE("apply_ruiz_scaling leaves colpin columns unscaled")
{
  // Same coupled matrix, but pin col1: its entries must stay at 0.1
  // (column factor forced to 1.0) however many passes run, and its
  // col_scale must remain 1.0.
  const std::vector<std::int32_t> matbeg {0, 2, 4};
  const std::vector<std::int32_t> matind {0, 1, 0, 1};
  std::vector<double> matval {100.0, 100.0, 1.0, 1.0};
  std::vector<double> rowlb(2, -kInfinity);
  std::vector<double> rowub(2, kInfinity);
  std::vector<double> collb(2, -kInfinity);
  std::vector<double> colub(2, kInfinity);
  std::vector<double> objval(2, 1.0);
  std::vector<double> col_scales {1.0, 1.0};
  const std::vector<std::int32_t> colpin {1};  // pin column 1

  const auto row_scales = apply_ruiz_scaling(matbeg,
                                             matind,
                                             matval,
                                             rowlb,
                                             rowub,
                                             collb,
                                             colub,
                                             objval,
                                             col_scales,
                                             colpin,
                                             kInfinity,
                                             FastSqrtMethod::std_sqrt,
                                             10,
                                             1e-3);
  CHECK(row_scales.size() == 2);

  CHECK(matval[2] == doctest::Approx(0.1));  // pinned col never rescaled
  CHECK(matval[3] == doctest::Approx(0.1));
  CHECK(col_scales[1] == doctest::Approx(1.0));  // pin keeps scale at 1
  CHECK(objval[1] == doctest::Approx(1.0));  // and objective untouched
}
