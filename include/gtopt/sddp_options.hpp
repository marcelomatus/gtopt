/**
 * @file      sddp_options.hpp
 * @brief     SDDP-specific solver configuration parameters
 * @date      2026-03-24
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/planning_enums.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

/**
 * @brief SDDP-specific solver configuration parameters
 *
 * Groups all SDDP-related options into a single sub-object for clearer
 * JSON organization.  Field names omit the `sddp_` prefix since they
 * already live inside the `sddp_options` namespace.
 *
 * All fields are optional — defaults are applied via `PlanningOptionsLP`.
 */
struct SddpOptions  // NOLINT(clang-analyzer-optin.performance.Padding)
{
  /** @brief Cut sharing mode: none (default), multicut, or markov
   * (opt-in, experimental — requires `markov_states` +
   * `markov_transition`).  The legacy expected/broadcast_mean,
   * accumulate, and max modes were REMOVED 2026-07-08 (invalid —
   * `docs/formulation/sddp-cut-validity.md` §7); their names now
   * hard-error at JSON parse time. */
  std::optional<CutSharingMode> cut_sharing_mode {};
  /** @brief Forward-pass sampling mode: persistent (default) or
   * resampled.
   *
   * `persistent` keeps each scene-driver on its own scenario path
   * (historical, byte-identical).  `resampled` re-draws a
   * probability-weighted scene realization at every phase boundary
   * (deterministic in (iteration, scene, phase)) and applies it via
   * the bound-only `update_aperture` machinery, so the forward UB
   * estimates the same stagewise-resampled process the
   * `cut_sharing_mode = multicut` LB certifies.  See
   * `ForwardSamplingMode` in `sddp_enums.hpp`. */
  std::optional<ForwardSamplingMode> forward_sampling_mode {};
  /** @brief Scene → Markov-state assignment for
   * `cut_sharing_mode = markov`: one integer per scene (in scene
   * order), each in `[0, M)` where `M` is the transition matrix
   * dimension.  Static per scene (v1).  Validated at SDDP setup; see
   * `docs/formulation/sddp-markov.md` §1. */
  std::optional<Array<int>> markov_states {};
  /** @brief Row-major M×M row-stochastic Markov transition matrix for
   * `cut_sharing_mode = markov` (`M` inferred from the array length,
   * which must be a perfect square).  Rows must sum to ≈ 1
   * (tolerance 1e-6) with non-negative entries.  See
   * `docs/formulation/sddp-markov.md` §1. */
  std::optional<Array<double>> markov_transition {};
  /** @brief Directory for Benders cut files (default: `"cuts"`) */
  OptName cut_directory {};
  /** @brief Enable the SDDP monitoring API (writes JSON status file each
   * iteration; default: true) */
  OptBool api_enabled {};
  /** @brief Iterations to skip between update_lp dispatches.
   * 0 = update every iteration (default).  Applies to all volume-dependent
   * LP element updates (seepage, discharge limit, production factor). */
  OptInt update_lp_skip {};

  // ── Iteration control ──────────────────────────────────────────────────────
  /** @brief Maximum number of forward/backward iterations (default: 100) */
  OptInt max_iterations {};
  /** @brief Minimum iterations before declaring convergence (default: 2) */
  OptInt min_iterations {};
  /** @brief Relative gap tolerance for convergence (default: 1e-4) */
  OptReal convergence_tol {};

  // ── Advanced tuning ────────────────────────────────────────────────────────
  //
  // Naming conventions for fields below — kept stable on purpose:
  //
  //   * ``scale_*``       -> mirrors ``scale_objective`` / ``scale_theta``
  //                          in ``model_options`` and ``lp_matrix_options``
  //                          (any ``scale_X`` is "divisor that keeps the
  //                          LP coefficients of variable X near unity").
  //   * ``*_eps``         -> mirrors ``optimal_eps``, ``feasible_eps``,
  //                          ``barrier_eps``, ``stats_eps``, ``matrix_eps``
  //                          (numerical-tolerance parameters).
  //   * ``*_threshold``   -> trigger thresholds on derived metrics
  //                          (e.g. ``lp_coeff_ratio_threshold``,
  //                          ``prune_dual_threshold``).
  //
  // Renaming any of ``scale_alpha`` / ``multi_cut_threshold`` /
  // ``cut_coeff_eps`` would break: existing JSON inputs, golden test
  // fixtures, the formulation white paper, and external CI pipelines
  // that pin these keys.  The names are kept as the canonical
  // representation; the doxygen blocks below carry the
  // expert-facing explanation.
  /** @brief Penalty for elastic slack variables in feasibility.
   *  Default: `PlanningOptionsLP::default_sddp_elastic_penalty`
   *  (see `planning_options_lp.hpp`). */
  OptReal elastic_penalty {};
  /** @brief Scale divisor for future cost variable α (default: 0 = auto).
   *
   * The LP alpha variable is α_lp = α / scale_alpha, with an objective
   * coefficient of scale_alpha so that the physical contribution is
   * preserved.  When 0 (default), auto-computed as max(var_scale) across
   * all state variables — keeps α O(1) relative to the largest state
   * variable in LP units. */
  OptReal scale_alpha {};

