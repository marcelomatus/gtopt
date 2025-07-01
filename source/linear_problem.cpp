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
  std::vector<fp_index_t> matbeg(ncols + 1);
  std::vector<fp_index_t> matind;
  std::vector<double> matval;

  size_t nnzero = 0;
  {
    SparseMatrix A(ncols);
    if (opts.reserve_matrix) {
      const auto avg_size = static_cast<size_t>(
          (opts.reserve_factor * static_cast<double>(ncoeffs)
           / static_cast<double>(ncols))
          + 1);

      for (auto& ai : A) {
        ai.reserve(avg_size);
      }

      SPDLOG_TRACE("reserving matrix with avg_size of {}", avg_size);
    }

    const auto eps = opts.eps;
    if (eps < 0) [[unlikely]] {
      for (const auto& [i, row] : std::views::enumerate(rows)) {
        for (const auto& [j, v] : row.cmap) {
          A[j].emplace(std::piecewise_construct,
                       std::forward_as_tuple(i),
                       std::forward_as_tuple(v));
          ++nnzero;
        }
      }
    } else [[likely]] {
      for (const auto& [i, row] : std::views::enumerate(rows)) {
        for (const auto& [j, v] : row.cmap) {
          if (std::abs(v) > eps) [[likely]] {
            A[j].emplace(std::piecewise_construct,
                         std::forward_as_tuple(i),
                         std::forward_as_tuple(v));
            ++nnzero;
          }
        }
      }
    }

    matind.resize(nnzero);
    matval.resize(nnzero);
    for (size_t ii = 0; const auto& [ic, ai] : std::views::enumerate(A)) {
      matbeg[ic] = static_cast<fp_index_t>(ii);
      for (const auto [j, aij] : ai) {
        matind[ii] = static_cast<fp_index_t>(j);
        matval[ii] = aij;
        ++ii;
      }
    }
    matbeg[ncols] = static_cast<fp_index_t>(nnzero);
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

  using fp_name_vec_t = FlatLinearProblem::name_vec_t;

  auto build_name_vector =
      [](auto& source, auto& dest, size_t size, bool move_names)
  {
    dest.reserve(size);
    for (auto& item : source) {
      dest.emplace_back(move_names ? std::move(item.name) : item.name);
    }
  };

  fp_name_vec_t colnm;
  if (opts.col_with_names || opts.col_with_name_map) [[unlikely]] {
    build_name_vector(cols, colnm, ncols, opts.move_names);
  }

  fp_name_vec_t rownm;
  if (opts.row_with_names || opts.row_with_name_map) [[unlikely]] {
    build_name_vector(rows, rownm, nrows, opts.move_names);
  }

  using fp_index_map_t = FlatLinearProblem::index_map_t;

  auto build_name_map =
      [](const auto& names, auto& map, std::string_view entity_type)
  {
    map.reserve(names.size());
    for (const auto& [i, name] : std::views::enumerate(names)) {
      if (auto [it, inserted] = map.try_emplace(name, i); !inserted)
          [[unlikely]]
      {
        const auto msg = fmt::format("repeated {} name {}", entity_type, name);
        SPDLOG_WARN(msg);
      }
    }
  };

  fp_index_map_t colmp;
  if (opts.col_with_name_map) [[unlikely]] {
    build_name_map(colnm, colmp, "column");
  }

  fp_index_map_t rowmp;
  if (opts.row_with_name_map) [[unlikely]] {
    build_name_map(rownm, rowmp, "row");
  }

#ifdef GTOPT_TRACE_LINEAR_PROBLEM
  {
    const double s_ratio = static_cast<double>(nnzero)
        / static_cast<double>(nrows) / static_cast<double>(ncols);
    SPDLOG_TRACE(
        "flattening lp with "
        " {} constraints, "
        " {} variables, "
        " {} nnzeros, "
        " {} opts-eps,"
        " {} s_ratio",
        nrows,
        ncols,
        nnzero,
        opts.eps,
        s_ratio);
  }
#endif

  return {.ncols = static_cast<fp_index_t>(ncols),
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
          .name = opts.move_names ? std::move(pname) : pname};
}

}  // namespace gtopt
