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

LinearProblem::LinearProblem(std::string name, const size_t rsize)
    : pname(std::move(name))
{
  rows.reserve(rsize);
  cols.reserve(rsize);
}

void LinearProblem::set_coeff(const index_t row,
                              const index_t col,
                              const double coeff,
                              const double eps)
{
  if (std::abs(coeff) > eps) {
    rows[row][col] = coeff;
    ++ncoeffs;
  }
}

double LinearProblem::get_coeff(const index_t row, const index_t col) const
{
  return rows[row][col];
}

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
      for (size_t i = 0; i < nrows; ++i) {
        for (auto [j, v] : rows[i].cmap) {
          A[j].emplace(i, v);
          ++nnzero;
        }
      }
    } else [[likely]] {
      for (size_t i = 0; i < nrows; ++i) {
        for (auto [j, v] : rows[i].cmap) {
          if (v > eps || -v > eps) [[likely]] {  // std::abs(v) > eps
            A[j].emplace(i, v);
            ++nnzero;
          }
        }
      }
    }

    matind.resize(nnzero);
    matval.resize(nnzero);
    for (size_t ic = 0, ii = 0; const auto& ai : A) {
      matbeg[ic] = static_cast<fp_index_t>(ii);
      ++ic;
      for (auto [j, aij] : ai) {
        matind[ii] = static_cast<fp_index_t>(j);
        matval[ii] = aij;
        ++ii;
      }
    }
    matbeg[ncols] = static_cast<fp_index_t>(nnzero);
  }

  std::vector<double> rowlb(nrows);
  std::vector<double> rowub(nrows);
  for (size_t i = 0; i < nrows; ++i) {
    const auto& r = rows[i];
    rowlb[i] = r.lowb;
    rowub[i] = r.uppb;
  }

  std::vector<double> collb(ncols);
  std::vector<double> colub(ncols);
  std::vector<double> objval(ncols);
  std::vector<fp_index_t> colint;
  colint.reserve(colints);

  for (size_t i = 0; i < ncols; ++i) {
    const auto& c = cols[i];
    collb[i] = c.lowb;
    colub[i] = c.uppb;
    objval[i] = c.cost;
  }

  for (size_t i = 0; i < ncols; ++i) {
    if (cols[i].is_integer) [[unlikely]] {
      colint.push_back(static_cast<fp_index_t>(i));
    }
  }

  using fp_name_vec_t = FlatLinearProblem::name_vec_t;

  fp_name_vec_t colnm;
  if (opts.col_with_names || opts.col_with_name_map) [[unlikely]] {
    colnm.reserve(ncols);
    if (opts.move_names) [[likely]] {
      for (auto& col : cols) {
        colnm.emplace_back(std::move(col.name));
      }
    } else [[unlikely]] {
      for (auto& col : cols) {
        colnm.emplace_back(col.name);
      }
    }
  }

  fp_name_vec_t rownm;
  if (opts.row_with_names || opts.row_with_name_map) [[unlikely]] {
    rownm.reserve(nrows);
    if (opts.move_names) [[likely]] {
      for (auto& row : rows) {
        rownm.emplace_back(std::move(row.name));
      }
    } else [[unlikely]] {
      for (auto& row : rows) {
        rownm.emplace_back(row.name);
      }
    }
  }

  using fp_index_map_t = FlatLinearProblem::index_map_t;

  fp_index_map_t colmp;
  if (opts.col_with_name_map) [[unlikely]] {
    colmp.reserve(ncols);
    for (fp_index_t i = 0; auto&& name : colnm) {
      if (!colmp.emplace(name, i++).second) [[unlikely]] {
        const auto msg = std::format("repeated column name {}", name);
        SPDLOG_WARN(msg);
      }
    }
  }

  fp_index_map_t rowmp;
  if (opts.row_with_name_map) [[unlikely]] {
    rowmp.reserve(nrows);
    for (fp_index_t i = 0; auto&& name : rownm) {
      if (!rowmp.emplace(name, i++).second) [[unlikely]] {
        const auto msg = std::format("repeated row name {}", name);
        SPDLOG_WARN(msg);
      }
    }
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
          .name = pname};
}

}  // namespace gtopt