  // ── Cut file management ────────────────────────────────────────────────────
  /** @brief Cut persistence mode: none (default), keep, append, or replace.
   *  Controls whether to load cuts from a previous run and how to handle
   *  the combined output file on completion. */
  std::optional<HotStartMode> cut_recovery_mode {};
  /** @brief Recovery mode: none (0), cuts (1), or full (2).
   *  Controls what is recovered from a previous run:
   *  - none:  no recovery (cold start).
   *  - cuts:  recover only Benders cuts.
   *  - full:  recover cuts + state variable solutions (default). */
  std::optional<RecoveryMode> recovery_mode {};
  /** @brief Save cuts after each iteration (default: true).
   *  When false, cuts are only saved at the end of the solve or on stop. */
  OptBool save_per_iteration {};
  /** @brief File path for loading initial cuts (hot-start; empty = cold start)
   */
  OptName cuts_input_file {};
  /** @brief Path to a sentinel file; if it exists, the solver stops gracefully
   * after the current iteration (analogous to PLP's userstop) */
  OptName sentinel_file {};
  /** @brief Elastic filter mode: chinneck (default, alias "iis"),
   *         single_cut (alias "cut"), or multi_cut.
   *         See ElasticFilterMode in sddp_enums.hpp for semantics.
   *         The legacy "backpropagate" mode is no longer supported;
   *         it falls through to the default. */
  std::optional<ElasticFilterMode> elastic_mode {};
  /** @brief Forward-pass infeasibility count threshold for switching
   *         from single_cut to multi_cut.  Default:
   *         `PlanningOptionsLP::default_sddp_multi_cut_threshold`
   *         (see `planning_options_lp.hpp`).  `0 = always multi_cut`;
   *         `< 0 = disabled`.
   *
   *         The counter is **persistent**: it accumulates across
   *         iterations and is not reset when a (scene, phase) solves
   *         feasibly.  Switch fires when `infeas_count >= threshold`. */
  OptInt multi_cut_threshold {};
  /** @brief Aperture UIDs for the backward pass.
   *
   * - absent (nullopt) – use per-phase `Phase::apertures` (default)
   * - empty array `[]` – no apertures (pure Benders)
   * - non-empty `[1,2,3]` – use exactly these aperture UIDs,
   *   overriding per-phase apertures
   */
  std::optional<Array<Uid>> apertures {};

  /** @brief First-N selector applied to each phase's `Phase::apertures`.
   *
   * - absent (nullopt) – no truncation; use the full per-phase list
   * - `0`              – no apertures (pure Benders, same as `apertures=[]`)
   * - `N > 0`          – take `Phase::apertures.first(N)` per phase
   *
   * Designed to pair with the wettest-first sort applied to
   * `Phase::apertures` by `plp2gtopt`: `num_apertures = N` picks the N
   * wettest apertures *per phase*, without the writer having to
   * materialise an explicit cross-phase UID whitelist.
   *
   * **Interaction with `apertures`**: the two compose.  `apertures` is
   * the global UID whitelist for `aperture_array` lookup; truncation
   * by `num_apertures` happens on `Phase::apertures` first, then each
   * surviving UID is resolved against the (possibly whitelisted)
   * aperture pool by `build_effective_apertures`.  A UID truncated
   * away by `num_apertures` is dropped before the whitelist check;
   * a UID present after truncation but absent from the whitelist is
   * dropped at lookup time.
   *
   * **Cascade levels**: each `CascadeLevelMethod` can override
   * `num_apertures` via the existing `merge_opt` path — e.g. L0
   * (uninodal) sets `num_apertures = 4`, L1 (transport) sets
   * `num_apertures = 8`, L2 (full) leaves it absent (= all apertures
   * per phase).  No `apertures` whitelist is needed at any level.
   */
  OptInt num_apertures {};

  /** @brief Selection rule used by `num_apertures` to pick a subset
   *  from each phase's `Phase::apertures` list.
   *
   * - `"head"` (default): take the first N entries = the N **wettest**
   *   apertures per phase.  Best when the level wants to concentrate
   *   on the wet tail (e.g. cascade L0 uninodal).
   * - `"stride"` / `"interleave"` / `"spread"`: take N entries evenly
   *   spaced across the full ordered list (indices `i × total / N`).
   *   Samples the full wetness spectrum — first pick is still wettest,
   *   last pick is near driest.  Best for representative coverage.
   * - `"tail"` / `"last"`: take the last N entries = the N **driest**
   *   apertures per phase (mirror of `head`).  Useful for value-function
   *   exploration of dry-tail scenarios.
   *
   * Has no effect when `num_apertures` is absent or `≥ len(Phase::apertures)`.
   * Each cascade level can override via its own `sddp_options`.
   */
  OptName aperture_selection_mode {};

  /** @brief Directory for aperture-specific scenario data.
   *
   * When present, scenarios referenced by `Aperture::source_scenario` are
   * first looked up in this directory.  If not found there, they fall back
   * to the regular `input_directory`.  This allows backward-pass apertures
   * to use different affluent data than the forward-pass scenarios.
   */
  OptName aperture_directory {};

  /** @brief Global default **aperture system** for the SDDP backward pass.
   *
   * Path to a Planning JSON whose `.system` (and its
   * `.options.model_options`) replaces the regular forward system when
   * solving apertures in the backward pass.  This is the global tier of
   * the resolution chain: `Phase::aperture_system_file` →
   * `CascadeLevel::aperture_system_file` → this field → regular system.
   *
   * The parent `simulation` is reused unchanged; the reduced system must
   * keep the same reservoir/storage/α inter-phase state elements (matched
   * by `uid`).  Lets the backward pass solve a cheaper, simplified model
   * (aggregated thermals, single-bus, no Kirchhoff) while the forward pass
   * keeps full detail — the per-aperture cuts are averaged anyway, so the
   * forward detail is largely lost after averaging.
   */
  OptName aperture_system_file {};

