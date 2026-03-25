/**
 * @file      lp_stats.hpp
 * @brief     Static analysis of LP coefficient matrices for numerical
 *            conditioning diagnostics
 * @date      2026-03-16
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides ScenePhaseLPStats and log_lp_stats_summary() — a lightweight
 * reporting facility that logs min/max absolute coefficient, ratio, and
 * conditioning quality per scene/phase.  When the global coefficient ratio
 * is below a configurable threshold the output is a single summary line;
 * otherwise a per-scene/phase table is printed.
 *
 * The actual min/max/nnz values are computed inside LinearProblem::lp_build()
 * when LpBuildOptions::compute_stats is true, and stored in the
 * FlatLinearProblem struct.
 */

#pragma once

#include <limits>
#include <string>
#include <vector>

#include <gtopt/linear_problem.hpp>

namespace gtopt
{

/**
 * @brief Label + stats for a single scene/phase LP.
 */
struct ScenePhaseLPStats
{
  int scene_uid {};
  int phase_uid {};
  int num_vars {};
  int num_constraints {};
  size_t stats_nnz {};
  size_t stats_zeroed {};  ///< Non-zero entries filtered to zero by eps
  double stats_max_abs {};
  double stats_min_abs {};

  int stats_max_col {-1};  ///< Column index with largest |coefficient|
  int stats_min_col {-1};  ///< Column index with smallest |coefficient|

  std::string stats_max_col_name {};  ///< Name of column with largest |coeff|
  std::string stats_min_col_name {};  ///< Name of column with smallest |coeff|

  [[nodiscard]] constexpr double coeff_ratio() const noexcept
  {
    if (stats_min_abs <= 0.0 || stats_min_col < 0
        || stats_min_abs >= std::numeric_limits<double>::max()
        || stats_min_abs == stats_max_abs || stats_nnz == 0)
    {
      return 1.0;
    }
    return stats_max_abs / stats_min_abs;
  }

  [[nodiscard]] constexpr const char* quality_label() const noexcept
  {
    const auto ratio = coeff_ratio();
    if (ratio <= 1e3) {
      return "excellent";
    }
    if (ratio <= 1e5) {
      return "good";
    }
    if (ratio <= 1e7) {
      return "fair";
    }
    return "poor";
  }
};

/**
 * @brief Log a summary of LP coefficient statistics.
 *
 * When the global coefficient ratio is below @p ratio_threshold a single
 * summary line is emitted.  Otherwise a per-scene/phase table is printed
 * followed by the global aggregate.
 *
 * @param entries      Per-scene/phase statistics.
 * @param ratio_threshold  Ratio above which the detailed table is shown
 *                         (default 1e7).
 */
void log_lp_stats_summary(const std::vector<ScenePhaseLPStats>& entries,
                          double ratio_threshold = 1e7);

}  // namespace gtopt
