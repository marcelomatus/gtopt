/**
 * @file      lp_equilibration.cpp
 * @brief     Implementation of the per-row equilibration primitives.
 * @date      2026-04-20
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <gtopt/lp_equilibration.hpp>

namespace gtopt
{

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

}  // namespace gtopt