  /** @brief Timeout in seconds for individual aperture LP solves in the
   *  SDDP backward pass.
   *
   * When an aperture LP exceeds this time, it is treated as infeasible
   * (skipped), a WARNING is logged, and the solver continues with the
   * remaining apertures.  Default 15 seconds.  0 = no timeout.
   */
  OptReal aperture_timeout {};

  /** @brief Save LP files for infeasible apertures to the log directory.
   *
   * When true, each infeasible aperture clone is written as
   * ``error_aperture_sc_<scene>_ph_<phase>_ap_<uid>.lp`` in the log
   * directory.  Useful for debugging but expensive in large cases.
   * Default: false (disabled).
   */
  OptBool save_aperture_lp {};

  /** @brief Comma-separated list of SDDP passes whose LP-debug dump
   *  is active.  Honoured only when `PlanningOptions::lp_debug=true`
   *  (the umbrella toggle).  Empty / unset defaults to
   *  `"forward,aperture"` (legacy behaviour); set to
   *  `"forward,backward,aperture"` (or simply `"all"`) to add the
   *  backward-pass tgt LP dump used during the off↔compress
   *  symmetry investigation.
   *
   *  Each pass writes via the shared `LpDebugWriter` to
   *  `log_directory` and respects the
   *  `lp_debug_scene_min/max` + `lp_debug_phase_min/max` filter
   *  window.  Filenames follow `sddp_file::debug_lp_fmt` (forward),
   *  `debug_backward_lp_fmt` (backward), and `debug_aperture_lp_fmt`
   *  (aperture).
   *
   *  Recognised tokens (case-insensitive, comma-separated):
   *  `forward`, `backward`, `aperture`, `all`.  Unknown tokens emit
   *  a one-time warning and are skipped.
   *
   *  Replaces the old `--lp-dump-backward <dir>` CLI flag and
   *  `GTOPT_DUMP_BACKWARD_LP=<dir>` env var (both still honoured as
   *  translation shims that set `lp_debug=true`,
   *  `lp_debug_passes=backward`, and `log_directory=<dir>`).
   */
  OptName lp_debug_passes {};

  /** @brief Use the manual (load_flat) clone route for aperture clones
   *  instead of the backend's native `clone()` (`CPXcloneprob`).
   *
   * When true, `solve_apertures_for_phase` builds each aperture clone
   * by replaying the source phase LP's stored `FlatLinearProblem`
   * snapshot through `load_flat()` on a fresh `LinearInterface`.  The
   * manual route uses only env-local solver calls and does NOT
   * acquire `s_global_clone_mutex`, so 80 aperture clones can be
   * built simultaneously instead of one-at-a-time under the lock.
   *
   * When false (default), the legacy native route is used —
   * `phase_li.clone(CloneKind::shallow)` calls the backend's
   * `clone()` (e.g. `CPXcloneprob`) under `s_global_clone_mutex`.
   * That path is structurally serialised because the previous
   * unguarded design crashed three CPLEX threads at the same
   * instruction pointer (commit `1d7a05c1`).
   *
   * Pre-condition for `true`: the source phase LP must hold an
   * uncompressed `FlatLinearProblem` snapshot — i.e.
   * `low_memory_mode = compress` (or `snapshot`).  The aperture
   * pass already decompresses the source via
   * `DecompressionGuard` (sddp_aperture_pass.cpp:390, 579), so
   * this is satisfied for typical SDDP runs.  When the
   * pre-condition is not met, the call site falls back to the
   * native route with a one-time WARN log.
   *
   * Default: false (preserve legacy behaviour).
   */
  OptBool aperture_use_manual_clone {};

  /** @brief Drop feasibility cuts from aperture clone replay.
   *  When true, feasibility cut rows (fcut) are filtered out during
   *  clone-from-flat replay so they don't conflict with perturbed
   *  trial states.  Default: true. */
  OptBool aperture_drop_fcuts {true};

  /** @brief Number of apertures solved serially per backward-pass task.
   *
   * Controls the chunked aperture pass.  Each chunk is submitted as
   * **one** SDDP work-pool task that clones the phase LP **once**
   * and solves its inner apertures sequentially.  By default each
   * inner solve is an independent cold barrier (the shared clone only
   * amortizes the LP reconstruction); set `aperture_solve_mode = warm`
   * to additionally warm-start every aperture after the first off the
   * previous one's resident basis (a few simplex pivots instead of a
   * fresh barrier).
   *
   * Pairs with the per-phase aperture wetness sort applied by
   * `plp2gtopt` (driest → wettest within each phase): consecutive
   * apertures in a chunk have similar bounds, maximising warm-
   * start reuse.
   *
   * Sentinel encoding (resolved at SDDPMethod setup):
   *
   *   * absent / `nullopt` / `0` → **auto** — currently resolves to
   *     `1` (the legacy 1-task-per-aperture path).  Measured on the
   *     juan/IPLP 16-scene × 16-aperture × 51-phase case under the
   *     parallel-safe manual-clone route, K=1 is consistently fastest
   *     because per-aperture solves are small (≪ 100 ms) and the
   *     work-pool fan-out wins over chunked warm-start reuse.  The
   *     `compute_auto_aperture_chunk_size` formula is retained for
   *     future workloads where clone setup dominates per-aperture
   *     solve cost.
   *   * `1`  → legacy behaviour: one task per aperture, one clone
   *     per task, no inner serial loop.
   *   * `K > 1` → use exactly `K` apertures per task.
   *   * `-1` → cap at `A_max` per phase: a single task per scene that
   *     iterates every aperture in series.  Useful for licence-
   *     constrained or memory-tight runs.
   *
   * Default: `nullopt` (auto = 1).
   */
  OptInt aperture_chunk_size {};

