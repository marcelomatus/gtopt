/**
 * @file      sddp_method.hpp
 * @brief     SDDP (Stochastic Dual Dynamic Programming) method for multi-phase
 *            planning problems with state variable coupling
 * @date      2026-03-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements a forward/backward iterative decomposition similar to the PLP
 * SDDP methodology.  Each gtopt-phase corresponds to a PLP-stage and is
 * solved as an independent LP subproblem.  State variables (reservoir
 * volumes, capacity expansion variables, and future irrigation rights)
 * link consecutive phases:
 *
 *   efin[t] → eini[t+1]   (reservoir volume)
 *   capainst[t] → capainst_ini[t+1]  (installed capacity)
 *
 * The solver uses the existing `SimulationLP::state_variables()` map to
 * discover all state-variable linkages generically, without hard-coding
 * any specific component type.
 *
 * **Forward pass** – phases are solved in order; state variable values
 *   propagate from source columns in phase t to dependent columns in
 *   phase t+1.
 *
 * **Backward pass** – starting from the last phase and walking back to
 *   phase 1, the solver generates a Benders optimality cut at each
 *   step from the reduced costs of the dependent state variables and
 *   adds it to the previous phase's LP.  Every non-last phase
 *   receives exactly one optimality cut per backward iteration (cuts
 *   land on phases 0..T-2; phase T-1 produces no outgoing cut).
 *
 *   The backward pass contains **no elastic branch and installs no
 *   feasibility cuts**.  Forward-pass infeasibility is handled at
 *   forward-pass time (`sddp_forward_pass.cpp`): when the original
 *   LP at phase k is infeasible at the trial state, the elastic
 *   filter (Chinneck Phase-1) runs on a clone, emits a feasibility
 *   cut on phase k-1, and returns the clone's solution.  The
 *   backward pass then treats phase k's cached solution as trial
 *   data and builds a standard optimality cut on phase k-1.
 *
 * **Multi-scene support** – each scene is an independent trajectory (like
 *   a PLP scenario).  Scenes are solved in parallel via the work pool.
 *
 * **Cut sharing** – optimality cuts generated in one scene can be shared
 *   with other scenes at the same phase level.  Three modes are supported:
 *   - None:     cuts stay in their originating scene (default)
 *   - Expected: an average cut across scenes is computed and added to all
 *   - Max:      all cuts from all scenes are added to all scenes
 *
 * **Cut persistence** – cuts can be saved to and loaded from JSON files
 *   for hot-start capability.
 *
 * The solver iterates until the gap between the upper bound (forward-pass
 * cost) and the lower bound (with future-cost approximation) falls below
 * a configurable tolerance, or a maximum iteration count is reached.
 */

#pragma once

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtopt/aperture.hpp>
#include <gtopt/aperture_data_cache.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/error.hpp>
#include <gtopt/fcut_log.hpp>
#include <gtopt/iteration_lp.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_method.hpp>
#include <gtopt/sddp_aperture.hpp>
#include <gtopt/sddp_cut_sharing.hpp>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/sddp_pool.hpp>
#include <gtopt/sddp_scene_tracker.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/solver_monitor.hpp>
#include <gtopt/solver_tier.hpp>
#include <gtopt/state_variable.hpp>

namespace gtopt
{

// Types (SDDPOptions, SDDPIterationResult, PhaseStateInfo,
// ForwardPassOutcome, BackwardPassOutcome, SDDPIterationCallback,
// sddp_file, compute_scene_weights, compute_convergence_gap,
// parse_cut_sharing_mode, parse_elastic_filter_mode) are provided by
// <gtopt/sddp_types.hpp> — included above.

// ─── SDDPMethod ─────────────────────────────────────────────────────────────

/**
 * @class SDDPMethod
 * @brief Iterative SDDP solver for multi-phase power system planning
 *
 * Wraps a `PlanningLP` and adds Benders decomposition on top of the
 * per-phase LP subproblems.  Handles reservoir volumes, capacity
 * expansion variables, and future state-variable types generically.
 *
 * Supports multiple scenes (solved in parallel), optimality cut sharing
 * between scenes, iterative feasibility backpropagation, and cut
 * persistence for hot-start.
 *
 * ## API for external monitoring / GUI integration
 *
 * The solver exposes a rich API that enables GUI or monitoring tools to
 * observe the iterative process and control execution:
 *
 * - **Callback**: register an `SDDPIterationCallback` via
 *   `set_iteration_callback()`.  It is invoked after every iteration with
 *   the full `SDDPIterationResult`.  Return `true` from the callback to
 *   request a stop.
 * - **Programmatic stop**: call `request_stop()` from any thread; the
 *   solver checks this flag at the start of each iteration and exits
 *   gracefully, saving all accumulated cuts.
 * - **Live query**: call `current_iteration()`, `current_gap()`,
 *   `current_lower_bound()`, `current_upper_bound()` at any time (they
 *   are atomic and thread-safe) to poll the solver's convergence state.
 * - **Sentinel file**: same as PLP's `userstop` — check for a sentinel
 *   file on disk.
 *
 * @code{.cpp}
 * SDDPMethod sddp(planning_lp, SDDPOptions{.max_iterations = 100});
 *
 * // Register a callback that prints progress and stops at gap < 1e-6
 * sddp.set_iteration_callback([](const SDDPIterationResult& r) {
 *     fmt::print("iter {} gap={:.6f}\n", r.iteration, r.gap);
 *     return r.gap < 1e-6;  // true → stop
 * });
 *
 * // Start solving (blocks until done / stopped)
 * auto results = sddp.solve();
 *
 * // Or stop programmatically from another thread:
 * sddp.request_stop();
 * @endcode
 */
class SDDPMethod
{
public:
  /// Aggregate per-iteration metrics exposed via the live-query API.
  ///
  /// Replaces five separate `std::atomic<T>` members with a single
  /// `std::atomic<std::shared_ptr<LiveMetrics>>` snapshot pointer so
  /// that cross-thread readers always observe a *coherent* set of
  /// `(iteration, gap, lower_bound, upper_bound, converged)` — no torn
  /// reads where iteration is from step N and gap is from step N+1.
  ///
  /// The writer (the main SDDP iteration thread) allocates a fresh
  /// `std::shared_ptr<LiveMetrics>` at the end of each iteration and
  /// publishes it with `std::memory_order_release`; readers acquire
  /// via `std::memory_order_acquire`.  Allocation rate is a few per
  /// second — negligible compared to the solve itself.
  struct LiveMetrics
  {
    IterationIndex iteration {0};
    double gap {1.0};
    double lower_bound {0.0};
    double upper_bound {0.0};
    bool converged {false};
  };

  explicit SDDPMethod(PlanningLP& planning_lp, SDDPOptions opts = {}) noexcept;

  /// Run the SDDP iterative solve
  [[nodiscard]] auto solve(const SolverOptions& lp_opts = {})
      -> std::expected<std::vector<SDDPIterationResult>, Error>;

  /// Ensure the solver is initialized (alpha variables, state links, etc.).
  /// Safe to call multiple times (guarded by m_initialized_ flag).
  /// Call before loading external cuts that reference alpha columns.
  [[nodiscard]] auto ensure_initialized() -> std::expected<void, Error>
  {
    return initialize_solver();
  }

  // ── Iteration callback / observer ──

  /// Register a callback invoked after each iteration.
  /// If the callback returns `true`, the solver stops after that iteration.
  void set_iteration_callback(SDDPIterationCallback cb) noexcept
  {
    m_iteration_callback_ = std::move(cb);
  }

  // ── Programmatic stop (thread-safe) ──

  /// Request the solver to stop gracefully after the current iteration.
  /// Thread-safe — may be called from any thread.
  void request_stop() noexcept { m_stop_requested_.store(true); }

