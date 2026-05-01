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
#include <gtopt/lp_validation.hpp>
#include <gtopt/planning_enums.hpp>  // CompressionCodec
#include <gtopt/sddp_enums.hpp>  // LowMemoryMode
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
  bool col_with_names {
      false,
  };  ///< Include column names (state vars at level 0)
  bool row_with_names {false};  ///< Include row names (level >= 1)
  bool col_with_name_map {false};  ///< Include column name mapping (level >= 1)
  bool row_with_name_map {false};  ///< Include row name mapping
  bool move_names {true};  ///< Move instead of copy names
  OptBool compute_stats {};  ///< Compute coefficient min/max/ratio
  std::optional<LpEquilibrationMethod>
      equilibration_method {};  ///< Matrix equilibration method.
                                ///< See LpEquilibrationMethod for options.
                                ///< Default is `row_max`.
  std::optional<FastSqrtMethod>
      fast_sqrt_method {};  ///< Approximate sqrt for Ruiz scaling.
                            ///< See FastSqrtMethod for options.
                            ///< Default is `ieee_halve`.
  double scale_objective {1.0};  ///< Global divisor for all objective
                                 ///< coefficients (numerical conditioning).
                                 ///< Applied uniformly during flatten().
  std::string solver_name {};  ///< Solver backend name (empty = auto-detect)

  /// Low-memory build hint.  When set to a non-`off` value, `SystemLP`'s
  /// constructor skips the initial `LinearInterface::load_flat()` call and
  /// installs the flat LP as a deferred snapshot instead — the solver
  /// backend is reconstructed lazily on first use.  Used by SDDP and
  /// cascade methods to avoid loading every (scene, phase) backend
  /// upfront only to release it again before the first solve.
  /// Default `off`: backend is loaded eagerly (current behavior).
  LowMemoryMode low_memory_mode {LowMemoryMode::off};

  /// Codec for the deferred snapshot when `low_memory_mode == compress`.
  /// `auto_select` defers to `select_codec()` which prefers lz4.
  CompressionCodec memory_codec {CompressionCodec::auto_select};

  /// Compute LP fingerprint (structural hash) during flatten.
  /// Default false — only enabled when `--lp-fingerprint` is set.
  bool compute_fingerprint {false};

  /** @brief LP coefficient ratio threshold for numerical conditioning
   * diagnostics.  When the global max/min |coefficient| ratio exceeds this
   * value, a per-scene/phase breakdown is printed.  (default: 1e7) */
  OptReal lp_coeff_ratio_threshold {};

  /// Build-time LP validation thresholds and on/off flag.  When enabled
  /// (default true at the consumer site), LinearInterface emits
  /// spdlog::warn lines as bad coefficients / bounds / RHS / objective
  /// values land in the LP, capped per-kind to avoid log-spam.  See
  /// LpValidationOptions for individual thresholds.
  LpValidationOptions validation {};

  /// Merge optional fields from another LpMatrixOptions.
  /// Non-optional fields (eps, col_with_names, etc.) are not merged —
  /// first-value-wins semantics like SolverOptions.
  void merge(const LpMatrixOptions& other)
  {
    merge_opt(lp_coeff_ratio_threshold, other.lp_coeff_ratio_threshold);
    merge_opt(equilibration_method, other.equilibration_method);
    merge_opt(fast_sqrt_method, other.fast_sqrt_method);
    merge_opt(compute_stats, other.compute_stats);
    validation.merge(other.validation);
  }
};

}  // namespace gtopt
