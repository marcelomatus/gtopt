/**
 * @file      sddp_aperture.hpp
 * @brief     Aperture backward-pass logic for SDDP solver
 * @date      2026-03-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Extracted from sddp_solver.cpp into standalone free functions following
 * the same pattern as benders_cut.hpp and sddp_cut_io.hpp.  Each function
 * takes explicit parameters instead of accessing class members, making
 * them independently testable and reusable.
 *
 * ## Free functions
 *
 * - `build_effective_apertures()` – deduplicate aperture UIDs with counts
 * - `build_synthetic_apertures()` – create apertures from first N scenarios
 * - `solve_apertures_for_phase()`  – clone LP per aperture, solve, build
 *    the probability-weighted expected Benders cut
 */

#pragma once

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstddef>
#include <functional>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtopt/aperture.hpp>
#include <gtopt/aperture_data_cache.hpp>
#include <gtopt/basis.hpp>
#include <gtopt/benders_cut.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/sddp_common.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/sparse_row.hpp>

namespace gtopt
{

// ─── Aperture element concepts ──────────────────────────────────────────────

/// Value-provider signature: (StageUid, BlockUid) -> std::optional<double>.
/// Returns the aperture value for the given stage/block, or std::nullopt
/// if the value is unavailable (keeps the forward-pass value unchanged).
using ApertureValueFn =
    std::function<std::optional<double>(StageUid, BlockUid)>;

/// An element that can update its LP for an aperture scenario using a
/// generic value provider.
template<typename T>
concept HasUpdateAperture = requires(const T& e,
                                     LinearInterface& li,
                                     const ScenarioLP& base,
                                     const ApertureValueFn& value_fn,
                                     const StageLP& stage) {
  { e.update_aperture(li, base, value_fn, stage) } -> std::same_as<bool>;
};

// ─── Effective aperture entry ───────────────────────────────────────────────

/// A deduplicated aperture reference with a repetition count.
///
/// When the per-phase apertures contains duplicates (e.g. [1,2,3,3,3]),
/// each unique aperture is solved only once but its weight is scaled by
/// @p count (the number of occurrences).
struct ApertureEntry
{
  std::reference_wrapper<const Aperture> aperture;
  int count {};
};

// ─── Effective aperture list builder ────────────────────────────────────────

/// Build the effective (deduplicated) aperture list for a single phase.
///
/// When @p phase_apertures is empty, all active apertures from
/// @p aperture_defs are used (each with count = 1).
/// When @p phase_apertures is non-empty, UIDs are counted for duplicates
/// and mapped to their definitions; order of first appearance is preserved
/// for deterministic results.
///
/// @param aperture_defs   All aperture definitions from the simulation
/// @param phase_apertures Per-phase aperture UID set (may be empty)
/// @return Deduplicated aperture entries with repetition counts
[[nodiscard]] auto build_effective_apertures(
    std::span<const Aperture> aperture_defs,
    std::span<const Uid> phase_apertures) -> std::vector<ApertureEntry>;

// ─── Per-phase aperture sub-selection ───────────────────────────────────────

/// Select a subset of a per-phase aperture-UID list according to
/// `SDDPOptions::num_apertures` and `aperture_selection_mode`.
///
/// `Phase::apertures` is emitted by `plp2gtopt` sorted wettest → driest;
/// this helper produces the effective per-phase list the backward pass
/// will actually solve for.
///
/// Semantics:
///   * `num_apertures = nullopt`        → return a copy of the full input.
///   * `num_apertures = 0`              → return an empty vector.
///   * `num_apertures = N ≥ size()`     → return a copy of the full input.
///   * `num_apertures = N > 0` with:
///       - `mode = head`   → `phase_apertures.first(N)` (N wettest).
///       - `mode = stride` → `N` entries at indices `i*size/N`,
///                            `i = 0..N-1` (evenly spaced across
///                            the full ordered list — wettest is
///                            still index 0, last pick is near driest).
///       - `mode = tail`   → `phase_apertures.last(N)` (N driest);
///                            mirror of `head`.
///
/// @param phase_apertures Per-phase aperture UIDs (assumed wettest-first).
/// @param num_apertures   `SDDPOptions::num_apertures` value to apply.
/// @param mode            Selection rule (head | stride; default head).
/// @return Owned vector of selected UIDs (preserves input order in `head`
///         mode; stride mode walks indices in ascending order so
///         wetness order is preserved within the picks).
[[nodiscard]] auto select_apertures(
    std::span<const Uid> phase_apertures,
    const std::optional<int>& num_apertures,
    ApertureSelectionMode mode = ApertureSelectionMode::head)
    -> std::vector<Uid>;

// ─── Aperture partitioning (chunked backward pass) ──────────────────────────

/// Partition a span of @p ApertureEntry into contiguous chunks of size
/// @p chunk_size (the last chunk may be shorter).
///
/// Each returned span aliases the input — the caller must keep the
/// backing storage alive for the lifetime of the returned vector.
///
/// Used by the chunked aperture backward pass: every chunk becomes a
/// single SDDP work-pool task that clones the phase LP once and solves
/// its inner apertures serially with warm-start reuse on the shared
/// clone.  Order is preserved end-to-end so the wetness sort applied
/// in `plp2gtopt` (wettest → driest within a phase) keeps similar
/// bounds adjacent within each chunk.  Pairs with
/// `SddpOptions::num_apertures = N` which truncates each phase's list
/// to its first N (= wettest N) entries before chunking.
///
/// Edge cases:
///   * `apertures` empty                → empty vector.
///   * `chunk_size <= 0`                → one chunk per aperture (K=1).
///   * `chunk_size >= apertures.size()` → exactly one chunk.
///
/// @param apertures   Effective (deduplicated) aperture list for the phase.
/// @param chunk_size  Apertures per task (`<=0` collapses to 1).
/// @return Vector of non-overlapping spans whose union covers @p apertures.
[[nodiscard]] auto partition_apertures(std::span<const ApertureEntry> apertures,
                                       int chunk_size)
    -> std::vector<std::span<const ApertureEntry>>;

// ─── Synthetic aperture builder ─────────────────────────────────────────────

/// Build synthetic aperture definitions from the first N scenarios.
///
/// Creates one aperture per scenario with equal probability (1/N).
/// Used when no explicit aperture_array is provided in the simulation
/// and the solver falls back to the legacy num_apertures-based behaviour.
///
/// @param all_scenarios   All scenario LP objects
/// @param n_apertures     Number of apertures to create (capped at
///                        all_scenarios.size())
/// @return Array of synthetic Aperture objects
[[nodiscard]] auto build_synthetic_apertures(
    std::span<const ScenarioLP> all_scenarios, std::ptrdiff_t n_apertures)
    -> Array<Aperture>;

// ─── Auto-tuned aperture chunk size ────────────────────────────────────────

/// @note As of commit 8e08b1c4, `SDDPMethod::initialize_solver` resolves
///       the `auto` sentinel (requested == 0) directly to K=1 (the
///       empirically-fastest setting on juan/IPLP), bypassing this
///       formula entirely.  The function is RETAINED — and exercised
///       by the six boundary-behaviour unit tests in
///       `test_sddp_aperture_functions.cpp` — as a documented helper
///       for future workloads where the chunked path may win again,
///       e.g. very large per-aperture LPs (≫ 100 ms each) where the
///       deflat+replay amortisation and dual-basis warm-start savings
///       outweigh the loss of work-pool fan-out.
///
/// Compute a default `aperture_chunk_size` from problem dimensions.
///
/// Sizes the per-phase aperture chunks so that the resulting task count
/// (`N_scenes × ⌈A/K⌉`) roughly equals the work pool's target
/// concurrency (`parallel_factor × N_cores`).  The intuition:
///
///   * If `N_scenes × A` ≤ `parallel_factor × N_cores`, the pool can
///     run every (scene, aperture) task concurrently — no point in
///     chunking, return 1.
///
///   * If `N_scenes × A` ≫ `parallel_factor × N_cores`, the pool is
///     oversubscribed at K=1.  Chunking K>1 reduces the queue depth
///     (one task per chunk instead of one per aperture) and gives each
///     chunk warm-start reuse on its inner solves — the
///     `update_aperture` deltas between successive apertures (column
///     bounds; equality-row RHS under AR inflows / profile elements)
///     keep the dual basis valid, so the second-and-later resolves
///     typically converge in a fraction of a cold barrier.
///
/// Closed form:
///
///   K_auto = clamp(⌈(N_scenes × A) / (parallel_factor × N_cores)⌉,
///                  1,
///                  A)
///
/// All inputs are clamped to non-negative; degenerate inputs (any of
/// `n_apertures`, `n_active_scenes`, `n_physical_cores` == 0, or
/// `parallel_factor` ≤ 0) collapse to 1 — a safe no-chunking default
/// that callers can detect and fall back to the legacy 1-task-per-
/// aperture path.
///
/// `parallel_factor` defaults to 2.0 to match `make_sddp_work_pool`'s
/// default `cpu_factor = 2.0` (`sddp_pool.hpp`): the pool sizes itself
/// at `2 × N_cores` workers, so the auto-K target tracks the same
/// over-subscription ratio.
///
/// @param max_apertures_per_phase Largest A across phases (use the max,
///        not the average — smaller phases will simply under-chunk,
///        which is harmless; larger phases would otherwise over-chunk
///        and grow the queue).
/// @param n_active_scenes         Number of scenes the SDDP solver
///        will process per iteration (`simulation.scene_count()`
///        minus inactive scenes).
/// @param n_physical_cores        Output of
///        `gtopt::physical_concurrency()` (CPU-quota-aware).
/// @param parallel_factor         Over-subscription factor matching
///        the work pool's `cpu_factor`.  Default 2.0.
/// @return The chunk size to use, in `[1, max_apertures_per_phase]`.
[[nodiscard]] constexpr int compute_auto_aperture_chunk_size(
    int max_apertures_per_phase,
    int n_active_scenes,
    int n_physical_cores,
    double parallel_factor = 2.0) noexcept
{
  if (max_apertures_per_phase <= 0 || n_active_scenes <= 0
      || n_physical_cores <= 0 || !(parallel_factor > 0.0))
  {
    return 1;
  }
  // Floating-point ceil to avoid integer overflow on very large inputs.
  const double total_work = static_cast<double>(max_apertures_per_phase)
      * static_cast<double>(n_active_scenes);
  const double target_tasks =
      parallel_factor * static_cast<double>(n_physical_cores);
  const double k_real = total_work / target_tasks;
  // ceil via integer arithmetic on the cast — k_real ≥ 0, so adding
  // a tiny epsilon would only matter at the integer boundary, where
  // taking the next-higher chunk is the safe choice.
  int k = static_cast<int>(k_real);
  if (static_cast<double>(k) < k_real) {
    ++k;
  }
  // Round DOWN to the *strictly previous* power of two so K lands on
  // the cache- and partition-friendly ladder {1, 2, 4, 8, 16, …}
  // *one step below* the raw formula.  Mapping examples:
  //
  //     raw k:   1  2  3  4  5  6  7  8  …  15  16  17  …  31  32
  //     K_auto:  1  1  2  2  4  4  4  4  …   8   8   8  …  16  16
  //
  // Note 16 → 8 (one step below the exact-pow-2 input), 7 → 4 (the
  // pow-2 below 7).  Rationale: empirically the chunked-aperture path
  // benefits from balanced chunks AND from leaving slack pool capacity
  // for the cross-scene fan-out — biasing one step lower than the
  // exact `bit_floor` keeps the per-chunk queue depth healthy when
  // many scenes contend for the same pool.  On juan/IPLP barrier +
  // split-crossover the strict-previous rounding measured ≤145s
  // (≈ raw `bit_floor` 142s); the LB stability win on cross-phase
  // backward passes (cuts not bunched on a single chunk tail) made
  // the trade worth it.
  const auto u = static_cast<unsigned>(std::max(k, 1));
  k = (u > 1) ? static_cast<int>(std::bit_floor(u - 1U)) : 1;
  return std::min(std::max(k, 1), max_apertures_per_phase);
}

// ─── Dual-shared aperture cut correction (Infanger–Morton) ─────────────────

/// Per-aperture intercept correction for the dual-shared cut synthesis
/// (Lemma AP2, `docs/formulation/sddp-cut-validity.md` §6).
///
/// Aperture LPs of one (scene, phase) share (A, b, c) and differ only in
/// column bounds, so they share one dual feasible set
/// `{y, λ ≥ 0, μ ≥ 0 : Aᵀy + λ − μ = c}` — bound-independent.  Splitting
/// the representative's vertex reduced costs `d = λ − μ` (`λ = d⁺`,
/// `μ = d⁻` by complementarity), weak duality on aperture `a`'s LP gives
///
///   Q_a(x) ≥ z* + corr_a + Σ_i rc_i (x_i − v̂_i),
///   corr_a = Σ_j max(d_j, 0)·(l_j^a − l_j^rep)
///          + Σ_j min(d_j, 0)·(u_j^a − u_j^rep)
///
/// over the changed columns — the representative cut's slope is shared
/// verbatim and only the intercept moves by `corr_a`.
///
/// Units: `d_j` from the representative clone's `get_col_cost()[j]`
/// (physical view, `rc_LP × scale_objective / col_scale`) and bound
/// deltas from `get_col_low()` / `get_col_upp()` (physical,
/// `raw × col_scale`) — the `col_scale` cancels in the product and the
/// `scale_objective` folding matches `z* = get_obj_value()`.
///
/// @param rep_reduced_costs Representative's physical reduced costs
///        (materialized `get_col_cost()` view, post-solve).
/// @param rep_low / rep_upp Representative's physical column bounds at
///        its solve (materialized `get_col_low()` / `get_col_upp()`).
/// @param ap_low / ap_upp   Aperture `a`'s physical column bounds (same
///        views read after `update_aperture` rewrote the clone).
/// @return The correction `corr_a`, or `std::nullopt` when it cannot be
///         computed soundly:
///           * span sizes disagree,
///           * a non-zero reduced cost meets a non-finite or
///             ±sentinel-magnitude (≥ 1e29) bound delta — a bound became
///             (un)bounded between the representative and the aperture.
///         Equal bounds contribute nothing (skipped before any
///         subtraction, so equal infinities are safe); zero reduced
///         costs annihilate their column regardless of the delta.
[[nodiscard]] auto dual_shared_bound_correction(
    std::span<const double> rep_reduced_costs,
    std::span<const double> rep_low,
    std::span<const double> rep_upp,
    std::span<const double> ap_low,
    std::span<const double> ap_upp) noexcept -> std::optional<double>;

// ─── Effective aperture resolution (filter / synthetic / fallback) ──────────

/// Resolve the effective aperture definitions for a single backward-pass
/// step.
///
/// Centralises the four-way decision used by both
/// `backward_pass_with_apertures` (loop) and
/// `backward_pass_with_apertures_single_phase` (single-phase dispatcher):
///
///   1. ``requested_uids`` is empty (``std::nullopt``):
///      - if ``aperture_defs`` is also empty → ``std::nullopt`` (caller
///        falls back to the non-aperture backward path),
///      - otherwise → return a span over ``aperture_defs`` unchanged.
///
///   2. ``requested_uids`` is present:
///      - if the requested list is empty → ``std::nullopt`` (fallback),
///      - if ``aperture_defs`` is non-empty → filter ``aperture_defs``
///        by UID into ``owned`` (warning emitted for any requested UID
///        not found),
///      - if ``aperture_defs`` is empty → build a synthetic aperture
///        list from ``all_scenarios`` (one per scenario, sized to
///        ``min(|requested|, |all_scenarios|)``).
///
///   3. If after filtering / synthesis ``owned`` is still empty →
///      ``std::nullopt`` (fallback).
///
/// The returned span aliases either the original ``aperture_defs``
/// span or the caller-provided ``owned`` storage; the caller must
/// keep both alive for the lifetime of the returned span.
///
/// @param aperture_defs   All aperture definitions from the simulation.
/// @param all_scenarios   All scenario LP objects (used to build a
///                        synthetic aperture list when no aperture_array).
/// @param requested_uids  Per-phase aperture UID set (may be empty
///                        ``std::nullopt`` to mean "use everything").
/// @param owned           Output storage for filtered / synthetic
///                        apertures.  Must be empty on entry.  The
///                        returned span may reference this buffer.
/// @param log_tag         Caller tag for the "requested UID not found"
///                        warning (e.g. "BackwardPass[i3 s1 p7]").
/// @return Span over the effective apertures, or ``std::nullopt`` to
///         tell the caller to fall back to the non-aperture path.
[[nodiscard]] auto resolve_effective_apertures(
    std::span<const Aperture> aperture_defs,
    std::span<const ScenarioLP> all_scenarios,
    const std::optional<Array<Uid>>& requested_uids,
    Array<Aperture>& owned,
    std::string_view log_tag) -> std::optional<std::span<const Aperture>>;

// ─── Aperture task submission ────────────────────────────────────────────────

/// Result of a single aperture's clone + update + solve + cut.
struct ApertureCutResult
{
  ApertureUid ap_uid {};
  double weight {0.0};
  bool feasible {false};
  int status {0};
  std::optional<SparseRow> cut {};
};

/// Result of a chunked task (one task can hold N aperture solves under
/// the chunked aperture pass).  At chunk_size=1 each chunk is a 1-element
/// vector — equivalent to the legacy 1-task-per-aperture path.
using ApertureChunkResult = std::vector<ApertureCutResult>;

/// Callback for submitting a chunk task (1..K aperture solves on a
/// shared LP clone) to the SDDP work pool.
///
/// The task body itself is responsible for cloning the phase LP once,
/// iterating its assigned apertures serially with warm-start reuse on
/// the shared clone, and producing one ``ApertureCutResult`` per
/// aperture (in input order, including memo-hit duplicates).  The
/// caller submits all chunks first, then collects every future,
/// enabling parallel execution across chunks.
using ApertureChunkSubmitFunc = std::function<std::future<ApertureChunkResult>(
    const std::function<ApertureChunkResult()>& task)>;

// ─── Core aperture solver ───────────────────────────────────────────────────

/// Solve all apertures for a single phase and return the expected cut.
///
/// For each effective aperture: clones the phase LP, updates flow column
/// bounds to the aperture's source scenario, solves the clone, and builds
/// a Benders cut from the reduced costs.  The probability-weighted average
/// of all feasible aperture cuts is returned as the expected cut.
///
/// Returns std::nullopt if all apertures are infeasible or skipped.
///
/// @param scene_index      Scene index (for logging/labelling)
/// @param phase_index      Target phase being solved
/// @param src_state        Phase state of the source (previous) phase
/// @param base_scenario    The scene's base scenario (for flow bound update)
/// @param all_scenarios    All simulation scenarios (for aperture lookup)
/// @param aperture_defs    Aperture definitions to use
/// @param phase_apertures  Per-phase aperture UID set (may be empty)
/// @param total_cuts       Running cut count (for label uniqueness)
/// @param sys              SystemLP for the (scene, phase) pair
/// @param phase_lp         PhaseLP for the target phase
/// @param opts             Solver options
/// @param label_maker      Label maker for LP row names
/// @param log_directory    Directory for debug LP files (empty = no save)
/// @param scene_uid_val    Scene UID (for logging)
/// @param phase_uid_val    Phase UID (for logging)
/// @param submit_fn        Callback to submit an aperture task to the work pool
/// @param aperture_timeout Timeout in seconds for each aperture LP solve;
///                         0 = no timeout.  When exceeded, the aperture is
///                         treated as infeasible and skipped.
/// @param save_aperture_lp If true, save each aperture LP to the log directory
/// @param aperture_cache   Cache of pre-built aperture LP data
/// @param iteration        Current SDDP iteration index
/// @param cut_coeff_eps    Epsilon below which cut coefficients are zeroed
/// @param lp_debug_writer  Optional writer.  When non-null, each aperture
///                         clone's LP is dumped to disk (pre-solve) under
///                         the writer's configured directory using
///                         `sddp_file::debug_aperture_lp_fmt`.  Lets the
///                         `lp_debug` option extend to aperture backward-
///                         pass clones — callers are responsible for
///                         applying the `lp_debug_scene/phase_min/max`
///                         filter window and passing nullptr when the
///                         current (scene, phase) is outside it.
/// @param use_manual_clone If true, each aperture clone is built via
///                         `LinearInterface::clone_from_flat()` (replays
///                         the source's `FlatLinearProblem` snapshot through
///                         `load_flat()` on a fresh backend env).  This
///                         skips the backend's native `clone()` and the
///                         `s_global_clone_mutex` — the manual route uses
///                         only env-local solver calls, so 80 aperture
///                         clones can be built in parallel rather than
///                         serialised under the lock.  Pre-condition: the
///                         source phase LP must hold a decompressed
///                         `FlatLinearProblem` snapshot.  When false
///                         (default), the legacy native route is used,
///                         serialising every clone under the global mutex.
/// @param chunk_size       Apertures per submitted task (chunked
///                         backward pass).  >=2 enables the chunked
///                         path: every chunk becomes ONE work-pool task
///                         that clones the phase LP **once** and solves
///                         its inner apertures sequentially with warm-
///                         start reuse on the shared clone.  Bound-only
///                         deltas keep the dual basis valid, so the
///                         second-and-later resolves typically converge
///                         in a fraction of a cold barrier.  A per-chunk
///                         UID memo collapses adjacent duplicate
///                         apertures (the wetness sort applied in
///                         plp2gtopt keeps duplicates adjacent) to a
///                         single solve + N-fold weight.  ``<=1``
///                         falls back to the legacy 1-aperture-per-
///                         task path (each chunk is a 1-element
///                         vector); the call site sees identical
///                         externally-observable behaviour.
[[nodiscard]] auto solve_apertures_for_phase(
    SceneIndex scene_index,
    PhaseIndex phase_index,
    const PhaseStateInfo& src_state,
    ColIndex src_alpha_col,
    const ScenarioLP& base_scenario,
    std::span<const ScenarioLP> all_scenarios,
    std::span<const Aperture> aperture_defs,
    std::span<const Uid> phase_apertures,
    int total_cuts,
    SystemLP& sys,
    const PhaseLP& phase_lp,
    const SolverOptions& opts,
    const LabelMaker& label_maker,
    const std::string& log_directory,
    SceneUid scene_uid_val,
    PhaseUid phase_uid_val,
    const ApertureChunkSubmitFunc& submit_fn,
    double aperture_timeout = 0.0,
    bool save_aperture_lp = false,
    const ApertureDataCache& aperture_cache = {},
    IterationIndex iteration_index = {},
    double cut_coeff_eps = 0.0,
    LpDebugWriter* lp_debug_writer = nullptr,
    bool use_manual_clone = false,
    int chunk_size = 1,
    /// Optional work-pool reference used to grab a `SlotReleaseGuard`
    /// around the chunk-future gather loop.  When non-null AND the
    /// caller is itself running on a worker thread of this pool, the
    /// guard temporarily releases the calling task's slot for the
    /// duration of the blocking wait, so the pool's CPU/thread gate
    /// sees one fewer active task and can dispatch the chunk tasks
    /// the caller is about to wait on.  Without this, the cell-task
    /// block + CPU-gate combo wedges the pool — see the 2026-05-16
    /// concurrency audit and `work_pool.hpp:732-797` for the
    /// rationale.  Defaults to `nullptr` (no slot release) for the
    /// in-tree unit-test callers that drive `solve_apertures_for_phase`
    /// without a live pool.
    class SDDPWorkPool* pool_for_slot_release = nullptr,
    /// Optional override for the state-variable links used to BUILD each
    /// aperture cut.  When non-empty it replaces `src_state.outgoing_links`
    /// in `build_benders_cut_physical`.  The aperture-system path passes
    /// "hybrid" links: `source_col`/`state_var` from the forward source
    /// phase (so the cut installs onto the forward LP) but `dependent_col`
    /// pointing into the aperture clone (so the reduced cost is read from
    /// the matching column).  Empty = use `src_state.outgoing_links` (the
    /// regular single-system path).  Referenced storage must outlive the
    /// call (the caller's vector) — the chunk tasks read it while solving.
    std::span<const StateVarLink> cut_links = {},
    /// Aperture solve / cut-recovery mode.  `cold` (default): cold barrier
    /// + crossover per aperture.  `warm`: first aperture in a chunk seeds a
    /// basis and subsequent apertures warm-start off it with a simplex
    /// re-solve (`SolverOptions::advanced_basis`; chunk_size > 1 only).
    /// `reduced_cost`: cold barrier without crossover, cut from
    /// interior-point reduced costs.  `dual_shared`: solve the
    /// highest-weight representative aperture only and synthesize every
    /// other aperture's cut from its vertex duals (bound-delta intercept
    /// correction, `dual_shared_bound_correction`); forces a single chunk.
    /// `screened`: `dual_shared` + exact re-solve of the top
    /// `aperture_screen_count` synthesized cuts by |correction|.
    /// See `ApertureSolveMode`.
    ApertureSolveMode aperture_solve_mode = ApertureSolveMode::reduced_cost,
    /// Cross-iteration first-aperture warm-start seed (optional).
    ///   - `seed_basis` (in): the previous iteration's first-aperture basis
    ///     for this cell, or `nullptr`/empty to disable seeding.  Each
    ///     chunk's first solve is dual-warm-started from it (reconciled to
    ///     the current dimensions).  Read-only during parallel execution.
    ///   - `captured_basis_out` (out): when non-null, the basis of THIS
    ///     iteration's first aperture (first chunk) is written here after the
    ///     chunk futures join, for the caller to persist into the cell slot.
    /// Honoured for every basis-capable (vertex) mode — all modes except
    /// `reduced_cost`, which has no basis to capture.  Both default off
    /// (legacy cold first solve).
    const Basis* seed_basis = nullptr,
    Basis* captured_basis_out = nullptr,
    /// Number of dual-shared cuts re-solved exactly under
    /// `aperture_solve_mode = screened` (largest |intercept correction|
    /// first).  Ignored by every other mode.  Both production call
    /// sites pass the resolved option; the default is the shared
    /// `default_sddp_aperture_screen_count` constant (sddp_enums.hpp).
    int aperture_screen_count = default_sddp_aperture_screen_count)
    -> std::optional<SparseRow>;

}  // namespace gtopt