  /// Clear a previous stop request (e.g., before re-running solve()).
  void clear_stop() noexcept { m_stop_requested_.store(false); }

  /// Check whether a stop has been requested.
  [[nodiscard]] bool is_stop_requested() const noexcept
  {
    return m_stop_requested_.load();
  }

  // ── α (future-cost) bookkeeping ──

  /// Release α's bootstrap pin at (scene, phase) so a subsequently-
  /// installed Benders (or boundary) cut can represent arbitrary
  /// future-cost values without being artificially clipped by the
  /// initial `lowb = uppb = 0` equality.  Idempotent.  Called
  /// automatically before every `add_row(alpha_cut)` on the source
  /// phase (backward pass, feasibility pass, aperture pass) and on
  /// the last phase when the first boundary cut is installed.
  /// Exposed publicly for characterisation tests and for external
  /// callers that install cuts on α directly.
  ///
  /// Both bounds are released: `lowb ← -DblMax`, `uppb ← +DblMax`.
  /// Under `low_memory = compress` the update is mirrored
  /// into the `m_dynamic_cols_` entry via `update_dynamic_col_bounds`
  /// so `apply_post_load_replay` preserves the freed bounds across a
  /// release+reload cycle.  Under `LowMemoryMode::off` only the live
  /// backend is modified — there is no snapshot to re-sync.
  void bound_alpha(SceneIndex scene_index, PhaseIndex phase_index);

  // ── Live query (thread-safe, atomic-shared_ptr reads) ──
  //
  // All five per-iteration metrics (iteration, gap, lower_bound,
  // upper_bound, converged) read from the same `LiveMetrics` snapshot
  // so callers observing multiple fields see a coherent view of the
  // solver's state at one point in time.

  /// Current iteration number (0 before first iteration completes)
  [[nodiscard]] IterationIndex current_iteration() const noexcept
  {
    return live_metrics_()->iteration;
  }

  /// Current relative convergence gap
  [[nodiscard]] double current_gap() const noexcept
  {
    return live_metrics_()->gap;
  }

  /// Current lower bound (phase-0 objective including α)
  [[nodiscard]] double current_lower_bound() const noexcept
  {
    return live_metrics_()->lower_bound;
  }

  /// Current upper bound (sum of actual phase costs)
  [[nodiscard]] double current_upper_bound() const noexcept
  {
    return live_metrics_()->upper_bound;
  }

  /// Whether the solver has converged
  [[nodiscard]] bool has_converged() const noexcept
  {
    return live_metrics_()->converged;
  }

  /// Current iteration-index offset.  Starts at 0 and advances past
  /// the last iteration executed at the end of each `solve()` call
  /// so re-entering `solve()` on the same instance keeps each cut's
  /// `IterationContext` disjoint from those already installed.  Also
  /// bumped by hot-start cut loaders to start past the max iteration
  /// found in the loaded file (see `initialize_solver`).
  [[nodiscard]] IterationIndex iteration_offset() const noexcept
  {
    return m_iteration_offset_;
  }

  /// Ratchet ``m_iteration_offset_`` forward to *value*.  Used by
  /// :class:`CascadePlanningMethod` between the forget-pass and the
  /// re-solve so phase-2 doesn't restart its iter counter at the
  /// phase-1 offset (which otherwise produces duplicate iter numbers
  /// in the per-iter log — observed as "iter 20" appearing twice at
  /// the first iter of every cascade level that ran a forget pass).
  /// ``std::max`` semantics: never moves the offset backwards.
  void bump_iteration_offset(IterationIndex value) noexcept
  {
    m_iteration_offset_ = std::max(m_iteration_offset_, value);
  }

  /// A single (UB, LB) pair from one prior cascade iter, used as
  /// an element of the cross-level Δgap seed array.
  struct PriorIterBounds
  {
    double upper_bound {0.0};
    double lower_bound {0.0};
  };

  /// Seed the stationary Δgap lookback with the prior cascade
  /// level's recent iter history.
  ///
  /// At construction the per-level ``results`` history is empty, so
  /// ``finalize_iteration_result`` falls back to the 1.0 sentinel for
  /// ``ir.gap_change`` on iter 1 (no lookback available).  When this
  /// solver is the second-or-later level of a :class:`CascadePlanning
  /// Method`, the previous level's last few iter bounds are a
  /// meaningful reference — the cuts and policy state have been
  /// inherited, so iter 1's Δgap should measure the cross-level UB
  /// delta against the prior level's tail (windowed) rather than a
  /// sentinel.
  ///
  /// **Array, not a single point**: ``stationary_window=N`` means the
  /// gap_change lookback compares ``results[size-1]`` against
  /// ``results[size-N]``.  At iter 1 of a new level the in-level
  /// ``results`` has size 0 / 1 / 2 — short of any window > 1.  Pass
  /// the LAST ``N`` (or more — STATIONARY_SEED_DEPTH below caps the
  /// max we'll consume) entries of the previous level so the new
  /// level's first ``N-1`` iters can still pull a windowed reference
  /// from the seed, ordered OLDEST-FIRST (the last element is the
  /// previous level's most-recent iter).
  ///
  /// ``finalize_iteration_result`` (sync) and the async equivalent
  /// at ``sddp_iteration.cpp:1846`` walk a combined index over
  /// ``[seed... | results...]`` to resolve the lookback target.
  ///
  /// **Convergence safety**: a tiny seed-vs-iter-1 Δgap can fall
  /// below ``stationary_tol`` on a level whose inherited envelope
  /// nearly matches the prior level's UB.  Pair this seed with
  /// ``min_iterations >= 2`` at the level (plp2gtopt enables this
  /// on L0/L1/L2) so the stationary check cannot fire on iter 1
  /// alone.  Without the min-iter guard, an L0→L1 transition where
  /// the multi-aperture solve happens to land at L0's UB would
  /// converge L1 immediately, defeating the whole cascade
  /// refinement.
  void seed_prior_bounds(std::vector<PriorIterBounds> history) noexcept
  {
    m_seed_prior_history_ = std::move(history);
  }

  /// Read the carried prior history (empty when no seed is installed
  /// — typical for L0 or for plain-SDDP runs).
  [[nodiscard]] auto seed_prior_bounds() const noexcept
      -> const std::vector<PriorIterBounds>&
  {
    return m_seed_prior_history_;
  }

  /// Max number of prior-level iter results the cascade orchestrator
  /// should pass into :func:`seed_prior_bounds`.  Chosen to cover any
  /// realistic ``stationary_window`` (current max in use is 4 for L0
  /// warmup; cascade L1+ use window ≤ 3).  Bigger is harmless — the
  /// gap_change calc only consumes ``min(window, seed.size() +
  /// results.size())`` entries — but keeps the per-cascade-transition
  /// memory footprint trivially small.
  static constexpr std::size_t STATIONARY_SEED_DEPTH = 8;

  /// Current pass: 0=idle, 1=forward, 2=backward
  [[nodiscard]] int current_pass() const noexcept
  {
    return m_current_pass_.load();
  }

  /// Number of scenes completed in the current pass
  [[nodiscard]] int scenes_done() const noexcept
  {
    return m_scenes_done_.load();
  }

  // ── Data accessors (valid after at least one iteration) ──

  /// Per-phase state for a given scene
  [[nodiscard]] constexpr auto& phase_states(
      SceneIndex scene_index) const noexcept
  {
    return m_scene_phase_states_[scene_index];
  }

  /// Legacy accessor for scene 0 (backward compatibility)
  [[nodiscard]] constexpr auto& phase_states() const noexcept
  {
    return m_scene_phase_states_[first_scene_index()];
  }

