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
#include <ranges>
#include <unordered_map>

#include <gtopt/linear_problem.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

auto LinearProblem::lp_build(const LpBuildOptions& opts) -> FlatLinearProblem
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

  std::vector<double> rowlb(nrows);
  std::vector<double> rowub(nrows);

  for (const auto& [i, row] : std::views::enumerate(rows)) {
    rowlb[i] = row.lowb;
    rowub[i] = row.uppb;

    for (const auto& [j, v] : row.cmap) {
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
  std::vector<double> col_scales(ncols, 1.0);
  std::vector<fp_index_t> colint;
  colint.reserve(colints);

  for (const auto& [i, col] : std::views::enumerate(cols)) {
    collb[i] = col.lowb;
    colub[i] = col.uppb;
    objval[i] = col.cost;
    col_scales[i] = col.scale;

    if (col.is_integer) [[unlikely]] {
      colint.push_back(static_cast<fp_index_t>(i));
    }
  }

  // Name vectors
  using fp_name_vec_t = FlatLinearProblem::name_vec_t;
  auto build_name_vector = [](auto& source, bool move_names) -> fp_name_vec_t
  {
    fp_name_vec_t names;
    names.reserve(source.size());

    for (auto& item : source) {
      names.emplace_back(move_names ? std::move(item.name) : item.name);
    }
    return names;
  };

  fp_name_vec_t colnm;
  if (opts.col_with_names || opts.col_with_name_map) [[unlikely]] {
    colnm = build_name_vector(cols, opts.move_names);
  }

  fp_name_vec_t rownm;
  if (opts.row_with_names || opts.row_with_name_map) [[unlikely]] {
    rownm = build_name_vector(rows, opts.move_names);
  }

  // Index name maps
  using fp_index_map_t = FlatLinearProblem::index_map_t;
  auto build_name_map = [](const auto& names,
                           std::string_view entity_type) -> fp_index_map_t
  {
    fp_index_map_t map;
    map.reserve(names.size());

    for (const auto& [i, name] : std::views::enumerate(names)) {
      if (name.empty()) [[unlikely]] {
        continue;  // skip empty names to avoid false-positive duplicates
      }
      if (auto [it, inserted] = map.try_emplace(name, i); !inserted)
          [[unlikely]]
      {
        SPDLOG_WARN(
            "linear problem using repeated {} name {}", entity_type, name);
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

  // ── Row equilibration scaling ─────────────────────────────────────
  // Scale each row so that its largest |coefficient| becomes 1.0.
  // This reduces the condition number by compressing the coefficient
  // range across rows.  The row_scales vector stores the inverse of
  // the scaling factor: dual_physical = dual_LP × row_scale[i].
  std::vector<double> row_scales_vec;
  if (opts.row_equilibration.value_or(true)) {
    // 1. Compute row max |coefficient| from the CSC matrix.
    std::vector<double> row_max(nrows, 0.0);
    for (size_t k = 0; k < matval.size(); ++k) {
      const auto r = static_cast<size_t>(matind[k]);
      row_max[r] = std::max(row_max[r], std::abs(matval[k]));
    }

    // 2. Compute scale = 1/max (identity for empty or zero-max rows).
    row_scales_vec.resize(nrows);
    for (size_t r = 0; r < static_cast<size_t>(nrows); ++r) {
      row_scales_vec[r] = (row_max[r] > 0.0) ? row_max[r] : 1.0;
    }

    // 3. Apply scaling to matrix coefficients.
    for (size_t k = 0; k < matval.size(); ++k) {
      const auto r = static_cast<size_t>(matind[k]);
      matval[k] /= row_scales_vec[r];
    }

    // 4. Scale row bounds (RHS).
    for (size_t r = 0; r < static_cast<size_t>(nrows); ++r) {
      const double s = row_scales_vec[r];
      if (s != 1.0) {
        rowlb[r] /= s;
        rowub[r] /= s;
      }
    }

    // 5. Recompute coefficient stats after equilibration so the reported
    //    ratio reflects the actual matrix sent to the solver.
    if (do_stats) {
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
      stats_max_col_name =
          (stats_max_col >= 0
           && static_cast<size_t>(stats_max_col) < colnm.size())
          ? colnm[static_cast<size_t>(stats_max_col)]
          : "";
      stats_min_col_name =
          (stats_min_col >= 0
           && static_cast<size_t>(stats_min_col) < colnm.size())
          ? colnm[static_cast<size_t>(stats_min_col)]
          : "";
    }
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
      .colnm = std::move(colnm),
      .rownm = std::move(rownm),
      .colmp = std::move(colmp),
      .rowmp = std::move(rowmp),
      .name =
          pname,  // always copy (trivially small, enables multiple lp_build)
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
  };
}

}  // namespace gtopt