  /** @brief How each backward-pass aperture is solved and how its cut's
   *         coefficients are recovered (`cold` / `warm` / `reduced_cost` /
   *         `dual_shared` / `screened`).
   *
   * See `ApertureSolveMode` for the full per-mode rationale.  Summary:
   *
   * - `cold`: independent cold barrier + crossover per aperture; cut
   *   coefficients are the exact vertex reduced costs.  Byte-for-byte the
   *   legacy behaviour.
   * - `warm` (**effective default** — an unset option resolves to `warm`
   *   in `PlanningOptionsLP::sddp_aperture_solve_mode()`, and plp2gtopt
   *   emits it explicitly): only meaningful with `aperture_chunk_size > 1`;
   *   the first aperture in a chunk seeds a basis (barrier + crossover)
   *   and every subsequent aperture re-optimizes it with a warm simplex
   *   solve (via `SolverOptions::advanced_basis` → CPLEX `ADVIND=1`)
   *   instead of a fresh cold barrier.  Aperture deltas are bound-only so
   *   the basis stays valid across apertures.
   * - `reduced_cost`: cold barrier WITHOUT crossover per aperture; the
   *   cut coefficients come straight from the interior-point reduced
   *   costs, filtered by `cut_coeff_eps`.  Skips crossover on big LPs
   *   (~35% faster per aperture than `cold`).
   * - `dual_shared` (opt-in): solve only the highest-weight
   *   representative aperture; synthesize every other aperture's cut from
   *   the representative's vertex duals via the Infanger–Morton
   *   bound-delta correction (Lemma AP2,
   *   `docs/formulation/sddp-cut-validity.md` §6).  Skips all
   *   non-representative re-solves — the remaining lever on backends
   *   without any warm-start path (e.g. cuOpt/PDLP; ledger F10).
   * - `screened` (opt-in): `dual_shared`, then the `aperture_screen_count`
   *   synthesized cuts with the largest |intercept correction| are
   *   re-solved exactly on the resident basis.
   *
   * Absent / `nullopt` resolves to `warm`.
   */
  std::optional<ApertureSolveMode> aperture_solve_mode {};

  /** @brief Number of dual-shared aperture cuts re-solved exactly under
   *         `aperture_solve_mode = screened`.
   *
   * After the representative solve and the dual-shared synthesis pass,
   * the `screened` mode picks the N synthesized cuts with the largest
   * absolute intercept correction |Σ max(d,0)Δl + Σ min(d,0)Δu| — the
   * apertures whose shared support is loosest — and re-solves them
   * exactly (warm dual simplex on the resident basis), replacing their
   * synthesized cuts with exact ones.  Ignored by every other mode.
   *
   * Default: `nullopt` → 2.
   */
  OptInt aperture_screen_count {};

  /** @brief Seed each iteration's first backward aperture from the previous
   *         iteration's first-aperture simplex basis (dual warm start).
   *
   * Orthogonal to `aperture_solve_mode`; only acts when the mode is `cold`
   * or `warm` (vertex cuts).  One basis per `(scene, phase)` cell is kept
   * across iterations (PhaseStateInfo) — `O(cells)` memory, not
   * `O(cells × apertures)`.  Same realization each iteration (the first
   * aperture), so the only delta is the incoming state (bound-only) plus
   * appended cut rows; the captured basis is an exact dual-simplex seed and
   * the gap shrinks as the policy converges.  The within-chunk resident
   * basis carries the remaining apertures (`aperture_chunk_size > 1`).
   *
   * Default: `nullopt` → `false` (every first aperture solves cold, the
   * legacy behaviour).
   */
  OptBool aperture_seed_basis {};

  /** @brief Cross-pass simplex-basis warm-start reuse between the forward and
   *         backward SDDP passes.
   *
   * Generalizes `aperture_seed_basis`: in addition to the within-backward
   * resident-basis chain, the forward-pass solve's basis can seed the backward
   * dual/aperture solve of the same `(scene, phase)` and vice versa (a
   * bidirectional cross cache in `PhaseStateInfo`).  A cross basis is only a
   * warm start — it cannot change the optimum or corrupt a cut — so every mode
   * is correctness-neutral; it silently degrades to a cold solve on backends
   * without basis save/restore (only CPLEX and HiGHS support it) and is gated
   * off where the row structure is not tail-append compatible
   * (`aperture_drop_fcuts`, `aperture_system_file`).
   *
   * See `BasisCrossMode`.  Default: `nullopt` → `off` (legacy behaviour).
   */
  std::optional<BasisCrossMode> basis_cross_mode {};

  /** @brief CSV file with boundary (future-cost) cuts for the last phase.
   *
   * These are analogous to PLP's "planos de embalse" — external optimality
   * cuts that approximate the expected future cost beyond the planning
   * horizon.  Each cut is of the form:
   *
   *   α ≥ rhs + Σ_i  coeff_i · state_var_i
   *
   * The CSV header row names the state variables (reservoir / battery);
   * subsequent rows provide the cut name, iteration, scene UID,
   * RHS, and gradient coefficients.
   *
   * Format:
   *
   * ```text
   * name,iteration,scene,rhs,Reservoir1,Reservoir2,...
   * cut_001,1,1,-5000.0,0.25,0.75,...
   * ```
   *
   * The `scene` column contains the scene UID (matching the `uid` field
   * in gtopt's `scene_array`).  The solver maps column headers to the LP
   * state-variable columns in the last phase and adds each cut as a
   * lower-bound constraint on the future cost variable α.
   * If empty, no boundary cuts are loaded.
   */
  OptName boundary_cuts_file {};