  /// Full scene-phase states (valid after ensure_initialized()).
  [[nodiscard]] constexpr auto& all_scene_phase_states() const noexcept
  {
    return m_scene_phase_states_;
  }

  /// Per-scene α-rebase offset c̄ (zero when not opted in via
  /// `SDDPOptions::boundary_cuts_mean_shift`).  The offset is now folded into
  /// the first-phase master LP's NATIVE objective offset at boundary-cut load
  /// (`initialize_solver`), so `get_obj_value()` — and by extension UB / LB —
  /// already carries it; the display sites no longer add it (that would
  /// double-count).  This accessor exposes the raw c̄ unchanged for tests and
  /// for `populate_future_cost_output`.
  [[nodiscard]] constexpr double scene_alpha_offset(
      SceneIndex scene_index) const noexcept
  {
    const auto idx = static_cast<std::size_t>(scene_index);
    return idx < m_scene_alpha_offsets_.size()
        ? m_scene_alpha_offsets_[scene_index]
        : 0.0;
  }

  /// SDDP options (const)
  [[nodiscard]] constexpr auto& options() const noexcept { return m_options_; }

  /// Mutable options access (for cascade orchestration between solve() calls).
  [[nodiscard]] constexpr auto& mutable_options() noexcept
  {
    return m_options_;
  }

  /// Clear all stored cut metadata (combined + per-scene).
  /// Used by CascadePlanningMethod between cascade phases.
  void clear_stored_cuts() noexcept;

  /// Remove the first @p count cuts from stored cuts and from the LP.
  /// This deletes the LP rows associated with those cuts (via
  /// `delete_rows`) and erases them from `m_stored_cuts_`.  Used by
  /// the cascade solver's "forget inherited cuts" feature: the first
  /// N cuts loaded via hot-start are dropped so the solver continues
  /// with only self-generated cuts.
  ///
  /// @param count  Number of leading cuts to remove (clamped to size).
  void forget_first_cuts(std::ptrdiff_t count);

  /// Union view over every stored cut across scenes, rebuilt on call.
  /// Equivalent to the former flat-vector accessor — kept for places
  /// (cascade transitions, save helpers) that need a combined list.
  [[nodiscard]] auto stored_cuts() const
  {
    return m_cut_store_.build_combined_cuts(planning_lp());
  }

  /// Total number of stored cuts across all scenes.
  [[nodiscard]] std::ptrdiff_t num_stored_cuts() const noexcept
  {
    return m_cut_store_.num_stored_cuts();
  }

  /// Access the cut manager (for cascade orchestration, tests, etc.).
  [[nodiscard]] SDDPCutManager& cut_manager() noexcept { return m_cut_store_; }

  /// Per-scene rollback state for `SDDPOptions::forward_infeas_rollback`.
  /// `global_cuts_at_last_failure` is set on the iteration where scene S
  /// was declared infeasible (forward pass).  At the next iteration's
  /// forward dispatch, S retries iff `m_cut_store_.num_stored_cuts()`
  /// has grown since the snapshot (peers contributed cuts).  When every
  /// previously-failed scene is "stalled" (no new cuts since failure),
  /// the run aborts with `SolverError`/`no recovery path`.
  ///
  /// **Terminal-skip extension (2026-05-02)** — adds a heavier-weight
  /// "stop re-dispatching" guard for scenes that fail forward with
  /// `"elastic filter produced no feasibility cut"` (= relaxed clone
  /// infeasible) on multiple consecutive iterations.  These scenes
  /// have a structurally infeasible LP at some phase; re-running their
  /// forward pass produces bit-identical failures and wastes ~50 s per
  /// iter per scene (juan/gtopt_iplp 2026-05-02 trace_29: 10 of 16
  /// scenes wasted ~33 min before being skipped).  Once the counter
  /// crosses ``terminal_failure_threshold``, ``terminal = true`` is
  /// set and the dispatch loop skips the scene until *new* cuts
  /// arrive globally (peer backward-pass cuts under cut sharing, or
  /// shared cuts via ``share_cuts_for_phase``) — at which point
  /// ``terminal`` is cleared and the scene retries normally.
  struct SceneRetryState
  {
    /// Snapshot of ``m_cut_store_.num_stored_cuts()`` at the iteration
    /// where this scene was last declared infeasible.  Used both by
    /// the existing rollback stall-stop guard and by the
    /// terminal-skip restart trigger below.  ``nullopt`` = scene is
    /// not currently in a failed-last-iter state.
    std::optional<std::ptrdiff_t> global_cuts_at_last_failure {};

    /// Count of consecutive iterations in which this scene failed
    /// forward with the structural-infeasibility signature
    /// (Chinneck filter could not produce a useful fcut).  Reset to
    /// zero on any forward-pass success, on any non-structural
    /// failure, or when ``terminal`` is cleared by the restart hook.
    int consecutive_structural_failures {0};

    /// True after ``consecutive_structural_failures`` crosses
    /// ``SDDPOptions::terminal_failure_threshold``.  When true, the
    /// dispatch loop in ``run_forward_pass_all_scenes`` skips the
    /// scene's forward pass and synthesises an infeasible result —
    /// no LP solves run for the scene this iteration.  Cleared when
    /// fresh cuts arrive globally (delta on
    /// ``num_stored_cuts()`` since
    /// ``global_cuts_at_last_failure``), since any new cut could in
    /// principle alter the trial state and unlock the previously
    /// infeasible phase.
    bool terminal {false};
  };

  /// Mutable per-scene retry state (test-friendly accessor).  Promoted
  /// to public so characterisation tests can synthesise the
  /// "scene S failed last iteration" state without first forcing a
  /// real LP infeasibility — set
  /// `scene_retry_state(s).global_cuts_at_last_failure = current_count`
  /// then drive `solve()` to exercise the stall-stop guard at the next
  /// forward dispatch.  Production callers should not mutate this
  /// vector directly; the rollback hook + stall guard in
  /// `run_forward_pass_all_scenes` own the lifecycle.
  [[nodiscard]] SceneRetryState& scene_retry_state(SceneIndex scene_index)
  {
    return m_scene_retry_state_[scene_index];
  }
  [[nodiscard]] const SceneRetryState& scene_retry_state(
      SceneIndex scene_index) const
  {
    return m_scene_retry_state_[scene_index];
  }

  /// Save accumulated cuts to a CSV file for hot-start
  [[nodiscard]] auto save_cuts(const std::string& filepath) const
      -> std::expected<void, Error>;

  /// Save cuts for a single scene to a per-scene file.
  /// Uses scene-specific storage, avoiding lock contention when called
  /// in parallel for different scenes.
  [[nodiscard]] auto save_scene_cuts(SceneIndex scene_index,
                                     const std::string& directory) const
      -> std::expected<void, Error>;

  /// Save all scenes' cuts to per-scene files in the given directory.
  /// Each scene gets its own file: `<directory>/scene_<N>.csv`.
  [[nodiscard]] auto save_all_scene_cuts(const std::string& directory) const
      -> std::expected<void, Error>;

  /// Load cuts from a CSV file and add to all scenes' phase LPs.
  /// Cuts are broadcast to all scenes regardless of originating scene,
  /// since loaded cuts serve as warm-start approximations for the entire
  /// problem (analogous to PLP's cut sharing across scenarios).
  [[nodiscard]] auto load_cuts(const std::string& filepath)
      -> std::expected<CutLoadResult, Error>;

  /// Load all per-scene cut files from a directory.
  /// Files matching `scene_<N>.csv` are loaded; files with the `error_`
  /// prefix (from infeasible scenes in a previous run) are skipped to
  /// prevent loading invalid cuts during hot-start.
  [[nodiscard]] auto load_scene_cuts_from_directory(
      const std::string& directory) -> std::expected<CutLoadResult, Error>;

