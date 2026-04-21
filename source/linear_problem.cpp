/**
 * @file      linear_problem.cpp
 * @brief     Implementation of the linear programming problem representation
 * @date      Fri Mar 28 22:18:17 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the core data structures and operations for building
 * and manipulating linear programming problems. It handles the sparse matrix
 * representation, problem flattening, and matrix operations needed for
 * efficient solver integration.
 */

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <format>
#include <ranges>
#include <stdexcept>
#include <unordered_map>

#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_equilibration.hpp>
#include <gtopt/map_reserve.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

// ── Fast sqrt implementations ───────────────────────────────────────
// Ruiz scaling is iterative and self-correcting, so approximate sqrt
// only affects convergence speed, not final accuracy.

/// IEEE 754 exponent halving (~2-3% accuracy, ~1 cycle).
[[nodiscard]] inline auto sqrt_ieee_halve(double x) noexcept -> double
{
  auto bits = std::bit_cast<uint64_t>(x);
  bits = (bits >> 1U) + 0x1FF8'0000'0000'0000ULL;
  return std::bit_cast<double>(bits);
}

/// IEEE halve + one Newton-Raphson step (~0.1% accuracy).
[[nodiscard]] inline auto sqrt_newton1(double x) noexcept -> double
{
  const double y = sqrt_ieee_halve(x);
  return 0.5 * (y + (x / y));
}

/// Standard library sqrt (exact).
[[nodiscard]] inline auto sqrt_std(double x) noexcept -> double
{
  return std::sqrt(x);
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
      return sqrt_std(x);
  }
  return sqrt_ieee_halve(x);
}

// ── Row-max equilibration (single pass) ─────────────────────────────
// Scale each row so that its largest |coefficient| becomes 1.0.
// Returns the per-row scale factors (max |coeff| per row, or 1.0 for
// empty rows).