  /** @brief How boundary cuts are loaded: noload, separated (default),
   * or combined.
   *
   * - noload — do not load boundary cuts even if a file is given.
   * - separated — load cuts per scene (scene UID matching; default).
   * - combined — load all cuts into all scenes (broadcast).
   */
  std::optional<BoundaryCutsMode> boundary_cuts_mode {};

  /** @brief How terminal/boundary cuts are shared across scenes on the
   * terminal α — the terminal-phase analogue of `cut_sharing_mode`.
   * - per_scene (default): each scene's terminal α bounded only by its own
   *   scenario's boundary cuts (valid; pairs with cut_sharing none/multicut).
   * - shared: broadcast each boundary cut onto every scene's single terminal α
   *   (valid only when the post-horizon value is scenario-identical).
   * - multicut: N terminal α columns, cut s → varphi_s, priced at the M4
   *   weight w_r = p_s (= 1/N under uniform probabilities; pairs with
   *   cut_sharing_mode=multicut).
   * When unset, derived from the legacy `boundary_cuts_mode` scope:
   * separated→per_scene, combined→shared; else per_scene.
   */
  std::optional<BoundaryCutSharingMode> boundary_cut_sharing_mode {};

  /** @brief Maximum number of SDDP iterations to load from the boundary
   * cuts file.  Only cuts from the last N iterations (by `iteration`
   * column, i.e. PLP's IPDNumIte) are loaded.  0 = load all (default).
   */
  OptInt boundary_max_iterations {};

  /** @brief How to handle cuts referencing state variables not in the model.
   *
   * - `skip_coeff` (default): drop the missing coefficient, load the cut.
   * - `skip_cut`: skip the entire cut if any missing variable has a
   *   non-zero coefficient.
   */
  std::optional<MissingCutVarMode> missing_cut_var_mode {};

  /** @brief Apply an α-rebase (mean-shift) to boundary cuts on load.
   *
   * When enabled, for each scene the per-cut RHS is shifted so the
   * scene's cut intercepts sum to zero, and the offset is carried
   * via `LinearInterface::add_obj_constant` so `get_obj_value()`
   * remains algebraically identical to the unshifted formulation.
   *
   * The rewrite is mathematically exact: α' = α − c̄ where c̄ is the
   * per-scene mean of installed cut RHSs (post-`bc_discount`).
   * Cuts on disk round-trip cleanly (the shift is recomputed each
   * load) and all `get_obj_value()` readers (SDDP bounds, cut
   * intercepts, forward-pass costs) see the same physical value
   * whether the shift is on or off.
   *
   * Default: false — pre-existing tests assert against the on-disk
   * RHS magnitudes, so the shift is opt-in for measurement /
   * conditioning experiments.  Enable to centre boundary cut RHSs
   * around 0 (helps LP equilibration on cases where boundary cut
   * intercepts dominate runtime cut intercepts by orders of
   * magnitude).
   */
  OptBool boundary_cuts_mean_shift {};

  // ``named_cuts_file`` (the CSV-based "named hot-start cuts" option)
  // was retired in 2026-05.  Internal hot-start cuts now travel via
  // ``cuts_input_file`` (Parquet) only — the typed schema is faster
  // to parse, lossless on float coefficients, and carries scene /
  // phase / iteration / extra directly without a name column.

  /// Maximum retained cuts per (scene, phase) LP.  0 = unlimited (default).
  OptInt max_cuts_per_phase {};
  /// Iterations between cut pruning passes.  Default: 10.
  OptInt cut_prune_interval {};
  /// Dual threshold for inactive cut detection.  Default: 1e-8.
  OptReal prune_dual_threshold {};

  /// Use single cut storage: store in per-scene vectors only.  Default: false.
  OptBool single_cut_storage {};
  /// Maximum total stored cuts per scene (0 = unlimited).  Default: 0.
  OptInt max_stored_cuts {};

  /// Run in simulation mode: no training iterations (max_iterations=0),
  /// forward-only evaluation of the policy from loaded cuts.
  /// No cuts are saved.  Default: false.
  OptBool simulation_mode {};

  /// Low memory mode: off or compress (default for SDDP/cascade).
  /// `compress` releases the solver backend after each per-(scene, phase)
  /// solve and keeps only the flat LP snapshot (optionally compressed via
  /// `memory_codec`), trading CPU time (reconstruction + decompression on the
  /// next touch) for significant memory savings on large problems.
  ///
  /// (The former `rebuild` mode was removed 2026-05-13; the JSON aliases
  /// `"rebuild"` and `"snapshot"` now both parse to `compress`.)
  ///
  /// When unset, `PlanningOptionsLP::sddp_low_memory()` resolves to
  /// `compress` for SDDP/cascade methods (the historical default was
  /// `off`).  Pass `--memory-saving off` to restore the eager-resident
  /// behaviour.
  std::optional<LowMemoryMode> low_memory_mode {};

  /// In-memory compression codec for low_memory level 2.
  /// Selects the algorithm used to compress the saved FlatLinearProblem.
  /// Default: auto (picks best available: lz4 > snappy > zstd > gzip).
  /// Accepted values: "auto", "none", "lz4", "snappy", "zstd", "gzip".
  std::optional<CompressionCodec> memory_codec {};