  /// Load boundary (future-cost) cuts from a named-variable CSV file.
  ///
  /// The CSV header names the state variables (e.g. reservoir or battery
  /// names); subsequent rows provide {name, scenario, rhs, coefficients}.
  /// Cuts are added only to the last phase, with an alpha column created
  /// if needed.  This is analogous to PLP's "planos de embalse".
  ///
  /// @return CutLoadResult with count and max iteration, or an error.
  [[nodiscard]] auto load_boundary_cuts(const std::string& filepath)
      -> std::expected<CutLoadResult, Error>;

  // ``load_named_cuts`` was retired in 2026-05.  The "named hot-start
  // cuts" CSV format was an internal-gtopt format; internal cuts now
  // travel exclusively via the typed Parquet writer / loader
  // (``save_cuts_parquet`` / ``load_cuts_parquet``).  Boundary cuts
  // (PLP-imported "planos de embalse") remain CSV-compatible via
  // ``load_boundary_cuts`` above — the only CSV cut path left.

  /// Get the global max kappa across all (scene, phase) LP solves.
  /// Returns the cached max regardless of when it was observed — used
  /// by the SDDP warning machinery that just needs "worst kappa seen
  /// during this solve".
  [[nodiscard]] double global_max_kappa() const noexcept
  {
    double gmax = relaxed_load(m_seed_max_kappa_);
    for (std::size_t s = 0; s < m_max_kappa_.size(); ++s) {
      auto& phase_kappas = m_max_kappa_[SceneIndex {s}];
      for (std::size_t p = 0; p < phase_kappas.size(); ++p) {
        const double k = relaxed_load(phase_kappas[PhaseIndex {p}]);
        if (k >= 0.0) {
          gmax = std::max(gmax, k);
        }
      }
    }
    return gmax;
  }

  /// Per-iter ``kappa`` for the per-iter log clause.  Returns the max
  /// kappa observed during *this* iteration only — i.e. a freshly
  /// computed condition number that reflects the LP at this iter, not
  /// the historical maximum.  Once a CPLEX barrier solve returns
  /// nullopt (no basis available, the common case under
  /// ``backward_resolve_target``-driven re-solves), no cell on the
  /// current iter gets updated, so this returns ``-1.0`` and the
  /// per-iter log clause is suppressed — much more honest than
  /// re-displaying the LP-construction probe value every iter.
  /// Different from :meth:`global_max_kappa` which returns the
  /// historical max for solve-time warnings.
  [[nodiscard]] double current_iter_max_kappa(
      IterationIndex iter) const noexcept
  {
    double imax = -1.0;
    for (std::size_t s = 0; s < m_max_kappa_iter_.size(); ++s) {
      auto& phase_iters = m_max_kappa_iter_[SceneIndex {s}];
      auto& phase_vals = m_max_kappa_[SceneIndex {s}];
      for (std::size_t p = 0; p < phase_iters.size(); ++p) {
        if (relaxed_load(phase_iters[PhaseIndex {p}]) == iter) {
          const double v = relaxed_load(phase_vals[PhaseIndex {p}]);
          if (v >= 0.0) {
            imax = std::max(imax, v);
          }
        }
      }
    }
    return imax;
  }

  /// Seed the kappa baseline for this solver from an earlier solver's
  /// max kappa.  Used by ``CascadePlanningMethod`` to carry the
  /// warmup-derived condition number forward across cascade levels so
  /// that observability (the ``kappa=…`` clause in the iter log) is
  /// preserved when CPLEX barrier without crossover leaves the new
  /// level's LPs without a freshly-queryable kappa.  Negative values
  /// (including the ``-1`` sentinel meaning "no observation yet") are
  /// ignored, so calling with ``other.global_max_kappa()`` is safe even
  /// when the other solver also never produced a kappa.
  void seed_max_kappa(double kappa) noexcept
  {
    if (kappa >= 0.0) {
      relaxed_max(m_seed_max_kappa_, kappa);
    }
  }

  // ─── Alpha / state-variable lifecycle (public for testability) ────────
  // Promoted from ``private:`` 2026-04-28 in support of the
  // ``test_sddp_method.cpp::SDDPMethod alpha lifecycle`` and
  // ``state-var capture round-trip`` test cases that pin behavior
  // across the Phase B ``sddp_method.cpp`` split.

  void initialize_alpha_variables(SceneIndex scene_index);
  void collect_state_variable_links(SceneIndex scene_index);

  /// Mirror per-state-variable runtime values onto the persistent
  /// `StateVariable` objects after a forward (or elastic-clone) solve.
  ///
  /// Always writes `col_sol` for every state variable of the solved
  /// phase, and `reduced_cost` on each source state variable reached
  /// via an incoming link from the previous phase.  Cut construction
  /// reads both fields directly from the persistent `StateVariable`
  /// objects, avoiding per-phase full-vector caches.
  ///
  /// `col_sol_phys` is a **physical-space** view (from
  /// `LinearInterface::get_col_sol()`) so solver-tolerance noise has
  /// already been clamped to each column's physical bound box.  The
  /// captured raw value is recovered by dividing by the state
  /// variable's `var_scale()`, yielding a clean raw LP value for
  /// `StateVariable::col_sol()` consumers (cut builders, next-phase
  /// `propagate_trial_values`).
  void capture_state_variable_values(
      SceneIndex scene_index,
      PhaseIndex phase_index,
      const ScaledView& col_sol_phys,
      std::span<const double> reduced_costs) const noexcept;

private:
  using scene_phase_states_t =
      StrongIndexVector<SceneIndex,
                        StrongIndexVector<PhaseIndex, PhaseStateInfo>>;

  /// Type alias for backward compatibility — now uses the public
  /// ElasticSolveResult from benders_cut.hpp.
  using ElasticResult = ElasticSolveResult;

  /// Scene-aware effective CPU over-commit factor for the SDDP work pools.
  ///
  /// When the user set `--cpu-factor` / `sddp_options.pool_cpu_factor`
  /// explicitly (`pool_cpu_factor_user_set`), returns the resolved value
  /// unchanged.  Otherwise applies the measured many-scene default:
  /// the async backward pass fans out `num_scenes × aperture-chunks`
  /// concurrent tasks, so once the run has enough scenes to saturate the
  /// cores on its own, the historical 4× over-commit only adds scheduler
  /// and futex contention (perf c2c: top contended cachelines are kernel
  /// futex/scheduler locks) — A/B measured ~4% slower at 18/30 scenes.
  /// We therefore cap the factor to 1.0 once
  /// `num_scenes >= max(2, physical_concurrency() / 4)` — a
  /// machine-scaled threshold (≥10 scenes on a 40-core box, ≥2 on a
  /// small box).  Few-scene runs keep the 4× over-commit, which still
  /// helps hide the clone-mutex / solver blocking there.
  [[nodiscard]] double effective_pool_cpu_factor() const noexcept;

  [[nodiscard]] auto forward_pass(SceneIndex scene_index,
                                  const SolverOptions& opts,
                                  IterationIndex iteration_index)
      -> std::expected<double, Error>;

  [[nodiscard]] auto backward_pass(SceneIndex scene_index,
                                   const SolverOptions& opts,
                                   IterationIndex iteration_index = {})
      -> std::expected<int, Error>;

