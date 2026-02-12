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

#include <gtopt/basic_types.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

auto LinearProblem::to_flat(const FlatOptions& opts) -> FlatLinearProblem
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

  // Pass 2: fill matind and matval using column offsets
  // Use a working copy of matbeg as write cursors
  std::vector<fp_index_t> colpos(matbeg.begin(), matbeg.begin() + ncols);

  for (const auto& [i, row] : std::views::enumerate(rows)) {
    for (const auto& [j, v] : row.cmap) {
      if (eps < 0 || std::abs(v) > eps) [[likely]] {
        const auto c = static_cast<size_t>(j);
        const auto pos = static_cast<size_t>(colpos[c]);
        matind[pos] = static_cast<fp_index_t>(i);
        matval[pos] = v;
        ++colpos[c];
      }
    }
  }

  std::vector<double> rowlb(nrows);
  std::vector<double> rowub(nrows);
  for (const auto& [i, row] : std::views::enumerate(rows)) {
    rowlb[i] = row.lowb;
    rowub[i] = row.uppb;
  }

  std::vector<double> collb(ncols);
  std::vector<double> colub(ncols);
  std::vector<double> objval(ncols);
  std::vector<fp_index_t> colint;
  colint.reserve(colints);

  for (const auto& [i, col] : std::views::enumerate(cols)) {
    collb[i] = col.lowb;
    colub[i] = col.uppb;
    objval[i] = col.cost;

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

    for (const auto& [i, name] : std::views::enumerate(names)) {
      if (auto [it, inserted] = map.try_emplace(name, i); !inserted)
          [[unlikely]]
      {
        const auto msg = std::format(
            "linear problem using repeated {} name {}", entity_type, name);
        SPDLOG_WARN(msg);
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
      .colnm = std::move(colnm),
      .rownm = std::move(rownm),
      .colmp = std::move(colmp),
      .rowmp = std::move(rowmp),
      .name = opts.move_names ? std::move(pname) : pname,
  };
}

}  // namespace gtopt