  /** @brief Absolute tolerance for filtering numerically tiny Benders cut
   * coefficients.
   *
   * When constructing an optimality cut, any state-variable coefficient
   * (reduced cost or row dual) whose absolute value is below this threshold
   * is dropped — both the coefficient and its corresponding RHS adjustment
   * are skipped.  This removes solver numerical noise that would otherwise
   * produce ill-conditioned cuts.
   *
   * Inspired by PLP's OptiEPS mechanism (default 1e-8 there).
   *
   * Default: 0.0 (no filtering — all coefficients are kept).
   * Typical useful values: 1e-12 to 1e-8.
   */
  OptReal cut_coeff_eps {};

  /** @brief How update_lp elements obtain reservoir/battery volume between
   * phases.
   *
   * Controls the fallback in StorageLP::physical_eini for nonlinear LP
   * coefficient updates (seepage, production factor, discharge limit).
   * Does NOT affect SDDP state-variable chaining or cut generation.
   *
   * - `warm_start` (default): volume from warm-start solution, recovered
   *   state file, or vini.  No cross-phase lookup.
   * - `cross_phase`: volume from the previous phase's efin within the
   *   same forward pass.
   */
  std::optional<StateVariableLookupMode> state_variable_lookup_mode {};

  // ── Convergence criteria
  // ────────────────────────────────────────────────────

  /** @brief Convergence criterion mode selection.
   *
   * - `gap_only`:        deterministic gap test only.
   * - `gap_stationary`:  gap + stationary gap detection.
   * - `statistical`:     gap + stationary + CI (default, PLP-style).
   *
   * The `statistical` mode degrades gracefully to `gap_stationary` when
   * only one scene is present (no apertures / pure Benders).
   */
  std::optional<ConvergenceMode> convergence_mode {};

  /** @brief Tolerance for stationary-gap convergence criterion.
   *
   * When the relative change in the convergence gap over the last
   * `stationary_window` iterations falls below this value, the gap is
   * considered stationary (no longer improving).  This triggers
   * convergence in two situations:
   *
   *  1. Standalone: gap is stationary → declare convergence even if
   *     gap > convergence_tol (non-zero gap accepted).
   *  2. Combined with convergence_confidence: when the CI test fails
   *     (gap > z*σ) but the gap is stationary → declare convergence
   *     (the non-zero gap has stabilised and further iterations won't
   *     help).
   *
   * Formula (after min_iterations and stationary_window completed):
   *   gap_change = |gap[i] − gap[i − window]| / max(1e-10, gap[i − window])
   *   if gap_change < stationary_tol → gap is stationary
   *
   * Default: 0.01 (1%).  Set to 0.0 to disable.
   */
  OptReal stationary_tol {};

  /** @brief Number of iterations to look back when checking for a stationary
   * gap.  Used by both the standalone stationary criterion and the
   * statistical+stationary criterion.  Default: 10.
   */
  OptInt stationary_window {};

  /** @brief Confidence level for statistical convergence criterion (0-1).
   *
   * When > 0 and multiple scenes exist, convergence is checked via
   * PLP-style confidence interval: UB - LB <= z_{α/2} * σ.
   * Combined with stationary_tol, also handles the non-zero-gap case
   * where the gap stabilises above the CI threshold.
   *
   * Default: 0.95 (95% CI).  Set to 0.0 to disable. */
  OptReal convergence_confidence {};

  /** @brief Absolute gap ceiling for the secondary convergence tests.
   *
   * Hard guard against premature convergence: when ``gap >=
   * stationary_gap_ceiling`` (relative gap), neither the stationary nor
   * the statistical CI test can fire — only the primary
   * ``convergence_tol`` test is allowed to declare convergence.
   *
   * Defends against heterogeneous-scene σ explosion (juan run
   * 2026-05-02 declared CI convergence at iter 2 with gap = 25 %
   * because σ ≈ 77 M dominated the 38 M absolute gap).  Tightening
   * this option to e.g. 0.05 forces the solver to keep iterating
   * until the gap closes below 5 %.
   *
   * Default: 0.5 (50 %).  Set to 1.0 to disable the ceiling. */
  OptReal stationary_gap_ceiling {};

  /** @brief Consecutive structural failures before terminal-skip kicks in.
   *
   * After this many iterations of "elastic filter produced no
   * feasibility cut" failures at the same scene, mark it terminal:
   * skip its forward pass each iter until *new* cuts arrive
   * globally.  Default: 2.  Set to 0 to disable. */
  OptInt terminal_failure_threshold {};

  // ── LP solver options (per-pass override)
  // ───────────────────────────────────

  /** @brief Maximum algorithm fallback attempts for forward-pass solves.
   *
   *  Controls how many alternative algorithms the solver tries when a
   *  forward-pass LP returns non-optimal.  Default: 2 (full cycle).
   */
  OptInt forward_max_fallbacks {};

  /** @brief Scene-level fail-stop forward pass (default: true).
   *
   *  When true (the new default), an infeasible phase that produces a
   *  feasibility cut on its predecessor causes the scene's forward pass
   *  to **stop immediately for the current iteration**: the cut is
   *  installed on phase p-1, the scene is marked failed-this-iter, and
   *  control returns to the caller.  The next iteration starts fresh
   *  from p1 with the newly accumulated cuts (preserved in the global
   *  cut store).
   *
   *  When false, the legacy PLP-style backtracking forward pass is
   *  restored: after installing the fcut on p-1, `phase_idx` is
   *  decremented and p-1 is re-solved under the new cut.  If p-1 is
   *  still infeasible, a fresh fcut is installed on p-2 and the
   *  cascade continues — bounded by `forward_max_attempts`.  Kept
   *  available for regression tests and academic fixtures that depend
   *  on the cascade dynamics.
   */
  OptBool forward_fail_stop {};