  /**
   * @brief Backward pass with apertures (hydrological realisations).
   *
   * For each phase in the backward pass (from last to first), this method
   * solves the phase LP once per aperture.  Each aperture corresponds to
   * one of the available simulation scenarios; the flow column bounds are
   * updated to the aperture scenario's discharge values before solving.
   * The probability-weighted average of all aperture cuts (expected cut) is
   * then added to the previous phase's LP.
   *
   * Apertures use the same state-variable trial values as the original
   * forward pass (only the flow bounds change).  If a specific aperture LP
   * is infeasible the aperture is skipped and its probability weight is
   * discarded.
   *
   * Falls back to the regular backward_pass() if num_apertures is 0 or no
   * scenarios are available.
   *
   * Uses the work pool for parallel aperture solves when available.
   */
  /// @param exec_pool  Pool that runs aperture chunks + owns the slot
  /// release.  `nullptr` (default) runs apertures inline on the calling
  /// thread — used by the coordinator-driven training backward (one driver
  /// thread per scene).  The async/cascade path passes the live pool so its
  /// scene tasks (which run ON pool workers) keep submitting chunks + releasing
  /// their slot while blocking.
  [[nodiscard]] auto backward_pass_with_apertures(
      SceneIndex scene_index,
      const SolverOptions& opts,
      IterationIndex iteration_index = {},
      SDDPWorkPool* exec_pool = nullptr) -> std::expected<int, Error>;

  // ─── Iteration helpers (public for testability) ──────────────────
  // Promoted 2026-04-28 to support the Group D unit tests in
  // ``test_sddp_method.cpp``; semantically still iteration internals.

public:
  /// Update the per-(scene, phase) max kappa value after an LP solve.
  /// Also checks kappa against the threshold and emits a warning or
  /// saves the LP file depending on the kappa_warning mode.
  void update_max_kappa(SceneIndex scene_index,
                        PhaseIndex phase_index,
                        const LinearInterface& li,
                        IterationIndex iteration_index = {});

  /// Analyze cut rows to identify which Benders cuts have the worst
  /// coefficient ratios.  Called by update_max_kappa when
  /// kappa_warning == diagnose and kappa exceeds the threshold.
  void diagnose_kappa(SceneIndex scene_index,
                      PhaseIndex phase_index,
                      const LinearInterface& li,
                      IterationIndex iteration_index);

  /// Update max kappa from an already-known value (no LP save possible).
  void update_max_kappa(SceneIndex scene_index,
                        PhaseIndex phase_index,
                        double kappa) noexcept
  {
    double& cell = m_max_kappa_[scene_index][phase_index];
    if (kappa >= 0.0) {
      relaxed_max(cell, kappa);
    } else if (relaxed_load(cell) < 0.0) {
      relaxed_store(cell, kappa);
    }
  }

  /// Check whether update_lp should be dispatched for this iteration.
  /// Returns false when the iteration is explicitly disabled or skipped
  /// by the global skip count.
  [[nodiscard]] bool should_dispatch_update_lp(
      IterationIndex iteration_index) const;

  /// Read-only view of the per-(scene, phase) max-kappa accumulator.
  /// Returns the largest kappa observed across all
  /// ``update_max_kappa`` calls for this cell, or a negative
  /// sentinel if no LP has been solved on it yet.
  /// (Added 2026-04-28 for testability — exposes the accumulator
  /// driven by the ``update_max_kappa`` overloads.)
  [[nodiscard]] double max_kappa(SceneIndex scene_index,
                                 PhaseIndex phase_index) const noexcept
  {
    return relaxed_load(m_max_kappa_[scene_index][phase_index]);
  }

private:
  // ── Lock-free kappa-grid access (std::atomic_ref, relaxed) ──────────
  // The kappa grid (m_max_kappa_ / m_max_kappa_iter_ / m_seed_max_kappa_) is a
  // monotone diagnostic aggregate: cells only grow toward a max, no other
  // state is published through them, and the whole-grid readers tolerate an
  // eventually-consistent (smeared) snapshot.  So per-cell relaxed atomics
  // replace what would otherwise be a grid mutex — no thread ever blocks.
  // update_max_kappa runs concurrently from parallel forward/backward/aperture
  // solve tasks (write-write on the same cell across apertures) while the
  // per-iter log reads the whole grid: a TSan-confirmed race, now race-free.
  // The grid members are `mutable` so the const readers can form a non-const
  // atomic_ref.
  template<class T>
  [[nodiscard]] static T relaxed_load(T& cell) noexcept
  {
    return std::atomic_ref<T> {cell}.load(std::memory_order_relaxed);
  }
  template<class T>
  static void relaxed_store(T& cell, T value) noexcept
  {
    std::atomic_ref<T> {cell}.store(value, std::memory_order_relaxed);
  }
  /// Fold `value` into `cell` as a running max, lock-free (CAS loop).
  static void relaxed_max(double& cell, double value) noexcept
  {
    std::atomic_ref<double> ref {cell};
    double cur = ref.load(std::memory_order_relaxed);
    while (value > cur
           && !ref.compare_exchange_weak(cur, value, std::memory_order_relaxed))
    {}
  }

  /// Run update_lp for a single phase, setting prev_phase_sys for
  /// cross-phase physical_eini lookup.  Returns the number of updated
  /// LP elements.
  int update_lp_for_phase(SceneIndex scene_index, PhaseIndex phase_index);

  /// Overwrite `(scene_index, phase_index)`'s forward-LP stochastic
  /// bounds with scene @p realization's scenario data
  /// (`ForwardSamplingMode::resampled`).  Routes through the same
  /// per-element `update_aperture` machinery the aperture backward
  /// pass uses (flow discharge column bounds — or the AR equality-row
  /// RHS under `Flow.inflow_model` — plus demand/generator/capacity
  /// profile bounds; dense overwrite, replay-recorded so a low-memory
  /// release/reload preserves it).  v1: the applied realization is the
  /// drawn scene's FIRST scenario (the same `scenarios().front()` base
  /// convention as the aperture path).  No-op when either scene has no
  /// scenarios.  Shared by the forward pass (apply the draw) and the
  /// backward target re-solve (re-apply the cached draw, so the cut is
  /// provably built from the SAME realization).
  void apply_sampled_realization(SceneIndex scene_index,
                                 PhaseIndex phase_index,
                                 SceneIndex realization);

  /// Clone the LP, apply elastic filter on the clone, and solve it.
  /// Returns an ElasticResult (with solution data and per-link slack info)
  /// if feasible, nullopt otherwise.
  /// The original LP is never modified (PLP clone pattern).
  [[nodiscard]] std::optional<ElasticResult> elastic_solve(
      SceneIndex scene_index,
      PhaseIndex phase_index,
      const SolverOptions& opts);

  // ── Refactored helper methods ──

  /// Store a cut for sharing and persistence (thread-safe).
  /// Writes to both per-scene storage and shared storage.
  void store_cut(SceneIndex scene_index,
                 PhaseIndex src_phase_index,
                 const SparseRow& cut,
                 CutType type = CutType::Optimality,
                 RowIndex row = RowIndex {-1});

  /// Resolve an LP via the SDDP work pool.  Falls back to direct resolve if
  /// the pool is not available.  Avoids naked direct resolve() calls.
  [[nodiscard]] auto resolve_via_pool(
      LinearInterface& li,
      const SolverOptions& opts,
      const BasicTaskRequirements<SDDPTaskKey>& task_req = {})
      -> std::expected<int, Error>;

  /// Resolve a cloned LP via the SDDP work pool.  The clone is moved into a
  /// shared_ptr for the pool task, then moved back after completion.
  [[nodiscard]] auto resolve_clone_via_pool(
      LinearInterface& clone,
      const SolverOptions& opts,
      const BasicTaskRequirements<SDDPTaskKey>& task_req = {})
      -> std::expected<int, Error>;

