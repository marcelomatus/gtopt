/**
 * @file      lp_matrix_options.hpp
 * @brief     Configuration options for LP matrix assembly
 * @date      Mon Mar 24 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the LpMatrixOptions structure that controls how
 * LinearProblem instances are converted into the flat (column-major)
 * representation consumed by LP solver backends.
 */

#pragma once

#include <optional>
#include <string>

#include <gtopt/basic_types.hpp>
#include <gtopt/lp_matrix_enums.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

// ─── LpMatrixOptions struct ──────────────────────────────────────────────────

/**
 * @struct LpMatrixOptions
 * @brief Configuration options for converting to flat LP representation
 */
struct LpMatrixOptions
{
  double eps {0};  ///< Coefficient epsilon: |v| <= eps is treated as zero.
                   ///< If negative, no filtering is applied.
  double stats_eps {1e-10};  ///< Minimum |coefficient| tracked in stats
                             ///< min/max. Applied in addition to eps: only
                             ///< values with |v| > max(eps, stats_eps) update
                             ///< stats_min_abs. Defaults to 1e-10 for
                             ///< consistency with external LP analysis tools.
  bool col_with_names {true};  ///< Include column names (state vars at level 0)
  bool row_with_names {false};  ///< Include row names (level >= 1)
  bool col_with_name_map {false};  ///< Include column name mapping (level >= 1)
  bool row_with_name_map {false};  ///< Include row name mapping
  bool move_names {true};  ///< Move instead of copy names
  OptBool compute_stats {};  ///< Compute coefficient min/max/ratio
  std::optional<LpEquilibrationMethod>
      equilibration_method {};  ///< Matrix equilibration method.
                                ///< See LpEquilibrationMethod for options.
                                ///< Default is `none` (no scaling).
  std::optional<FastSqrtMethod>
      fast_sqrt_method {};  ///< Approximate sqrt for Ruiz scaling.
                            ///< See FastSqrtMethod for options.
                            ///< Default is `ieee_halve`.
  LpNamesLevel lp_names_level {LpNamesLevel::minimal};  ///< Computed naming
                                                        ///< level (internal)
  double scale_objective {1.0};  ///< Global divisor for all objective
                                 ///< coefficients (numerical conditioning).
                                 ///< Applied uniformly during flatten().
  std::string solver_name {};  ///< Solver backend name (empty = auto-detect)

  /** @brief LP naming level (user-facing JSON/CLI option).
   *
   * - `minimal`:      State-variable column names only (default).
   * - `only_cols`:    All column names + name-to-index maps.
   * - `cols_and_rows`: Column + row names + maps + warn on duplicates.
   *
   * Accepts integer (0/1/2) or string name in JSON.
   */
  std::optional<LpNamesLevel> names_level {};

  /** @brief LP coefficient ratio threshold for numerical conditioning
   * diagnostics.  When the global max/min |coefficient| ratio exceeds this
   * value, a per-scene/phase breakdown is printed.  (default: 1e7) */
  OptReal lp_coeff_ratio_threshold {};

  /// Merge optional fields from another LpMatrixOptions.
  /// Non-optional fields (eps, col_with_names, etc.) are not merged —
  /// first-value-wins semantics like SolverOptions.
  void merge(const LpMatrixOptions& other)
  {
    merge_opt(names_level, other.names_level);
    merge_opt(lp_coeff_ratio_threshold, other.lp_coeff_ratio_threshold);
    merge_opt(equilibration_method, other.equilibration_method);
    merge_opt(fast_sqrt_method, other.fast_sqrt_method);
    merge_opt(compute_stats, other.compute_stats);
  }
};

}  // namespace gtopt