  /** @brief Per-scene rollback on forward-pass infeasibility (default:
   *  false).
   *
   *  When `true`, a scene declared infeasible in the forward pass has
   *  every cut it has accumulated in the global cut store deleted from
   *  its LP cells and from `m_cut_store_.scene_cuts()[scene]` — both
   *  forward-pass feasibility cuts and earlier backward-pass
   *  optimality cuts go.  The bad trajectory that produced any of
   *  them is no longer trusted, and the scene starts fresh next
   *  iteration with whatever cuts arrive from peers via cut sharing
   *  or their own backward-pass.
   *
   *  Stall guard: at iteration k+1's forward dispatch, a scene that
   *  failed at iteration k retries iff the global stored-cut count
   *  has grown since the failure.  If every previously-failed scene
   *  is "stalled" (no new cuts arrived), the run aborts with a
   *  `SolverError`/`no recovery path` error to avoid an infinite loop
   *  on degenerate single-scene runs or all-scenes-failed iterations.
   *
   *  When `false` (default): cuts persist across iterations even when
   *  the scene that produced them was declared infeasible — legacy
   *  behaviour, preserved as the safe default until the rollback
   *  feature has soaked.
   */
  OptBool forward_infeas_rollback {};

  /** @brief Re-solve target phase t LP before extracting cut data
   *  (default: true).
   *
   *  When ``true`` (default), the backward pass at phase t re-solves
   *  LP_t at the forward-pass trial state v̂_{t-1} **before** building
   *  the cut for α_{t-1}.  Cuts on α_t added earlier in the same
   *  backward pass — when this loop processed phase t+1 — are now
   *  reflected in z_t and the reduced costs, producing a textbook
   *  one-iter-tight Benders cut on α_{t-1}.  The aperture-success
   *  path picks up the same in-pass cuts automatically because each
   *  aperture clone is forked from LP_t (with cut replay under both
   *  native ``CPXcloneprob`` and manual ``clone_from_flat`` routes).
   *
   *  When ``false``, cuts use the forward-pass cached
   *  ``forward_full_obj_physical`` and StateVariable-mirrored reduced
   *  costs.  Cheaper per iteration, but a fresh cut at phase T takes
   *  ≈ T iterations to fully ripple to phase 1.
   *
   *  Cost: one extra LP resolve per (scene, phase) per backward pass.
   *  On juan/iplp scale (50 phases × 7 scenes ≈ 350 extra resolves
   *  per iter, ≈75 ms each) this is +26 s per iter — typically
   *  recovered by needing 5-10× fewer iterations to converge. */
  OptBool backward_resolve_target {};

  /** @brief Maximum algorithm fallback attempts for backward-pass and
   *  aperture solves.
   *
   *  Controls how many alternative algorithms the solver tries when a
   *  backward-pass or aperture LP returns non-optimal.  Default: 0
   *  (no fallback — fail immediately).
   */
  OptInt backward_max_fallbacks {};

  /** @brief SDDP work pool CPU over-commit factor.
   *  Multiplied by hardware_concurrency to set max pool threads.
   *  Default 4.0 — extra threads keep CPUs busy while others block on
   *  the clone mutex. */
  OptReal pool_cpu_factor {};

  /** @brief Process memory limit in MB for the SDDP work pool.
   *  When non-zero, the pool blocks task dispatch if process RSS exceeds
   *  this value.  Accepts values parsed by parse_memory_size (e.g. 5G).
   *  0 = no limit (default). */
  OptReal pool_memory_limit_mb {};

  /** @brief Maximum iteration spread between fastest and slowest scene
   *  when cut_sharing is none and multiple scenes exist.
   *
   * When > 0, the solver runs scenes asynchronously: each scene
   * progresses through its own forward/backward iteration loop.  The
   * pool's SDDPTaskKey priority naturally schedules slower scenes first
   * (lower iteration -> higher priority), self-regulating the spread.
   *
   * 0 = synchronous (current behavior, default).
   */
  OptInt max_async_spread {};

  /** @brief How to drain in-flight cuts after the aggregate-convergence
   *  stop signal fires in `SDDPMethod::solve_async`.
   *
   *  See `CutDrainMode` for the full rationale.  Summary:
   *   - `count`     — per-scene count snapshot; legacy asymmetric.
   *   - `iteration` — filter by `cut.iteration_index <= certified`;
   *                   symmetric, deterministic.  Default.
   *   - `all`       — keep every cut; maximises retention, gives up
   *                   run-to-run determinism.
   */
  std::optional<CutDrainMode> cut_drain_mode {};

  /** @brief Optional LP solver configuration for SDDP forward pass.
   *
   * When set, these options are merged with the global
   * `PlanningOptions::solver_options`.  Forward-pass-specific options
   * take precedence over the global ones.
   *
   * Typical use: use barrier for the forward pass (fresh solves) while
   * using dual simplex for the backward pass (warm-started resolves).
   */
  std::optional<SolverOptions> forward_solver_options {};