  /// Create a submit function that submits complete aperture tasks to
  /// the work pool for parallel execution.
  ///
  /// When the SDDP work pool is available, aperture tasks are submitted
  /// to the pool for concurrent execution across available threads.
  /// When the pool is unavailable, tasks run synchronously in the caller.
  ///
  /// @param phase      Phase index (for pool priority ordering)
  /// @param iteration  SDDP iteration index (for pool priority ordering)
  [[nodiscard]] auto make_aperture_submit_fn(PhaseIndex phase_index,
                                             IterationIndex iteration_index,
                                             SDDPWorkPool* pool)
      -> ApertureChunkSubmitFunc;

  /// Prune inactive cuts from all (scene, phase) LPs.
  /// Removes cuts whose |dual| < prune_dual_threshold, keeping at most
  /// max_cuts_per_phase active cuts per LP.  Only LP rows are removed;
  /// m_stored_cuts_ and m_scene_cuts_ are preserved for persistence.
  void prune_inactive_cuts();

  /// Cap per-scene stored cuts to max_stored_cuts (oldest dropped first).
  void cap_stored_cuts();

  /// Build combined stored cuts from per-scene vectors (for persistence).
  [[nodiscard]] std::vector<StoredCut> build_combined_cuts() const;

  // ─── Stop-condition helpers (public for testability) ──────────────
  // Promoted 2026-04-28 to support the Group C unit tests in
  // ``test_sddp_method.cpp``.  Production callers are still inside
  // ``solve()`` / ``run_iteration()`` so the practical API surface
  // is unchanged.

public:
  /// Check whether the sentinel file exists (user-requested stop)
  [[nodiscard]] bool check_sentinel_stop() const;

  /// Check whether the monitoring API stop-request file exists
  [[nodiscard]] bool check_api_stop_request() const;

  /// Check all stop conditions: sentinel file, API stop request, programmatic
  /// stop, callback
  [[nodiscard]] bool should_stop() const;

private:
  /// Apply cut sharing across scenes for a given phase
  void share_cuts_for_phase(
      PhaseIndex phase_index,
      const StrongIndexVector<SceneIndex, std::vector<SparseRow>>& scene_cuts,
      IterationIndex iteration_index);

  /// Validate that the simulation has ≥2 phases and ≥1 scene.
  [[nodiscard]] auto validate_inputs() const -> std::optional<Error>;

  /// Bootstrap-solve + initialize α variables, state links, and hot-start.
  /// Called once (guarded by m_initialized_).
  [[nodiscard]] auto initialize_solver() -> std::expected<void, Error>;

  /// Publish the per-scene α-rebase constants c̄ onto the persistent
  /// SimulationLP (`set_alpha_offsets`) so `FutureCostLP::add_to_output` can
  /// SELF-FIND them — and its α columns, via the persistent state-variable
  /// registry — at write time.  Called at the END of solve(), sync + async.
  /// Read-only w.r.t. the LP.  Because the simulation outlives the per-cell LP
  /// rebuild that `write_out` performs under compress, the FutureCost α-output
  /// works under ALL low_memory modes.
  void populate_future_cost_output();

  /// Reset live-query atomics to their start-of-solve values.
  void reset_live_state() noexcept;

  /// Run the forward pass for all scenes in parallel.
  /// Returns a ForwardPassOutcome or an error if ALL scenes failed.
  [[nodiscard]] auto run_forward_pass_all_scenes(SDDPWorkPool& pool,
                                                 const SolverOptions& opts,
                                                 IterationIndex iteration_index)
      -> std::expected<ForwardPassOutcome, Error>;

  /// Run the backward pass for all feasible scenes in parallel.
  /// When cut_sharing is None, scenes run their full backward pass
  /// independently.  When cut sharing is enabled, the backward pass is
  /// synchronized per-phase: all scenes complete a phase before optimality
  /// cuts are shared and the next phase is processed.
  [[nodiscard]] auto run_backward_pass_all_scenes(
      std::span<const uint8_t> scene_feasible,
      SDDPWorkPool& pool,
      const SolverOptions& opts,
      IterationIndex iteration_index) -> BackwardPassOutcome;

  /// Process a single backward-pass phase step (pi → pi-1) for one scene.
  /// Builds the optimality cut, stores it, adds it to the LP, re-solves,
  /// and handles feasibility backpropagation.
  /// @return Number of optimality cuts added during this step.
  [[nodiscard]] auto backward_pass_single_phase(SceneIndex scene_index,
                                                PhaseIndex phase_index,
                                                int cut_offset,
                                                const SolverOptions& opts,
                                                IterationIndex iteration_index)
      -> std::expected<int, Error>;

  /// Process a single backward-pass phase step (pi → pi-1) with apertures.
  /// @return Number of optimality cuts added during this step.
  /// @param exec_pool  when non-null, aperture chunks are submitted to this
  ///   pool (Tier-2 parallelism) instead of running inline on the caller
  ///   thread.  Used by the synchronized backward pass when num_scenes <
  ///   cores so the aperture dimension fills the otherwise-idle cores.
  [[nodiscard]] auto backward_pass_with_apertures_single_phase(
      SceneIndex scene_index,
      PhaseIndex phase_index,
      int cut_offset,
      const SolverOptions& opts,
      IterationIndex iteration_index,
      SDDPWorkPool* exec_pool = nullptr) -> std::expected<int, Error>;

  /// Implementation helper for aperture per-phase backward step.
  /// Used by backward_pass_with_apertures_single_phase.  @p exec_pool is
  /// forwarded to `make_aperture_submit_fn` (null → inline apertures).
  [[nodiscard]] auto backward_pass_aperture_phase_impl(
      SceneIndex scene_index,
      PhaseIndex phase_index,
      int cut_offset,
      const ScenarioLP& base_scenario,
      std::span<const ScenarioLP> all_scenarios,
      std::span<const Aperture> aperture_defs,
      const SolverOptions& opts,
      IterationIndex iteration_index,
      SDDPWorkPool* exec_pool = nullptr) -> std::expected<int, Error>;

  /// Install a Benders cut on the source-phase LP during the aperture
  /// backward pass.  Shared by both aperture entry points.
  ///
  /// When `expected_cut` has a value (aperture produced the richer
  /// multi-scenario cut): adds it to `src_li`, resolves when
  /// `src_phase_index > 0`, records kappa on success.  On the rare
  /// non-optimal resolve, deletes the freshly-added row and falls back to
  /// a `bcut` built from cached forward-pass state-variable reduced costs
  /// — a valid Benders underestimator that cannot make src_li infeasible.
  ///
  /// When `expected_cut` is empty (aperture itself produced no cut):
  /// installs the bcut directly without resolving.
  ///
  /// The bcut is constructed from `StateVariable::reduced_cost()` values
  /// captured by the forward pass via `capture_state_variable_values()`.
  /// Those cached values are refreshed each forward solve and never
  /// overwritten by the backward pass, so the bcut always reflects the
  /// most recent forward-pass optimum.
  [[nodiscard]] auto install_aperture_backward_cut(
      SceneIndex scene_index,
      PhaseIndex src_phase_index,
      PhaseIndex phase_index,
      int cut_offset,
      IterationIndex iteration_index,
      ColIndex src_alpha_col,
      const PhaseStateInfo& src_state,
      const PhaseStateInfo& target_state,
      std::optional<SparseRow> expected_cut,
      LinearInterface& src_li,
      const SolverOptions& opts) -> int;

  /// Phase-synchronized backward pass: processes phases one at a time,
  /// sharing optimality cuts between scenes after each phase completes.
  [[nodiscard]] auto run_backward_pass_synchronized(
      std::span<const uint8_t> scene_feasible,
      SDDPWorkPool& pool,
      const SolverOptions& opts,
      IterationIndex iteration_index) -> BackwardPassOutcome;

