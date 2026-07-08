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

  void merge(const CascadeTransition& opts)
  {
    merge_opt(inherit_optimality_cuts, opts.inherit_optimality_cuts);
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
  /// First-N selector for this level (nullopt = inherit).
  /// See `SddpOptions::num_apertures` for full semantics — paired with
  /// the wettest-first sort applied to `Phase::apertures` by `plp2gtopt`,
  /// `num_apertures = N` picks the N wettest per phase at this level.
  OptInt num_apertures {};
  /// Aperture selection rule for this level (nullopt = inherit).
  /// One of `"head"` (default), `"stride"` / `"interleave"` / `"spread"`,
  /// or `"tail"` / `"last"`.  See `SddpOptions::aperture_selection_mode`
  /// for full semantics.
  OptName aperture_selection_mode {};
  /// Aperture re-solve mode for this level (nullopt = inherit base
  /// ``SDDPOptions::aperture_solve_mode``).  Small single-bus levels
  /// (warmup / uninodal) win with ``"warm"`` (basis reuse across a serial
  /// chunk); the large multi-bus levels (transport / full_network) are
  /// net-negative under warm-start and should use ``"reduced_cost"`` (cold
  /// barrier, no crossover) so chunks can be split for parallelism.
  OptName aperture_solve_mode {};
  /// Aperture chunk size for this level (nullopt = inherit base
  /// ``SDDPOptions::aperture_chunk_size``).  ``0`` = auto (≈2× cores tasks
  /// per phase, the balanced default), ``-1`` = fully serial per scene
  /// (one LP clone, warm-start reuse, but caps parallelism at S tasks),
  /// ``K`` = K apertures per chunk.  Pair ``-1``/large with ``"warm"`` and
  /// ``0``/small with ``"reduced_cost"``.
  OptInt aperture_chunk_size {};
  /// Number of dual-shared cuts re-solved exactly under
  /// ``aperture_solve_mode = "screened"`` for this level (nullopt =
  /// inherit base ``SDDPOptions::aperture_screen_count``).  Ignored by
  /// every other mode.
  OptInt aperture_screen_count {};
  /// Seed each iteration's first aperture from the previous iteration's
  /// first-aperture basis for this level (nullopt = inherit base
  /// ``SDDPOptions::aperture_seed_basis``).  Only acts with cold/warm modes.
  OptBool aperture_seed_basis {};
  /// Convergence tolerance for this level.
  OptReal convergence_tol {};
  /// Stationary-gap tolerance for this level (nullopt = inherit base).
  /// Per-level override of ``SDDPOptions::stationary_tol``: plp2gtopt
  /// emits looser values for early cascade levels (e.g. 4 % at warmup,
  /// 2 % at uninodal) so they can early-exit on a wide stationary
  /// plateau without waiting for the strict ``1 %`` tail levels need.
  OptReal stationary_tol {};
  /// Maximum ``|gap|`` for stationary convergence at this level
  /// (nullopt = inherit base ``SDDPOptions::stationary_gap_ceiling``).
  /// Early cascade levels with reduced apertures have a structural UB
  /// underestimate and the gap can sit at +40% on warmup / +25% on
  /// uninodal even when Δgap has plateaued; without raising the ceiling
  /// here the ``stationary_tol`` threshold alone cannot trip
  /// convergence.  plp2gtopt emits a uniform 50% across all levels.
  OptReal stationary_gap_ceiling {};
  /// Number of consecutive iterations Δgap must stay below
  /// ``stationary_tol`` before stationary convergence trips (nullopt =
  /// inherit base ``SDDPOptions::stationary_window``).  Looser early
  /// levels can use a shorter window (e.g. 2) to short-circuit on a
  /// shallow plateau; the tail level may want a stricter window (e.g.
  /// 8) to filter sampling noise.
  OptInt stationary_window {};
  /// Elastic-cut mode override for this level (nullopt = inherit base
  /// ``SDDPOptions::elastic_mode``).  Warmup is typically fine with
  /// ``single_cut`` while ``full_network`` benefits from ``multi_cut``
  /// once aperture coverage is broad enough to justify the extra cut
  /// rows.  String matches the JSON enum spelling.
  OptName elastic_mode {};
  /// Elastic-cut penalty override for this level in $/MWh of
  /// elastic-state violation (nullopt = inherit base
  /// ``SDDPOptions::elastic_penalty``).  Lower at warmup (looser
  /// feasibility) and tighter at full_network (penalty must be
  /// commensurate with the physical-cost scale).
  OptReal elastic_penalty {};

  void merge(const CascadeLevelMethod& opts)
  {
    merge_opt(max_iterations, opts.max_iterations);
    merge_opt(min_iterations, opts.min_iterations);
    merge_opt(apertures, opts.apertures);
    merge_opt(num_apertures, opts.num_apertures);
    merge_opt(aperture_selection_mode, opts.aperture_selection_mode);
    merge_opt(aperture_solve_mode, opts.aperture_solve_mode);
    merge_opt(aperture_chunk_size, opts.aperture_chunk_size);
    merge_opt(aperture_screen_count, opts.aperture_screen_count);
    merge_opt(aperture_seed_basis, opts.aperture_seed_basis);
    merge_opt(convergence_tol, opts.convergence_tol);
    merge_opt(stationary_tol, opts.stationary_tol);
    merge_opt(stationary_gap_ceiling, opts.stationary_gap_ceiling);
    merge_opt(stationary_window, opts.stationary_window);
    merge_opt(elastic_mode, opts.elastic_mode);
    merge_opt(elastic_penalty, opts.elastic_penalty);
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
  /// Optional path to a Planning JSON whose ``system`` (bus_array,
  /// line_array, component bus refs) replaces the parent planning's
  /// system for this level only.  Used by the cascade-reduced mode: the
  /// reducer emits per-level reduced JSONs and this field points each
  /// cascade level at its dedicated one.  Resolution: tried as-is from
  /// the current working directory first, then under
  /// ``options.input_directory`` as a fallback.  Only ``.system`` is
  /// taken from the loaded JSON; its ``.options`` and ``.simulation``
  /// blocks are deliberately ignored — the cascade level's
  /// ``model_options`` overlay remains authoritative.
  OptName system_file {};
  /// Optional path to a Planning JSON whose ``system`` (and its
  /// ``options.model_options``) is used as the **aperture system** for
  /// this level's SDDP backward pass — the simplified model solved per
  /// aperture, distinct from this level's forward ``system_file`` /
  /// ``model_options``.  Resolution chain (highest first):
  /// ``Phase::aperture_system_file`` → this field →
  /// ``sddp_options.aperture_system_file`` → regular forward system.
  /// The parent ``simulation`` is reused; the reduced system must keep
  /// the same reservoir/storage/α inter-phase state elements (by ``uid``).
  OptName aperture_system_file {};
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
  void merge(CascadeLevel opts)
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

    merge_opt(system_file, std::move(opts.system_file));
    merge_opt(aperture_system_file, std::move(opts.aperture_system_file));

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
        transition = opts.transition;
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