  /** @brief Optional LP solver configuration for SDDP backward pass.
   *
   * When set, these options are merged with the global
   * `PlanningOptions::solver_options`.  Backward-pass-specific options
   * take precedence over the global ones.
   *
   * Typical use: use dual simplex for the backward pass.
   */
  std::optional<SolverOptions> backward_solver_options {};

  void merge(SddpOptions&& opts)
  {
    merge_opt(cut_sharing_mode, opts.cut_sharing_mode);
    merge_opt(forward_sampling_mode, opts.forward_sampling_mode);
    merge_opt(markov_states, std::move(opts.markov_states));
    merge_opt(markov_transition, std::move(opts.markov_transition));
    merge_opt(cut_drain_mode, opts.cut_drain_mode);
    merge_opt(cut_directory, std::move(opts.cut_directory));
    merge_opt(api_enabled, opts.api_enabled);
    merge_opt(update_lp_skip, opts.update_lp_skip);
    merge_opt(max_iterations, opts.max_iterations);
    merge_opt(min_iterations, opts.min_iterations);
    merge_opt(convergence_tol, opts.convergence_tol);
    merge_opt(elastic_penalty, opts.elastic_penalty);
    merge_opt(scale_alpha, opts.scale_alpha);
    merge_opt(cut_recovery_mode, opts.cut_recovery_mode);
    merge_opt(recovery_mode, opts.recovery_mode);
    merge_opt(save_per_iteration, opts.save_per_iteration);
    merge_opt(cuts_input_file, std::move(opts.cuts_input_file));
    merge_opt(sentinel_file, std::move(opts.sentinel_file));
    merge_opt(elastic_mode, opts.elastic_mode);
    merge_opt(multi_cut_threshold, opts.multi_cut_threshold);
    merge_opt(apertures, std::move(opts.apertures));
    merge_opt(num_apertures, opts.num_apertures);
    merge_opt(aperture_selection_mode, std::move(opts.aperture_selection_mode));
    merge_opt(aperture_directory, std::move(opts.aperture_directory));
    merge_opt(aperture_system_file, std::move(opts.aperture_system_file));
    merge_opt(aperture_timeout, opts.aperture_timeout);
    merge_opt(save_aperture_lp, opts.save_aperture_lp);
    merge_opt(aperture_use_manual_clone, opts.aperture_use_manual_clone);
    merge_opt(aperture_drop_fcuts, opts.aperture_drop_fcuts);
    merge_opt(aperture_chunk_size, opts.aperture_chunk_size);
    merge_opt(aperture_solve_mode, opts.aperture_solve_mode);
    merge_opt(aperture_screen_count, opts.aperture_screen_count);
    merge_opt(aperture_seed_basis, opts.aperture_seed_basis);
    merge_opt(basis_cross_mode, opts.basis_cross_mode);
    merge_opt(boundary_cuts_file, std::move(opts.boundary_cuts_file));
    merge_opt(boundary_cuts_mode, opts.boundary_cuts_mode);
    merge_opt(boundary_cut_sharing_mode, opts.boundary_cut_sharing_mode);
    merge_opt(boundary_max_iterations, opts.boundary_max_iterations);
    merge_opt(boundary_cuts_mean_shift, opts.boundary_cuts_mean_shift);
    merge_opt(missing_cut_var_mode, opts.missing_cut_var_mode);
    merge_opt(max_cuts_per_phase, opts.max_cuts_per_phase);
    merge_opt(cut_prune_interval, opts.cut_prune_interval);
    merge_opt(prune_dual_threshold, opts.prune_dual_threshold);
    merge_opt(single_cut_storage, opts.single_cut_storage);
    merge_opt(max_stored_cuts, opts.max_stored_cuts);
    merge_opt(simulation_mode, opts.simulation_mode);
    merge_opt(low_memory_mode, opts.low_memory_mode);
    merge_opt(memory_codec, opts.memory_codec);
    merge_opt(cut_coeff_eps, opts.cut_coeff_eps);
    merge_opt(state_variable_lookup_mode, opts.state_variable_lookup_mode);
    merge_opt(convergence_mode, opts.convergence_mode);
    merge_opt(stationary_tol, opts.stationary_tol);
    merge_opt(stationary_window, opts.stationary_window);
    merge_opt(convergence_confidence, opts.convergence_confidence);
    merge_opt(stationary_gap_ceiling, opts.stationary_gap_ceiling);
    merge_opt(terminal_failure_threshold, opts.terminal_failure_threshold);
    merge_opt(forward_max_fallbacks, opts.forward_max_fallbacks);
    merge_opt(forward_fail_stop, opts.forward_fail_stop);
    merge_opt(forward_infeas_rollback, opts.forward_infeas_rollback);
    merge_opt(backward_resolve_target, opts.backward_resolve_target);
    merge_opt(backward_max_fallbacks, opts.backward_max_fallbacks);
    merge_opt(max_async_spread, opts.max_async_spread);
    merge_opt(pool_cpu_factor, opts.pool_cpu_factor);
    merge_opt(pool_memory_limit_mb, opts.pool_memory_limit_mb);
    if (opts.forward_solver_options.has_value()) {
      if (forward_solver_options.has_value()) {
        forward_solver_options->merge(*opts.forward_solver_options);
      } else {
        forward_solver_options = opts.forward_solver_options;
      }
    }
    if (opts.backward_solver_options.has_value()) {
      if (backward_solver_options.has_value()) {
        backward_solver_options->merge(*opts.backward_solver_options);
      } else {
        backward_solver_options = opts.backward_solver_options;
      }
    }

    auto _ = std::move(opts);
  }
};

}  // namespace gtopt