  /// Asynchronous scene execution: when cut_sharing == none and
  /// max_async_spread > 0, scenes run their own forward/backward
  /// iteration loops at different speeds.  The SDDPTaskKey priority
  /// (lower iteration = higher priority) self-regulates the spread.
  [[nodiscard]] auto solve_async(SDDPWorkPool& pool,
                                 const SolverOptions& fwd_opts,
                                 const SolverOptions& bwd_opts)
      -> std::expected<std::vector<SDDPIterationResult>, Error>;

public:
  // ── Iteration step helpers (public so they are independently testable)
  // These are the per-iteration building blocks that `solve()` and
  // `solve_async()` call.  Their pure-arithmetic contracts (weighted
  // bounds, convergence gate, gap_change look-back) are easier to pin
  // down with direct unit tests than via a full SDDP run.  Production
  // callers stay inside this class; external callers have no reason to
  // touch them directly.

  /// Compute and fill ir.upper_bound, ir.lower_bound, ir.scene_lower_bounds.
  void compute_iteration_bounds(SDDPIterationResult& ir,
                                std::span<const uint8_t> scene_feasible,
                                std::span<const double> weights) const;

  /// Apply cut-sharing across scenes for all phases generated in this
  /// iteration.
  /// @param iteration    Current SDDP iteration index.
  ///
  /// New cuts are identified by comparing each scene's current
  /// `m_scene_cuts_` vector size against `m_scene_cuts_before_` —
  /// the caller must populate the latter before the backward pass.
  void apply_cut_sharing_for_iteration(IterationIndex iteration_index);

  /// Compute gap + gap_change, update convergence flag, update live-query
  /// atomics, log.  @p results carries the previous iterations so that
  /// gap_change can be evaluated against the look-back window before the
  /// log line is emitted — otherwise the mid-iteration log would show a
  /// stale 1.0 sentinel while the later "done" log shows the real value.
  void finalize_iteration_result(
      SDDPIterationResult& ir,
      IterationIndex iteration_index,
      const std::vector<SDDPIterationResult>& results);

  /// Write the monitoring API status file if API is enabled.
  void maybe_write_api_status(const std::string& status_file,
                              const std::vector<SDDPIterationResult>& results,
                              std::chrono::steady_clock::time_point solve_start,
                              SolverMonitor& monitor) const;

  /// Save cuts (combined + per-scene) after an iteration, handling infeasible
  /// scene renaming.
  void save_cuts_for_iteration(IterationIndex iteration_index,
                               std::span<const uint8_t> scene_feasible);

private:
  // Accessor for the wrapped PlanningLP reference (avoids raw reference member)
  [[nodiscard]] PlanningLP& planning_lp() noexcept
  {
    return m_planning_lp_.get();
  }
  [[nodiscard]] const PlanningLP& planning_lp() const noexcept
  {
    return m_planning_lp_.get();
  }

  /// Convert a `SceneIndex` to the matching `SceneUid`.  Mirror
  /// of `SimulationLP::uid_of(SceneIndex)` for callers that already
  /// hold an SDDPMethod reference — avoids going through
  /// `planning_lp().simulation().uid_of(...)` at every call site.
  [[nodiscard]] SceneUid uid_of(SceneIndex si) const noexcept
  {
    return planning_lp().simulation().uid_of(si);
  }

  /// Convert a `PhaseIndex` to the matching `PhaseUid`.
  [[nodiscard]] PhaseUid uid_of(PhaseIndex pi) const noexcept
  {
    return planning_lp().simulation().uid_of(pi);
  }

  std::reference_wrapper<PlanningLP> m_planning_lp_;
  SDDPOptions m_options_;
  ApertureDataCache m_aperture_cache_;
  LabelMaker m_label_maker_;
  scene_phase_states_t m_scene_phase_states_;
  SDDPCutManager m_cut_store_;

  /// Per-scene α-rebase offsets accumulated during boundary-cut load.
  ///
  /// When `SDDPOptions::boundary_cuts_mean_shift` is enabled,
  /// `load_boundary_cuts_csv` shifts each scene's boundary cut RHSs
  /// by `−c̄_scene` so the LP-side α is centred near zero.  The
  /// offsets land here so SDDP UB / LB display can re-add `c̄_scene`
  /// and present the algebraically-original physical objective —
  /// the LP itself stays in shifted space.  Indexed by `SceneIndex`;
  /// zero-filled when the shift is disabled or no cuts landed on a
  /// scene.  See `SDDPOptions::boundary_cuts_mean_shift` and the
  /// install loop in `source/sddp_boundary_cuts.cpp` for details.
  StrongIndexVector<SceneIndex, double> m_scene_alpha_offsets_;

  /// NVarPhi of `BoundaryCutsMode::phi_expectation` — the number of
  /// distinct plane hydrologies in the boundary-cut CSV, resolved by
  /// `initialize_solver`'s pre-scan (`boundary_cut_scene_count`) BEFORE
  /// α registration so `register_alpha_variables` can lay down the
  /// NVarPhi terminal φ_j columns.  0 = mode inactive (every other
  /// `boundary_cuts_mode`, or a missing/unreadable CSV) — the terminal
  /// layout then follows `boundary_cut_sharing` unchanged.
  std::size_t m_boundary_phi_count_ {0};

  /// Per-(scene, phase) count of consecutive forward-pass infeasibilities.
  /// Incremented when the elastic filter is used in forward_pass at (scene,
  /// phase).  Reset to 0 when the phase is solved normally (no elastic).
  /// Used by the backward pass to decide single_cut vs multi_cut mode.
  StrongIndexVector<SceneIndex, StrongIndexVector<PhaseIndex, int>>
      m_infeasibility_counter_;

  /// Per-(scene, phase) maximum kappa (condition number) across all LP
  /// solves (forward, backward, aperture).  Updated after every solve call.
  mutable StrongIndexVector<SceneIndex, StrongIndexVector<PhaseIndex, double>>
      m_max_kappa_;

  /// Per-(scene, phase) iteration index of the most recent kappa
  /// observation.  Tracked alongside ``m_max_kappa_`` so the per-iter
  /// log clause can filter to "kappa observed *this iter*" rather than
  /// re-displaying the historical max every iteration.  Default
  /// ``IterationIndex{}`` (= 0) is fine because the first solve at iter
  /// 0 will overwrite it; subsequent iters either update with a fresh
  /// observation or leave the prior iter index in place, which
  /// :meth:`current_iter_max_kappa(iter)` then rejects.
  mutable StrongIndexVector<SceneIndex,
                            StrongIndexVector<PhaseIndex, IterationIndex>>
      m_max_kappa_iter_;

  /// Baseline kappa seeded by an earlier solver via :meth:`seed_max_kappa`
  /// — typically the warmup-level max in a cascade run.  Folded into
  /// :meth:`global_max_kappa` so the iter log keeps the ``kappa=…``
  /// clause across cascade levels even when CPLEX barrier without
  /// crossover leaves the new level's LPs without a queryable basis.
  /// Negative sentinel ``-1.0`` means "no seed" (back-compat with
  /// standalone SDDP runs).
  mutable double m_seed_max_kappa_ {-1.0};

  /// Per-scene retry state for `SDDPOptions::forward_infeas_rollback`.
  /// Public struct definition lives in the upper public block so the
  /// `scene_retry_state()` accessor (declared above) can return it.
  /// `global_cuts_at_last_failure` is empty (`std::nullopt`) until S
  /// has been declared infeasible at least once.  Cleared back to
  /// `std::nullopt` when S retries successfully (snapshot grew).
  /// Sized once in `initialize_solver`; reset on every fresh `solve()`
  /// call.
  StrongIndexVector<SceneIndex, SceneRetryState> m_scene_retry_state_;

