/**
 * @file      matrix_stats.hpp
 * @brief     Matrix-wide numerical statistics for an LP matrix
 * @date      2026-07-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Plain value aggregate holding the conditioning statistics captured from a
 * `FlatLinearProblem` after flatten (non-zero count, largest/smallest
 * coefficient and the columns owning them, per-row-type stats).  Extracted
 * from `LinearInterface` as the first step of decomposing that class: it is a
 * behaviour-free struct owned by value, so it carries no coupling back to the
 * interface and can be constructed and asserted on in isolation.
 */

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <gtopt/linear_problem.hpp>  // FlatLinearProblem::RowTypeStatsEntry
#include <gtopt/sparse_col.hpp>  // ColIndex

namespace gtopt
{

/// Matrix-wide numerical statistics captured from a `FlatLinearProblem` after
/// flatten.  Surfaced through `LinearInterface`'s stats accessors for
/// conditioning diagnostics.  Copied verbatim from the flat problem's
/// `stats_*` fields (see `LinearInterface::load_flat`).
struct MatrixStats
{
  std::size_t nnz {};  ///< non-zero coefficient count
  std::size_t zeroed {};  ///< coefficients thresholded to zero
  double max_abs {};  ///< largest |coefficient|
  double min_abs {};  ///< smallest non-zero |coefficient|
  std::optional<ColIndex> max_col {};  ///< column owning max_abs
  std::optional<ColIndex> min_col {};  ///< column owning min_abs
  std::string max_col_name {};  ///< name of max_col (if known)
  std::string min_col_name {};  ///< name of min_col (if known)
  std::vector<FlatLinearProblem::RowTypeStatsEntry> row_type_stats {};
};

}  // namespace gtopt