[[nodiscard]] auto apply_row_max_equilibration(
    std::span<const FlatLinearProblem::index_t> matind,
    std::span<double> matval,
    std::span<double> rowlb,
    std::span<double> rowub,
    double infinity) -> std::vector<double>
{
  // Bulk row-max equilibration used at build time.  The per-row scaling
  // math is shared with the single-row `equilibrate_row_in_place`
  // primitive in `lp_equilibration.cpp`; this variant just stays
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
// Alternately normalizes rows and columns by sqrt(infinity-norm)
// until convergence.  Updates row_scales, col_scales, row/col bounds,
// and objective coefficients.
//
// Reference: Ruiz, D. (2001) "A scaling algorithm to equilibrate both
// rows and columns norms in matrices".

struct RuizScalingResult
{
  std::vector<double> row_scales;
  // col_scales are updated in-place (passed by reference)
};

[[nodiscard]] auto apply_ruiz_scaling(
    std::span<const FlatLinearProblem::index_t> matbeg,
    std::span<const FlatLinearProblem::index_t> matind,
    std::span<double> matval,
    std::span<double> rowlb,
    std::span<double> rowub,
    std::span<double> collb,
    std::span<double> colub,
    std::span<double> objval,
    std::span<double> col_scales,
    double infinity,
    FastSqrtMethod sqrt_method = FastSqrtMethod::ieee_halve,
    int max_iterations = 10,
    double tolerance = 1e-3) -> std::vector<double>
{
  const auto nrows = rowlb.size();
  const auto ncols = collb.size();

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
    for (size_t j = 0; j < ncols; ++j) {
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

}  // namespace

auto LinearProblem::flatten(const LpMatrixOptions& opts) -> FlatLinearProblem
{
  const size_t ncols = get_numcols();
  const size_t nrows = get_numrows();
  if (ncols == 0 || nrows == 0) [[unlikely]] {
    return {};
  }

  using fp_index_t = FlatLinearProblem::index_t;
  std::vector<fp_index_t> matbeg(ncols + 1, 0);
  std::vector<fp_index_t> matind;
  std::vector<double> matval;

  // Two-pass approach avoids creating an intermediate SparseMatrix
  // of ncols flat_maps, reducing memory allocations and sort overhead.

  // Pass 1: count non-zeros per column to build matbeg
  const auto eps = opts.eps;
  for (const auto& row : rows) {
    for (const auto& [j, v] : row.cmap) {
      assert(j >= 0 && "column index in row.cmap must be non-negative");
      if (eps < 0 || std::abs(v) > eps) [[likely]] {
        ++matbeg[static_cast<size_t>(j)];
      }
    }
  }

  // Convert counts to start offsets (exclusive prefix sum)
  {
    fp_index_t cumsum = 0;
    for (size_t c = 0; c < ncols; ++c) {
      const auto count = matbeg[c];
      matbeg[c] = cumsum;
      cumsum += count;
    }
    matbeg[ncols] = cumsum;

    const auto nnzero = static_cast<size_t>(cumsum);
    matind.resize(nnzero);
    matval.resize(nnzero);
  }

  // Pass 2: fill matind and matval using column offsets, and extract row
  // bounds in the same traversal to avoid iterating rows a third time.
  // Reuse matbeg as write cursors (avoids allocating a separate colpos
  // vector); we reconstruct matbeg from the final cursor positions after
  // the loop.
  auto& colpos = matbeg;

  // Optionally track coefficient stats during the matrix scan (zero cost
  // when disabled — the branch is predictable and the inner loop is hot).
  const bool do_stats = opts.compute_stats.value_or(false);
  double stats_max = 0.0;
  double stats_min = std::numeric_limits<double>::max();
  size_t stats_nnz = 0;
  size_t stats_zeroed = 0;
  fp_index_t stats_max_col = -1;
  fp_index_t stats_min_col = -1;

  // Effective minimum for stats min/max tracking:
  //   max(eps, stats_eps) — applied after the matrix eps filter.
  // The new requirement states that min/max stats are computed after applying
  // the eps tolerance for the A-matrix coefficients, so both the matrix eps
  // (outer if-condition) and the stats_eps floor are respected here.
  const double eff_stats_eps =
      (eps >= 0) ? std::max(eps, opts.stats_eps) : opts.stats_eps;

  // Pre-pass: collect col_scales before Pass 2 so that matrix coefficients
  // can be scaled by col_scale during the row traversal.
  // physical_value = LP_value × col_scale, so LP_coeff = phys_coeff ×
  // col_scale.
  std::vector<double> col_scales(ncols, 1.0);
  for (const auto& [i, col] : enumerate(cols)) {
    col_scales[i] = col.scale;
  }

  std::vector<double> rowlb(nrows);
  std::vector<double> rowub(nrows);

  for (const auto& [i, row] : enumerate(rows)) {
    const auto rs = row.scale;
    const auto inv_rs = (rs != 1.0) ? 1.0 / rs : 1.0;
    rowlb[i] = (rs != 1.0 && row.lowb > -m_infinity_ && row.lowb < m_infinity_)
        ? row.lowb * inv_rs
        : row.lowb;
    rowub[i] = (rs != 1.0 && row.uppb > -m_infinity_ && row.uppb < m_infinity_)
        ? row.uppb * inv_rs
        : row.uppb;

    for (const auto& [j, v_raw] : row.cmap) {
      assert(j >= 0 && "column index in row.cmap must be non-negative");
      const auto cs = col_scales[static_cast<size_t>(j)];
      const auto v = v_raw * cs * inv_rs;
      if (eps < 0 || std::abs(v) > eps) [[likely]] {
        const auto c = static_cast<size_t>(j);
        const auto pos = static_cast<size_t>(colpos[c]);
        matind[pos] = static_cast<fp_index_t>(i);
        matval[pos] = v;
        ++colpos[c];

        if (do_stats) [[unlikely]] {
          const double abs_v = std::abs(v);
          ++stats_nnz;
          // MIN/MAX are computed after applying the effective eps tolerance.
          // Values in [0, eff_stats_eps] are in the matrix but not counted
          // for min, ensuring consistent results with external LP analysis
          // tools that use LP-file precision (typically ~1e-10 floor).
          if (abs_v > stats_max) {
            stats_max = abs_v;
            stats_max_col = static_cast<fp_index_t>(j);
          }
          if (abs_v >= eff_stats_eps && abs_v < stats_min) {
            stats_min = abs_v;
            stats_min_col = static_cast<fp_index_t>(j);
          }
        }
      } else if (do_stats && eps >= 0 && v != 0.0) [[unlikely]] {
        // Non-zero entry filtered out (set to zero) by the eps tolerance.
        ++stats_zeroed;
      }
    }
  }

  // Reconstruct matbeg from the advanced write cursors: after pass 2,
  // colpos[c] == original matbeg[c+1].  Shift right and reset position 0.
  for (size_t c = ncols; c > 0; --c) {
    matbeg[c] = matbeg[c - 1];
  }
  matbeg[0] = 0;

  std::vector<double> collb(ncols);
  std::vector<double> colub(ncols);
  std::vector<double> objval(ncols);
  std::vector<fp_index_t> colint;
  colint.reserve(colints);

  for (const auto& [i, col] : enumerate(cols)) {
    // SparseCol bounds are physical; convert to LP units by dividing
    // by the column scale factor.  Infinite bounds are preserved as-is
    // (IEEE 754 guarantees inf / finite = inf, but we skip the division
    // for clarity and to avoid -inf / negative-scale sign issues).
    const auto s = col.scale;
    const auto inf = m_infinity_;
    collb[i] = (s != 1.0 && col.lowb > -inf && col.lowb < inf) ? col.lowb / s
                                                               : col.lowb;
    colub[i] = (s != 1.0 && col.uppb > -inf && col.uppb < inf) ? col.uppb / s
                                                               : col.uppb;
    // Objective cost: physical cost × col_scale.  Since LP variable =
    // physical / col_scale, this ensures obj = cost_phys × x_phys.
    objval[i] = col.cost * s;

    if (col.is_integer) [[unlikely]] {
      colint.push_back(static_cast<fp_index_t>(i));
    }
  }

  // Name vectors — delegated to LabelMaker which honors LpNamesLevel and
  // handles the `is_state` gating for minimal-level labels.  The same
  // LabelMaker is copied into FlatLinearProblem so LinearInterface can
  // continue generating labels for rows/cols added after load_flat().
  //
  // Fallback: if the caller did not explicitly install a LabelMaker via
  // set_label_maker() (leaving it at LpNamesLevel::none), derive an
  // effective one from the naming bools in LpMatrixOptions.
  //
  // Gating: flatten() populates colnm / rownm whenever the caller asks
  // for them via `col_with_names` / `row_with_names`.  Per-entry label
  // content is decided by LabelMaker, so an entry may be an empty
  // string when the level disables it — callers never need to gate.
  const LabelMaker effective_lm = [&]
  {
    if (m_label_maker_.names_level() != LpNamesLevel::none) {
      return m_label_maker_;
    }
    const auto lvl = (opts.row_with_name_map || opts.col_with_names)
        ? LpNamesLevel::all
        : LpNamesLevel::none;
    return LabelMaker {lvl};
  }();

  using fp_name_vec_t = FlatLinearProblem::name_vec_t;

  fp_name_vec_t colnm;
  if (opts.col_with_names || opts.col_with_name_map) [[unlikely]] {
    colnm.reserve(cols.size());
    for (const auto& col : cols) {
      colnm.emplace_back(effective_lm.make_col_label(col));
    }
  }

  fp_name_vec_t rownm;
  if (opts.row_with_names || opts.row_with_name_map) [[unlikely]] {
    rownm.reserve(rows.size());
    for (const auto& row : rows) {
      rownm.emplace_back(effective_lm.make_row_label(row));
    }
  }

  // Index name maps
  using fp_index_map_t = FlatLinearProblem::index_map_t;
  auto build_name_map = [](const auto& names,
                           std::string_view entity_type) -> fp_index_map_t
  {
    fp_index_map_t map;
    map.reserve(names.size());

    for (const auto& [i, name] : enumerate(names)) {
      if (name.empty()) [[unlikely]] {
        continue;  // skip empty names to avoid false-positive duplicates
      }
      if (auto [it, inserted] = map.try_emplace(name, i); !inserted)
          [[unlikely]]
      {
        throw std::runtime_error(
            std::format("linear problem using repeated {} name '{}' "
                        "(first at index {}, duplicate at index {})",
                        entity_type,
                        name,
                        it->second,
                        i));
      }
    }
    return map;
  };

  fp_index_map_t colmp;
  if (opts.col_with_name_map) [[unlikely]] {
    colmp = build_name_map(colnm, "column");
  }

  fp_index_map_t rowmp;
  if (opts.row_with_name_map) [[unlikely]] {
    rowmp = build_name_map(rownm, "row");
  }

  // Populate col name strings for stats from colnm (which may have been
  // moved from cols, so we must read from colnm, not cols).
  std::string stats_max_col_name;
  std::string stats_min_col_name;
  if (do_stats && !colnm.empty()) {
    if (stats_max_col >= 0) {
      stats_max_col_name = colnm[static_cast<size_t>(stats_max_col)];
    }
    if (stats_min_col >= 0) {
      stats_min_col_name = colnm[static_cast<size_t>(stats_min_col)];
    }
  }

  // ── Matrix equilibration scaling ────────────────────────────────────
  // Dispatch to the selected equilibration method.  The row_scales
  // vector stores the cumulative divisor per row:
  //   dual_physical = dual_LP / row_scale[i]
  // Column scales are updated in-place inside col_scales.

  const auto eq_method =
      opts.equilibration_method.value_or(LpEquilibrationMethod::row_max);

  std::vector<double> row_scales_vec;
  if (eq_method == LpEquilibrationMethod::row_max) {
    row_scales_vec =
        apply_row_max_equilibration(matind, matval, rowlb, rowub, m_infinity_);
  } else if (eq_method == LpEquilibrationMethod::ruiz) {
    const auto sqrt_method =
        opts.fast_sqrt_method.value_or(FastSqrtMethod::ieee_halve);
    row_scales_vec = apply_ruiz_scaling(matbeg,
                                        matind,
                                        matval,
                                        rowlb,
                                        rowub,
                                        collb,
                                        colub,
                                        objval,
                                        col_scales,
                                        m_infinity_,
                                        sqrt_method);
  }

  // Compose SparseRow scales with equilibration scales.
  // Total row_scale = sparserow_scale × equilibration_scale, so that
  //   dual_physical = dual_LP / total_row_scale.
  // When equilibration is disabled (row_scales_vec empty) but some rows
  // have non-unit SparseRow::scale, build the vector from those scales.
  {
    bool has_row_scale = false;
    for (const auto& row : rows) {
      if (row.scale != 1.0) {
        has_row_scale = true;
        break;
      }
    }
    if (has_row_scale) {
      if (row_scales_vec.empty()) {
        row_scales_vec.resize(nrows, 1.0);
      }
      for (const auto& [i, row] : enumerate(rows)) {
        if (row.scale != 1.0) {
          row_scales_vec[i] *= row.scale;
        }
      }
    }
  }

  if (!row_scales_vec.empty() && do_stats) {
    // Recompute coefficient stats after equilibration so the reported
    // ratio reflects the actual matrix sent to the solver.
    stats_max = 0.0;
    stats_min = std::numeric_limits<double>::max();
    stats_max_col = -1;
    stats_min_col = -1;
    for (size_t c = 0; c < ncols; ++c) {
      const auto beg = static_cast<size_t>(matbeg[c]);
      const auto end = static_cast<size_t>(matbeg[c + 1]);
      for (size_t k = beg; k < end; ++k) {
        const double abs_v = std::abs(matval[k]);
        if (abs_v > stats_max) {
          stats_max = abs_v;
          stats_max_col = static_cast<fp_index_t>(c);
        }
        if (abs_v >= eff_stats_eps && abs_v < stats_min) {
          stats_min = abs_v;
          stats_min_col = static_cast<fp_index_t>(c);
        }
      }
    }
    // Update column name references for the new min/max columns.
    stats_max_col_name = (stats_max_col >= 0
                          && static_cast<size_t>(stats_max_col) < colnm.size())
        ? colnm[static_cast<size_t>(stats_max_col)]
        : "";
    stats_min_col_name = (stats_min_col >= 0
                          && static_cast<size_t>(stats_min_col) < colnm.size())
        ? colnm[static_cast<size_t>(stats_min_col)]
        : "";
  }

  // ── Per-row-type coefficient statistics ──────────────────────────────
  // Classify rows by constraint type (extracted from row names) and
  // compute per-type min/max |coefficient|.  Computed after row
  // equilibration so stats reflect the actual matrix sent to the solver.
  std::vector<FlatLinearProblem::RowTypeStatsEntry> row_type_stats_vec;
  if (do_stats && !rownm.empty()) {
    std::vector<double> row_max(nrows, 0.0);
    std::vector<double> row_min(nrows, std::numeric_limits<double>::max());
    std::vector<size_t> row_nnz(nrows, 0);

    for (size_t k = 0; k < matval.size(); ++k) {
      const auto r = static_cast<size_t>(matind[k]);
      const double av = std::abs(matval[k]);
      if (av > 0.0) {
        ++row_nnz[r];
        row_max[r] = std::max(row_max[r], av);
        row_min[r] = std::min(row_min[r], av);
      }
    }

    auto extract_row_type = [](std::string_view name) -> std::string_view
    {
      if (name.empty()) {
        return "unknown";
      }
      const auto first_sep = name.find('_');
      if (first_sep == std::string_view::npos) {
        return name;
      }
      const auto rest = name.substr(first_sep + 1);
      const auto second_sep = rest.find('_');
      return rest.substr(0, second_sep);
    };

    struct TypeAccum
    {
      size_t count {};
      size_t nnz {};
      double max_abs {};
      double min_abs {std::numeric_limits<double>::max()};
    };
    std::unordered_map<std::string_view, TypeAccum> type_map;
    map_reserve(type_map, 32);

    for (size_t r = 0; r < nrows; ++r) {
      const auto type = extract_row_type(rownm[r]);
      auto& acc = type_map[type];
      ++acc.count;
      acc.nnz += row_nnz[r];
      acc.max_abs = std::max(acc.max_abs, row_max[r]);
      if (row_nnz[r] > 0) {
        acc.min_abs = std::min(acc.min_abs, row_min[r]);
      }
    }

    row_type_stats_vec.reserve(type_map.size());
    for (auto& [type, acc] : type_map) {
      row_type_stats_vec.push_back({
          .type = std::string(type),
          .count = acc.count,
          .nnz = acc.nnz,
          .max_abs = acc.max_abs,
          .min_abs = acc.min_abs,
      });
    }

    std::ranges::sort(
        row_type_stats_vec,
        [](const auto& a, const auto& b)
        {
          const double ra = (a.min_abs > 0.0
                             && a.min_abs < std::numeric_limits<double>::max())
              ? a.max_abs / a.min_abs
              : 1.0;
          const double rb = (b.min_abs > 0.0
                             && b.min_abs < std::numeric_limits<double>::max())
              ? b.max_abs / b.min_abs
              : 1.0;
          return ra > rb;
        });
  }

  // ── Global objective scaling ──────────────────────────────────────
  // Divide all objective coefficients by scale_objective to improve
  // numerical conditioning.  Applied after equilibration since it is
  // a uniform divisor that does not affect relative magnitudes.
  const auto scale_obj = opts.scale_objective;
  if (scale_obj != 1.0) {
    const auto inv_scale = 1.0 / scale_obj;
    for (auto& v : objval) {
      v *= inv_scale;
    }
  }

  return {
      .ncols = static_cast<fp_index_t>(ncols),
      .nrows = static_cast<fp_index_t>(nrows),
      .matbeg = std::move(matbeg),
      .matind = std::move(matind),
      .matval = std::move(matval),
      .collb = std::move(collb),
      .colub = std::move(colub),
      .objval = std::move(objval),
      .rowlb = std::move(rowlb),
      .rowub = std::move(rowub),
      .colint = std::move(colint),
      .col_scales = std::move(col_scales),
      .row_scales = std::move(row_scales_vec),
      .equilibration_method = eq_method,
      .scale_objective = scale_obj,
      .colnm = std::move(colnm),
      .rownm = std::move(rownm),
      .colmp = std::move(colmp),
      .rowmp = std::move(rowmp),
      .name = pname,  // always copy (trivially small, enables multiple flatten)
      .stats_nnz = stats_nnz,
      .stats_zeroed = stats_zeroed,
      .stats_max_abs = stats_max,
      .stats_min_abs =
          stats_min_col >= 0 ? stats_min : std::numeric_limits<double>::max(),
      .stats_max_col = stats_max_col,
      .stats_min_col = stats_min_col,
      .stats_max_col_name = std::move(stats_max_col_name),
      .stats_min_col_name = std::move(stats_min_col_name),
      .row_type_stats = std::move(row_type_stats_vec),
      .variable_scale_map = std::move(m_vsm_),
      .label_maker = effective_lm,
  };
}

}  // namespace gtopt
