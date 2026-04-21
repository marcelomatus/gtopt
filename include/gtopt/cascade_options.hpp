/**
 * @file      cascade_options.hpp
 * @brief     Cascade solver configuration: transitions, levels, and options
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/model_options.hpp>
#include <gtopt/sddp_options.hpp>

namespace gtopt
{

/**
 * @brief Transition configuration: how a cascade level receives
 *        information from the previous level.
 */
struct CascadeTransition
{
  /// Carry forward optimality cuts (Benders cuts) from previous level.
  /// The value controls when inherited cuts are dropped ("forgotten"):
  ///   - absent or 0: do not inherit
  ///   - -1:          inherit and keep forever
  ///   - N > 0:       inherit but forget after N training iterations,
  ///                  then re-solve with only self-generated cuts
  OptInt inherit_optimality_cuts {};
  /// Add elastic state variable target constraints from previous
  /// solution.  Same semantics as inherit_optimality_cuts:
  ///   - absent or 0: do not inherit
  ///   - -1:          inherit and keep forever
  ///   - N > 0:       inherit but remove target constraints after N
  ///                  training iterations
  OptInt inherit_targets {};
  /// Relative tolerance for target band.  Default: 0.05 (5%).
  OptReal target_rtol {};
  /// Minimum absolute tolerance for target band.  Default: 1.0.
  OptReal target_min_atol {};
  /// Elastic penalty cost per unit violation of target.  Default: 500.
  OptReal target_penalty {};
  /// Minimum |dual| threshold for transferring cuts.  Cuts with
  /// |dual| < threshold are considered inactive and skipped.
  /// Default: 0.0 (transfer all cuts regardless of dual).
  OptReal optimality_dual_threshold {};

  void merge(const CascadeTransition& opts)
  {
    merge_opt(inherit_optimality_cuts, opts.inherit_optimality_cuts);
    merge_opt(inherit_targets, opts.inherit_targets);
    merge_opt(target_rtol, opts.target_rtol);
    merge_opt(target_min_atol, opts.target_min_atol);
    merge_opt(target_penalty, opts.target_penalty);
    merge_opt(optimality_dual_threshold, opts.optimality_dual_threshold);
  }
};

/**
 * @brief Solver options for one cascade level.
 */
struct CascadeLevelMethod
{
  /// Maximum iterations for this level.
  OptInt max_iterations {};
  /// Minimum iterations before convergence can be declared.
  OptInt min_iterations {};
  /// Aperture UIDs for this level (nullopt = inherit, empty = Benders).
  std::optional<Array<Uid>> apertures {};
  /// Convergence tolerance for this level.
  OptReal convergence_tol {};

  void merge(const CascadeLevelMethod& opts)
  {
    merge_opt(max_iterations, opts.max_iterations);
    merge_opt(min_iterations, opts.min_iterations);
    merge_opt(apertures, opts.apertures);
    merge_opt(convergence_tol, opts.convergence_tol);
  }
};

/**
 * @brief One cascade level configuration.
 *
 * LP is automatically rebuilt when `model_options` is present.
 * When absent, the previous level's LP and solver are reused.
 */
struct CascadeLevel
{
  /// Unique identifier for this level.
  OptUid uid {};
  /// Human-readable level name (for logging).
  OptName name {};
  /// When `false`, the level is skipped entirely by the cascade solver:
  /// no LP is built, no solver runs, and no state/cuts are produced for
  /// downstream levels.  Intended for quickly disabling a level in
  /// configuration without removing it (useful for boundary tests and
  /// partial runs).  Default: `true` (active).
  OptBool active {};
  /// Model overrides for this level (absent → reuse previous LP).
  std::optional<ModelOptions> model_options {};
  /// SDDP solver options for this level.
  std::optional<CascadeLevelMethod> sddp_options {};
  /// Transition from the previous level.
  std::optional<CascadeTransition> transition {};

  /// Merge another CascadeLevel on top of this one.  Used by
  /// `CascadeOptions::merge` for element-wise level-array merges (e.g.
  /// from --set array-index overlays like
  /// `cascade_options.level_array.0.sddp_options.max_iterations=20`).
  /// Only fields set in @p opts overwrite the corresponding fields here;
  /// unset optionals in @p opts leave the existing values intact.  Nested
  /// optional structs (`model_options`, `sddp_options`, `transition`) are
  /// themselves recursively merged when both sides have a value.
  void merge(CascadeLevel&& opts)
  {
    merge_opt(uid, opts.uid);
    merge_opt(name, std::move(opts.name));
    merge_opt(active, opts.active);

    if (opts.model_options.has_value()) {
      if (model_options.has_value()) {
        model_options->merge(*opts.model_options);
      } else {
        model_options = std::move(opts.model_options);
      }
    }

    if (opts.sddp_options.has_value()) {
      if (sddp_options.has_value()) {
        sddp_options->merge(*opts.sddp_options);
      } else {
        sddp_options = std::move(opts.sddp_options);
      }
    }

    if (opts.transition.has_value()) {
      if (transition.has_value()) {
        transition->merge(*opts.transition);
      } else {
        transition = std::move(opts.transition);
      }
    }
  }
};

/**
 * @brief Cascade solver configuration: variable number of levels.
 *
 * Contains an `SddpOptions` sub-object (`sddp_options`) so that all SDDP
 * options (convergence_tol, cut_sharing_mode, elastic_mode, etc.) can be
 * set at the cascade level and serve as defaults for each level solver.
 *
 * `sddp_options.max_iterations` is used as the **global iteration budget**
 * across all levels (not per-level).  Per-level
 * `CascadeLevelMethod::max_iterations` controls iterations within each level.
 *
 * Each level can have different LP formulation options, solver
 * parameters, and transition rules.  When `level_array` is empty,
 * a single default level is created that passes through all options.
 */
struct CascadeOptions
{
  /// Global model options — serve as defaults for all levels.
  /// Per-level model_options override these when set.
  ModelOptions model_options {};
  /// Global SDDP options — serve as defaults for all levels.
  /// max_iterations here is the global iteration budget across all levels.
  SddpOptions sddp_options {};
  /// Array of cascade level configurations.
  Array<CascadeLevel> level_array {};

  void merge(
      CascadeOptions&&
          opts)  // NOLINT(cppcoreguidelines-rvalue-reference-param-not-moved)
  {
    model_options.merge(opts.model_options);
    sddp_options.merge(std::move(opts.sddp_options));

    // level_array merge rules:
    //  - overlay empty                 → keep base (no change)
    //  - base empty                    → adopt overlay (initial load)
    //  - sizes match, non-empty        → element-wise merge (enables
    //    `--set cascade_options.level_array.N.foo=bar` overlays, where
    //    the overlay array is constructed with (N - 1) empty placeholder
    //    objects and only index N filled — see
    //    build_set_option_json in gtopt_json_io_set.cpp)
    //  - sizes differ                  → replace wholesale (preserves
    //    existing "two JSON files with different level_array sizes
    //    → last-wins" semantics for full-file merges)
    if (opts.level_array.empty()) {
      return;
    }
    if (level_array.empty() || level_array.size() != opts.level_array.size()) {
      level_array = std::move(opts.level_array);
      return;
    }
    for (std::size_t i = 0; i < level_array.size(); ++i) {
      level_array[i].merge(std::move(opts.level_array[i]));
    }
  }
};

}  // namespace gtopt
