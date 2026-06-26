/**
 * @file      lp_equilibration.cpp
 * @brief     Implementation of the per-row equilibration primitives.
 * @date      2026-04-20
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtopt/lp_equilibration.hpp>

namespace gtopt
{

// ── Fast sqrt implementations ───────────────────────────────────────
// Ruiz scaling is iterative and self-correcting, so an approximate sqrt
// only affects convergence speed, not final accuracy.
namespace
{
/// IEEE 754 exponent halving (~2-3% accuracy, ~1 cycle).
[[nodiscard]] inline auto sqrt_ieee_halve(double x) noexcept -> double
{
  auto bits = std::bit_cast<std::uint64_t>(x);
  bits = (bits >> 1U) + 0x1FF8'0000'0000'0000ULL;
  return std::bit_cast<double>(bits);
}

/// IEEE halve + one Newton-Raphson step (~0.1% accuracy).
[[nodiscard]] inline auto sqrt_newton1(double x) noexcept -> double
{
  const double y = sqrt_ieee_halve(x);
  return 0.5 * (y + (x / y));
}

/// Dispatch to the selected sqrt method.
[[nodiscard]] inline auto fast_sqrt(double x, FastSqrtMethod method) noexcept
    -> double
{
  switch (method) {
    case FastSqrtMethod::ieee_halve:
      return sqrt_ieee_halve(x);
    case FastSqrtMethod::newton1:
      return sqrt_newton1(x);
    case FastSqrtMethod::std_sqrt:
      return std::sqrt(x);
  }
  return sqrt_ieee_halve(x);
}
}  // namespace

double equilibrate_row_in_place(std::span<double> elements,
                                double& rowlb,
                                double& rowub,
                                double infinity) noexcept
{
  // 1. Find max |coeff| over the row's non-zeros.
  double max_abs = 0.0;
  for (const double v : elements) {
    max_abs = std::max(max_abs, std::abs(v));
  }

  // Empty, all-zero, or already-normalised: skip the pass.  The
  // idempotency check (==1.0) is deliberately strict so that a caller
  // passing in an already-normalised row from a previous equilibrate
  // call gets the expected 1.0 back (no compounding scale).
  if (max_abs == 0.0 || max_abs == 1.0) {
    return 1.0;
  }

  // 2. Scale coefficients in place.  Multiplying by the reciprocal
  //    matches `apply_row_max_equilibration` and avoids N divides.
  const double inv = 1.0 / max_abs;
  for (double& v : elements) {
    v *= inv;
  }

  // 3. Scale finite bounds, preserve ±infinity sentinels.
  if (rowlb > -infinity) {
    rowlb *= inv;
  }
  if (rowub < infinity) {
    rowub *= inv;
  }

  return max_abs;
}

double equilibrate_row_ruiz_in_place(std::span<const int> columns,
                                     std::span<double> elements,
                                     std::span<const double> col_scales,
                                     double& rowlb,
                                     double& rowub,
                                     double infinity) noexcept
{
  // Fold frozen column scales into the new row's coefficients.  Any
  // column index outside the saved `col_scales` span (rare — would
  // indicate a column appended after load_flat) is treated as scale
  // 1.0, matching the behaviour of `get_col_scale`.
  const auto n_col_scales = col_scales.size();
  const auto n = std::min(columns.size(), elements.size());
  for (std::size_t k = 0; k < n; ++k) {
    const auto j = static_cast<std::size_t>(columns[k]);
    if (j < n_col_scales) {
      elements[k] *= col_scales[j];
    }
  }

  // Row-max normalise on top of the column-scaled coefficients so the
  // combined new-row entries match the equilibrated matrix profile.
  return equilibrate_row_in_place(elements, rowlb, rowub, infinity);
}

// ── Row-max equilibration (single pass) ─────────────────────────────
// Scale each row so that its largest |coefficient| becomes 1.0.
// Returns the per-row scale factors (max |coeff| per row, or 1.0 for
// empty rows).

std::vector<double> apply_row_max_equilibration(
    std::span<const std::int32_t> matind,
    std::span<double> matval,
    std::span<double> rowlb,
    std::span<double> rowub,
    double infinity)
{
  // Bulk row-max equilibration used at build time.  The per-row scaling
  // math is shared conceptually with the single-row
  // `equilibrate_row_in_place` primitive above; this variant just stays
  // CSC-oriented because it already holds the whole matrix in hand.
  const auto nrows = rowlb.size();

  // 1. Compute row max |coefficient| from the CSC matrix.
  std::vector<double> row_scales(nrows, 0.0);
  for (size_t k = 0; k < matval.size(); ++k) {
    const auto r = static_cast<size_t>(matind[k]);
    row_scales[r] = std::max(row_scales[r], std::abs(matval[k]));
  }

  // 2. Convert to reciprocals (1/scale); use 1.0 for empty rows.
  //    Multiplying by reciprocal is ~4x faster than dividing.
  for (auto& s : row_scales) {
    s = (s > 0.0) ? (1.0 / s) : 1.0;
  }

  // 3. Apply scaling to matrix coefficients using reciprocal multiply.
  for (size_t k = 0; k < matval.size(); ++k) {
    const auto r = static_cast<size_t>(matind[k]);
    matval[k] *= row_scales[r];
  }

  // 4. Scale row bounds (RHS), preserving infinite bounds.
  for (size_t r = 0; r < nrows; ++r) {
    const double rs = row_scales[r];
    if (rs != 1.0) {
      if (rowlb[r] > -infinity) {
        rowlb[r] *= rs;
      }
      if (rowub[r] < infinity) {
        rowub[r] *= rs;
      }
    }
  }

  // 5. Invert back to scale factors for the caller (who expects max-abs).
  for (auto& s : row_scales) {
    if (s != 1.0) {
      s = 1.0 / s;
    }
  }

  return row_scales;
}

// ── Ruiz geometric-mean iterative scaling ───────────────────────────
// Alternately normalizes rows and columns by sqrt(infinity-norm) until
// convergence.  Updates row_scales, col_scales, row/col bounds and
// objective coefficients.
//
// Reference: Ruiz, D. (2001) "A scaling algorithm to equilibrate both
// rows and columns norms in matrices".

std::vector<double> apply_ruiz_scaling(std::span<const std::int32_t> matbeg,
                                       std::span<const std::int32_t> matind,
                                       std::span<double> matval,
                                       std::span<double> rowlb,
                                       std::span<double> rowub,
                                       std::span<double> collb,
                                       std::span<double> colub,
                                       std::span<double> objval,
                                       std::span<double> col_scales,
                                       std::span<const std::int32_t> colpin,
                                       double infinity,
                                       FastSqrtMethod sqrt_method,
                                       int max_iterations,
                                       double tolerance)
{
  const auto nrows = rowlb.size();
  const auto ncols = collb.size();

  // ``colpin`` carries column indices exempt from rescaling — the
  // union of (a) integer-declared columns (where a non-unit scale
  // would turn the physical bound 1 into a non-integer LP upper bound
  // and break backend integer enforcement) and (b) ``pin_scale``-
  // tagged semantically-binary continuous columns (LP-relaxed
  // commitment status / startup / shutdown — see SparseCol::pin_scale
  // and task #50).  Build a dense bitmap so the inner loops branch
  // O(1) without a linear-scan of ``colpin`` per iteration.
  std::vector<bool> is_pinned_col(ncols, false);
  for (const auto idx : colpin) {
    const auto j = static_cast<size_t>(idx);
    if (j < ncols) {
      is_pinned_col[j] = true;
    }
  }

  // Cumulative row scales (product of per-iteration sqrt factors).
  std::vector<double> cum_row_scales(nrows, 1.0);

  std::vector<double> row_inf_norm(nrows);
  std::vector<double> col_inf_norm(ncols);
  std::vector<double> row_factor(nrows);
  std::vector<double> col_factor(ncols);

  for (int iter = 0; iter < max_iterations; ++iter) {
    // 1. Compute row and column infinity-norms in a single CSC pass.
    std::ranges::fill(row_inf_norm, 0.0);
    for (size_t j = 0; j < ncols; ++j) {
      const auto beg = static_cast<size_t>(matbeg[j]);
      const auto end = static_cast<size_t>(matbeg[j + 1]);
      double cmax = 0.0;
      for (size_t k = beg; k < end; ++k) {
        const double av = std::abs(matval[k]);
        const auto r = static_cast<size_t>(matind[k]);
        row_inf_norm[r] = std::max(row_inf_norm[r], av);
        cmax = std::max(cmax, av);
      }
      col_inf_norm[j] = cmax;
    }

    // 2. Compute row reciprocal factors and track convergence.
    //    Store 1/sqrt(norm) so we multiply instead of divide (~4x faster).
    double max_deviation = 0.0;
    for (size_t r = 0; r < nrows; ++r) {
      const double n = row_inf_norm[r];
      if (n > 0.0) {
        max_deviation = std::max(max_deviation, std::abs(n - 1.0));
        row_factor[r] = 1.0 / fast_sqrt(n, sqrt_method);
      } else {
        row_factor[r] = 1.0;
      }
    }

    // 3. Compute column reciprocal factors and track convergence.
    //    Pinned columns are kept at col_factor = 1.0 — see the
    //    ``is_pinned_col`` rationale at the top of this function.
    for (size_t j = 0; j < ncols; ++j) {
      if (is_pinned_col[j]) [[unlikely]] {
        col_factor[j] = 1.0;
        continue;
      }
      const double n = col_inf_norm[j];
      if (n > 0.0) {
        max_deviation = std::max(max_deviation, std::abs(n - 1.0));
        col_factor[j] = 1.0 / fast_sqrt(n, sqrt_method);
      } else {
        col_factor[j] = 1.0;
      }
    }
    if (max_deviation < tolerance) {
      break;
    }

    // 4. Apply row + column scaling to matrix coefficients.
    //    row_factor/col_factor are reciprocals, so multiply.
    for (size_t j = 0; j < ncols; ++j) {
      const auto beg = static_cast<size_t>(matbeg[j]);
      const auto end = static_cast<size_t>(matbeg[j + 1]);
      const double cf = col_factor[j];
      for (size_t k = beg; k < end; ++k) {
        const auto r = static_cast<size_t>(matind[k]);
        matval[k] *= row_factor[r] * cf;
      }
    }

    // 5. Scale row bounds, preserving infinite bounds.
    for (size_t r = 0; r < nrows; ++r) {
      const double rf = row_factor[r];
      if (rf != 1.0) {
        if (rowlb[r] > -infinity) {
          rowlb[r] *= rf;
        }
        if (rowub[r] < infinity) {
          rowub[r] *= rf;
        }
      }
    }

    // 6. Scale column bounds and objective.
    //    Column scaling substitutes y_j = x_j / col_factor[j]:
    //    col_factor is 1/sqrt(norm), so:
    //      collb /= col_factor (= *= sqrt(norm))
    //      colub /= col_factor (= *= sqrt(norm))
    //      objval *= col_factor
    //      col_scales *= col_factor
    for (size_t j = 0; j < ncols; ++j) {
      const double cf = col_factor[j];
      if (cf != 1.0) {
        const double cf_inv = 1.0 / cf;
        if (collb[j] > -infinity) {
          collb[j] *= cf_inv;
        }
        if (colub[j] < infinity) {
          colub[j] *= cf_inv;
        }
        objval[j] *= cf;
        col_scales[j] *= cf;
      }
    }

    // 7. Accumulate row scales (reciprocal: divide instead of multiply).
    for (size_t r = 0; r < nrows; ++r) {
      cum_row_scales[r] /= row_factor[r];
    }
  }

  return cum_row_scales;
}

}  // namespace gtopt