  bool m_initialized_ {false};

  /// Preallocated iteration vector, sized to
  /// `iteration_offset + max_iterations`.
  /// Default-constructed entries represent iterations with no
  /// user override. Populated in initialize_solver() after iteration_offset is
  /// known.
  StrongIndexVector<IterationIndex, IterationLP> m_iterations_ {};

  /// Iteration offset from hot-start cuts.  When cuts from a previous
  /// run are loaded, the solver starts numbering new iterations after
  /// the highest iteration found in the loaded cuts, avoiding name
  /// collisions.
  IterationIndex m_iteration_offset_ {};

  /// Cross-level seed history installed by
  /// :func:`seed_prior_bounds`.  Each element is one (UB, LB) pair
  /// from a recent iter of the previous cascade level, ordered
  /// oldest-first; the last element is the previous level's most
  /// recent iter.  Consumed by ``finalize_iteration_result`` (sync)
  /// and the async gap_change site at ``sddp_iteration.cpp:1846``
  /// — those walk a combined index over ``[seed ⧺ results]`` so
  /// the new level's first ``stationary_window`` iters can still
  /// resolve a windowed lookback target.  Empty when this solver
  /// is not the second-or-later level of a cascade.  See setter
  /// docstring for the convergence-safety rationale (pair with
  /// min_iterations >= 2).
  std::vector<PriorIterBounds> m_seed_prior_history_ {};

  // ── Stop / callback machinery ──
  SDDPIterationCallback m_iteration_callback_ {};
  std::atomic<bool> m_stop_requested_ {false};
  /// When true, should_stop() returns false (simulation pass ignores stops).
  /// Atomic: the orchestrator toggles it (solve_async) while forward-pass
  /// worker threads read it (sddp_forward_pass.cpp) — TSan-confirmed race.
  std::atomic<bool> m_in_simulation_ {false};
  /// One-shot latch for the `integer_cuts=none` × integer-bearing-cell
  /// WARN (uncertified flat MIP cut — sddip doc §1).  Atomic: the
  /// backward pass runs one task per scene (`backward_pass_single_phase`
  /// on pool threads).
  std::atomic<bool> m_mip_flat_cut_warned_ {false};
  /// Two-phase simulation flag: false during Pass 1 (fcut collection, no
  /// crossover, no write_out), true during Pass 2 (crossover on, inline
  /// write_out fused into each cell's solve task).  Always false outside
  /// the simulation pass; the forward-pass body branches on this flag to
  /// decide whether to emit parquet shards and to gate the per-cell elastic-
  /// recovery logic.
  bool m_sim_write_enabled_ {false};

  // ── Atomic live-query state ──
  //
  // One `std::atomic<std::shared_ptr<LiveMetrics>>` replaces five
  // independent atomics (iteration, gap, lb, ub, converged) so that
  // cross-thread readers see a coherent snapshot.  Per-iteration the
  // writer allocates a fresh LiveMetrics and publishes the pointer;
  // readers acquire the current pointer and deref its fields.
  //
  // `m_current_pass_` and `m_scenes_done_` stay as separate atomics:
  // they change at a different rate (per-pass / per-scene-finish) and
  // don't belong in the per-iteration snapshot cadence.
  std::atomic<std::shared_ptr<LiveMetrics>> m_live_metrics_ {
      std::make_shared<LiveMetrics>()};
  std::atomic<int> m_current_pass_ {0};  ///< 0=idle, 1=forward, 2=backward
  std::atomic<int> m_scenes_done_ {0};  ///< Scenes completed in current pass

  /// Publish a fresh live-metrics snapshot.  Writer is always the
  /// main iteration thread; readers use acquire semantics via
  /// `live_metrics_()` / the live-query accessors above.
  void publish_live_metrics_(const LiveMetrics& metrics) noexcept
  {
    m_live_metrics_.store(std::make_shared<LiveMetrics>(metrics),
                          std::memory_order_release);
  }

  /// Mutate a single field of the live-metrics snapshot while leaving
  /// the others unchanged.  Used by early-converge writers (line 364
  /// etc. of sddp_iteration.cpp) that only want to flip `converged`.
  /// The read-modify-write is safe because the writer is still the
  /// single main iteration thread.
  template<class Mutator>
  void update_live_metrics_(Mutator&& mutator) noexcept
  {
    auto current = m_live_metrics_.load(std::memory_order_acquire);
    auto next = std::make_shared<LiveMetrics>(*current);
    std::invoke(std::forward<Mutator>(mutator), *next);
    m_live_metrics_.store(std::move(next), std::memory_order_release);
  }

  /// Helper: acquire-load the current live-metrics snapshot.  Never
  /// returns nullptr because the default-initialised member value is
  /// always a valid shared_ptr.
  [[nodiscard]] auto live_metrics_() const noexcept
      -> std::shared_ptr<LiveMetrics>
  {
    return m_live_metrics_.load(std::memory_order_acquire);
  }
  PhaseGridRecorder m_phase_grid_;  ///< Per-(iter,scene,phase) activity grid

  // ── BendersCut: wraps elastic-filter LP solves via the work pool ──
  /// Constructed with null pool; updated in solve() once the pool is created.
  /// Uses m_aux_pool_ (AdaptiveWorkPool) for LP solve monitoring.
  BendersCut m_benders_cut_;

  /// Non-owning pointer to the SDDP work pool (SDDPWorkPool) created in
  /// solve().  Uses SDDPTaskKey for tuple-based priority ordering.
  /// Set to non-null while solve() is running; reset to nullptr on return.
  /// Used by resolve_via_pool() and resolve_clone_via_pool().
  SDDPWorkPool* m_pool_ {nullptr};

  /// Tier-2 LP-solve executor (seam).  Wraps `m_pool_` today; later steps
  /// swap the engine behind it.  Kept in sync with `m_pool_` via set_pool().
  SolverTier m_solver_tier_ {};

  /// Non-owning pointer to the auxiliary AdaptiveWorkPool used by
  /// BendersCut (elastic LP solves) and LpDebugWriter (gzip compression).
  /// Separate from m_pool_ so that those helpers continue to use the
  /// simple int64_t key while the main SDDP LP solves use SDDPTaskKey.
  AdaptiveWorkPool* m_aux_pool_ {nullptr};

  /// LP debug writer — active when lp_debug is enabled and log_directory is
  /// set.  Initialised at the start of solve() and drained at the end.
  LpDebugWriter m_lp_debug_writer_ {};

  /// PLP-style feasibility-cut debug log (`gtopt_fcut.log`, the
  /// `plpfact.log` analogue) — armed at the start of solve() when
  /// `SDDPOptions::fcut_log` is set and `log_directory` is non-empty.
  /// Mutex-serialised: the forward passes run scene-parallel and each
  /// event record must land atomically.
  FcutLogWriter m_fcut_log_ {};
};

// ─── SDDPPlanningMethod ─────────────────────────────────────────────────────

/**
 * @class SDDPPlanningMethod
 * @brief Adapter that wraps SDDPMethod behind the PlanningMethod interface
 */
class SDDPPlanningMethod final : public PlanningMethod
{
public:
  explicit SDDPPlanningMethod(SDDPOptions opts = {}) noexcept;

  [[nodiscard]] auto solve(PlanningLP& planning_lp, const SolverOptions& opts)
      -> std::expected<int, Error> override;

  /// Access the last iteration results (valid after solve())
  [[nodiscard]] const auto& last_results() const noexcept
  {
    return m_last_results_;
  }

private:
  SDDPOptions m_sddp_opts_;
  std::vector<SDDPIterationResult> m_last_results_ {};
};

}  // namespace gtopt
