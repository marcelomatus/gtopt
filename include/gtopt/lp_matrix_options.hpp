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
#include <gtopt/phase.hpp>  // PhaseIndex
#include <gtopt/planning_enums.hpp>  // CompressionCodec
#include <gtopt/scene.hpp>  // SceneIndex
#include <gtopt/sddp_enums.hpp>  // LowMemoryMode
#include <gtopt/utils.hpp>

namespace gtopt
{

class LpNameSpillStore;  // fwd-decl: non-owning back-pointer carried below

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

  /// Ruiz equilibration iteration cap and convergence tolerance.  JSON-bound
  /// (sibling of `equilibration_method` / `fast_sqrt_method`), so the
  /// converter can emit them and `--set lp_matrix_options.ruiz_max_iterations`
  /// can tune them.  The iterative Ruiz pass (`apply_ruiz_scaling`) runs at
  /// most `ruiz_max_iterations` row/column rescaling rounds, stopping early
  /// when the worst infinity-norm deviation from 1 drops below
  /// `ruiz_tolerance`.  Only consulted when `equilibration_method == ruiz`.
  ///
  /// Default cap is 5 (was 10).  A CPLEX kappa sweep on the 10-19 PLEXOS and
  /// juan/IPLP LPs showed byte-identical kappa and deterministic ticks for
  /// every cap in {1,5,10,15,20} — CPLEX's own scaling re-equilibrates the
  /// matrix, so additional Ruiz iterations buy no conditioning or solve
  /// speedup while costing build time linearly.  5 keeps a margin of
  /// equilibration headroom for scaling-free backends (HiGHS / CLP) while
  /// halving the per-build Ruiz cost vs the old 10.
  int ruiz_max_iterations {5};
  double ruiz_tolerance {1e-3};

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

  /// Skip the entire `LinearProblem::flatten()` matrix-build body and
  /// return an empty `FlatLinearProblem`.  Used by the write-out
  /// rebuild path in `system_lp.cpp::rebuild_collections_if_needed`
  /// where the produced flat LP is discarded — only the `add_to_lp`
  /// side effects (XLP per-element col/row indices) are needed.
  /// Setting this kills the CSC build pass, the row-bound scan, the
  /// col-bound scan, the label-name pass and the equilibration pass
  /// — typically 5–10× faster on the rebuild slice for juan-scale
  /// cells.  Default false.
  bool skip_matrix_build {false};

  /// Release each cell's AMPL variable + metadata registry immediately
  /// after `flatten()`.  That registry exists only to resolve user
  /// constraints during the build (`find_ampl_col` / `collect_sum_cols`)
  /// and is never read afterwards, so freeing it reclaims memory —
  /// hundreds of MB at CEN scale — for the rest of the solve.  INTERNAL
  /// build-control flag (like `skip_matrix_build`), deliberately NOT
  /// JSON-bound: the production solve path
  /// (`gtopt_lp_runner::prepare_matrix_options`) turns it on, while the
  /// direct `PlanningLP` / `SystemLP` APIs leave it `false` so white-box
  /// tests can still inspect the registry via `find_ampl_col` after
  /// building.  Default false.
  bool release_ampl_after_flatten {false};

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

  /// Optional (scene, phase) UIDs of the LP cell currently being
  /// flattened.  Used to prepend `[s14 p46]` to per-cell LP_QUALITY
  /// messages so they line up with `SDDP Forward / Backward` info
  /// lines (which also print UIDs, not internal indices).  Set by
  /// `system_lp.cpp::flatten_from_collections` and read by
  /// `LinearProblem::flatten`.  Stored as strong-typed UIDs, not a
  /// pre-formatted string, so consumers can also use them for
  /// structured logging or per-cell stat aggregation.  Not merged
  /// across LpMatrixOptions.
  std::optional<SceneUid> flatten_scene_uid {};
  std::optional<PhaseUid> flatten_phase_uid {};

  /// Non-owning back-pointer to the run-lifetime async metadata store
  /// (`PlanningLP::m_name_store_`).  When non-null (set only when names are
  /// kept and the method is not monolithic), `create_linear_interface` spills
  /// each cell's label metadata here and drops the resident copy; `write_lp`
  /// reloads it on demand.  Null disables the spill (current behaviour).
  /// Build-control carrier only — NOT merged, serialised, or compared.
  LpNameSpillStore* name_store {nullptr};

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
