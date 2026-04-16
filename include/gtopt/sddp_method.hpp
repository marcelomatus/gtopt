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
 * **Backward pass** – starting from the last phase, optimality (Benders)
 *   cuts are generated from the reduced costs of the dependent state
 *   variables and added to the previous phase's LP.  An elastic filter
 *   ensures feasibility when the trial point from the forward pass would
 *   otherwise make the downstream LP infeasible.  Feasibility issues
 *   propagate backward iteratively: if adding a cut makes phase k
 *   infeasible, the solver builds a feasibility cut for phase k-1, and
 *   continues all the way to phase 0 if necessary.
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
#include <string>
#include <vector>

#include <gtopt/aperture.hpp>
#include <gtopt/aperture_data_cache.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/error.hpp>
#include <gtopt/iteration_lp.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_method.hpp>
#include <gtopt/sddp_aperture.hpp>
#include <gtopt/sddp_clone_pool.hpp>
#include <gtopt/sddp_cut_sharing.hpp>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/sddp_pool.hpp>
#include <gtopt/sddp_scene_tracker.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/solver_monitor.hpp>
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

  // ── Live query (thread-safe, atomic reads) ──

  /// Current iteration number (0 before first iteration completes)
  [[nodiscard]] int current_iteration() const noexcept
  {
    return m_current_iteration_.load();
  }

  /// Current relative convergence gap
  [[nodiscard]] double current_gap() const noexcept
  {
    return m_current_gap_.load();
  }

  /// Current lower bound (phase-0 objective including α)
  [[nodiscard]] double current_lower_bound() const noexcept
  {
    return m_current_lb_.load();
  }

  /// Current upper bound (sum of actual phase costs)
  [[nodiscard]] double current_upper_bound() const noexcept
  {
    return m_current_ub_.load();
  }

  /// Whether the solver has converged
  [[nodiscard]] bool has_converged() const noexcept
  {
    return m_converged_.load();
  }

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
  /// Used by cascade cut inheritance to resolve @alpha columns.
  [[nodiscard]] constexpr auto& all_scene_phase_states() const noexcept
  {
    return m_scene_phase_states_;
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
  void forget_first_cuts(int count);

  /// Update dual values of stored cuts from the current LP solution.
  /// Call after the solver finishes to populate the dual field in each
  /// StoredCut with the row dual from the last forward-pass solve.
  void update_stored_cut_duals();

  /// All stored cuts (for persistence / inspection)
  [[nodiscard]] const auto& stored_cuts() const noexcept
  {
    return m_cut_store_.stored_cuts();
  }

  /// Number of stored cuts (thread-safe).
  /// In single_cut_storage mode, counts across all per-scene vectors.
  [[nodiscard]] int num_stored_cuts() const noexcept
  {
    return m_cut_store_.num_stored_cuts(m_options_.single_cut_storage);
  }

  /// Access the cut store (for cascade orchestration, etc.).
  [[nodiscard]] SDDPCutStore& cut_store() noexcept { return m_cut_store_; }

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

  /// Load named-variable cuts from a CSV file with a `phase` column.
  ///
  /// Unlike `load_boundary_cuts()` (which loads into the last phase only),
  /// this method resolves named state-variable headers in each specified
  /// phase and adds the cuts to the corresponding phase LP.  The CSV
  /// format is:
  ///   name,iteration,scene,phase,rhs,StateVar1,StateVar2,...
  ///
  /// This is used for hot-start from PLP planos data where cuts span
  /// multiple stages (mapped to gtopt phases).
  ///
  /// @return CutLoadResult with count and max iteration, or an error.
  [[nodiscard]] auto load_named_cuts(const std::string& filepath)
      -> std::expected<CutLoadResult, Error>;

  /// Save state variable column solutions and reduced costs to a CSV file.
  /// Writes one row per column with its name, phase, scene, value, and
  /// reduced cost.  Saved alongside cuts for hot-start state restoration.
  [[nodiscard]] auto save_state(const std::string& filepath) const
      -> std::expected<void, Error>;

  /// Load state variable column solutions from a CSV file.
  /// Sets the warm column solution on each phase's LinearInterface so
  /// that physical_eini/physical_efin return loaded values before the
  /// first solve.
  [[nodiscard]] auto load_state(const std::string& filepath)
      -> std::expected<void, Error>;

  /// Get the global max kappa across all (scene, phase) LP solves.
  [[nodiscard]] double global_max_kappa() const noexcept
  {
    double gmax = -1.0;
    for (const auto& phase_kappas : m_max_kappa_) {
      for (const auto k : phase_kappas) {
        if (k >= 0.0) {
          gmax = std::max(gmax, k);
        }
      }
    }
    return gmax;
  }

private:
  using scene_phase_states_t =
      StrongIndexVector<SceneIndex,
                        StrongIndexVector<PhaseIndex, PhaseStateInfo>>;

  /// Type alias for backward compatibility — now uses the public
  /// ElasticSolveResult from benders_cut.hpp.
  using ElasticResult = ElasticSolveResult;

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
  void capture_state_variable_values(
      SceneIndex scene_index,
      PhaseIndex phase_index,
      std::span<const double> col_sol,
      std::span<const double> reduced_costs) const noexcept;

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
  [[nodiscard]] auto backward_pass_with_apertures(
      SceneIndex scene_index,
      const SolverOptions& opts,
      IterationIndex iteration_index = {}) -> std::expected<int, Error>;

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
    if (kappa >= 0.0) {
      m_max_kappa_[scene_index][phase_index] =
          std::max(m_max_kappa_[scene_index][phase_index], kappa);
    } else if (m_max_kappa_[scene_index][phase_index] < 0.0) {
      m_max_kappa_[scene_index][phase_index] = kappa;
    }
  }

  /// Check whether update_lp should be dispatched for this iteration.
  /// Returns false when the iteration is explicitly disabled or skipped
  /// by the global skip count.
  [[nodiscard]] bool should_dispatch_update_lp(
      IterationIndex iteration_index) const;

  /// Run update_lp for a single phase, setting prev_phase_sys for
  /// cross-phase physical_eini lookup.  Returns the number of updated
  /// LP elements.
  int update_lp_for_phase(SceneIndex scene_index, PhaseIndex phase_index);

  /// Conditionally dispatch update_lp for all phases in a scene.
  /// Checks the preallocated iteration vector for explicit skip/force
  /// flags and the global skip count before calling SystemLP::update_lp()
  /// on each phase.
  void dispatch_update_lp(SceneIndex scene_index,
                          IterationIndex iteration_index);

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

  /// Iterative feasibility backpropagation: propagate from start_phase_index
  /// backward to phase 0 using elastic filter and cuts.
  /// Returns the number of additional cuts added.
  [[nodiscard]] auto feasibility_backpropagate(SceneIndex scene_index,
                                               PhaseIndex start_phase_index,
                                               int total_cuts,
                                               const SolverOptions& opts,
                                               IterationIndex iteration_index)
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
                                             IterationIndex iteration_index)
      -> ApertureSubmitFunc;

  /// Prune inactive cuts from all (scene, phase) LPs.
  /// Removes cuts whose |dual| < prune_dual_threshold, keeping at most
  /// max_cuts_per_phase active cuts per LP.  Only LP rows are removed;
  /// m_stored_cuts_ and m_scene_cuts_ are preserved for persistence.
  void prune_inactive_cuts();

  /// Cap per-scene stored cuts to max_stored_cuts (oldest dropped first).
  void cap_stored_cuts();

  /// Build combined stored cuts from per-scene vectors (for persistence).
  [[nodiscard]] std::vector<StoredCut> build_combined_cuts() const;

  /// Get a pooled clone pointer for aperture solves (nullptr if pool disabled).
  LinearInterface* get_pooled_clone_ptr(SceneIndex scene_index,
                                        PhaseIndex phase_index)
  {
    if (!m_options_.use_clone_pool || !m_clone_pool_.is_allocated()) {
      return nullptr;
    }
    return m_clone_pool_.get_ptr(
        scene_index,
        phase_index,
        planning_lp(),
        m_scene_phase_states_[scene_index][phase_index].base_nrows);
  }

  /// Check whether the sentinel file exists (user-requested stop)
  [[nodiscard]] bool check_sentinel_stop() const;

  /// Check whether the monitoring API stop-request file exists
  [[nodiscard]] bool check_api_stop_request() const;

  /// Check all stop conditions: sentinel file, API stop request, programmatic
  /// stop, callback
  [[nodiscard]] bool should_stop() const;

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
  [[nodiscard]] auto backward_pass_with_apertures_single_phase(
      SceneIndex scene_index,
      PhaseIndex phase_index,
      int cut_offset,
      const SolverOptions& opts,
      IterationIndex iteration_index) -> std::expected<int, Error>;

  /// Implementation helper for aperture per-phase backward step.
  /// Used by backward_pass_with_apertures_single_phase.
  [[nodiscard]] auto backward_pass_aperture_phase_impl(
      SceneIndex scene_index,
      PhaseIndex phase_index,
      int cut_offset,
      const ScenarioLP& base_scenario,
      std::span<const ScenarioLP> all_scenarios,
      std::span<const Aperture> aperture_defs,
      const SolverOptions& opts,
      IterationIndex iteration_index) -> std::expected<int, Error>;

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

  /// Compute and fill ir.upper_bound, ir.lower_bound, ir.scene_lower_bounds.
  void compute_iteration_bounds(SDDPIterationResult& ir,
                                std::span<const uint8_t> scene_feasible,
                                std::span<const double> weights) const;

  /// Apply cut-sharing across scenes for all phases generated in this
  /// iteration.
  /// @param cuts_before  Value of m_stored_cuts_.size() BEFORE this
  ///                     iteration's backward pass.
  /// @param iteration    Current SDDP iteration index.
  void apply_cut_sharing_for_iteration(std::size_t cuts_before,
                                       IterationIndex iteration_index);

  /// Compute gap, update convergence flag, update live-query atomics, log.
  void finalize_iteration_result(SDDPIterationResult& ir,
                                 IterationIndex iteration_index);

  /// Write the monitoring API status file if API is enabled.
  void maybe_write_api_status(const std::string& status_file,
                              const std::vector<SDDPIterationResult>& results,
                              std::chrono::steady_clock::time_point solve_start,
                              const SolverMonitor& monitor) const;

  /// Save cuts (combined + per-scene) after an iteration, handling infeasible
  /// scene renaming.
  void save_cuts_for_iteration(IterationIndex iteration_index,
                               std::span<const uint8_t> scene_feasible);

  // Accessor for the wrapped PlanningLP reference (avoids raw reference member)
  [[nodiscard]] PlanningLP& planning_lp() noexcept
  {
    return m_planning_lp_.get();
  }
  [[nodiscard]] const PlanningLP& planning_lp() const noexcept
  {
    return m_planning_lp_.get();
  }

  /// Get the scene UID for a given SceneIndex.
  [[nodiscard]] SceneUid scene_uid(SceneIndex si) const noexcept
  {
    return planning_lp().simulation().scenes()[si].uid();
  }

  /// Get the phase UID for a given PhaseIndex.
  [[nodiscard]] PhaseUid phase_uid(PhaseIndex pi) const noexcept
  {
    return planning_lp().simulation().phases()[pi].uid();
  }

  std::reference_wrapper<PlanningLP> m_planning_lp_;
  SDDPOptions m_options_;
  ApertureDataCache m_aperture_cache_;
  LabelMaker m_label_maker_;
  scene_phase_states_t m_scene_phase_states_;
  SDDPCutStore m_cut_store_;

  /// Clone pool: one cached LinearInterface per (scene, phase) for aperture
  /// reuse.  Empty when use_clone_pool is false.
  SDDPClonePool m_clone_pool_ {};

  /// Per-(scene, phase) count of consecutive forward-pass infeasibilities.
  /// Incremented when the elastic filter is used in forward_pass at (scene,
  /// phase).  Reset to 0 when the phase is solved normally (no elastic).
  /// Used by the backward pass to decide single_cut vs multi_cut mode.
  StrongIndexVector<SceneIndex, StrongIndexVector<PhaseIndex, int>>
      m_infeasibility_counter_;

  /// Per-(scene, phase) maximum kappa (condition number) across all LP
  /// solves (forward, backward, aperture).  Updated after every solve call.
  StrongIndexVector<SceneIndex, StrongIndexVector<PhaseIndex, double>>
      m_max_kappa_;

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

  // ── Stop / callback machinery ──
  SDDPIterationCallback m_iteration_callback_ {};
  std::atomic<bool> m_stop_requested_ {false};
  /// When true, should_stop() returns false (simulation pass ignores stops).
  bool m_in_simulation_ {false};

  // ── Atomic live-query state ──
  std::atomic<int> m_current_iteration_ {0};
  std::atomic<double> m_current_gap_ {1.0};
  std::atomic<double> m_current_lb_ {0.0};
  std::atomic<double> m_current_ub_ {0.0};
  std::atomic<bool> m_converged_ {false};
  std::atomic<int> m_current_pass_ {0};  ///< 0=idle, 1=forward, 2=backward
  std::atomic<int> m_scenes_done_ {0};  ///< Scenes completed in current pass
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

  /// Non-owning pointer to the auxiliary AdaptiveWorkPool used by
  /// BendersCut (elastic LP solves) and LpDebugWriter (gzip compression).
  /// Separate from m_pool_ so that those helpers continue to use the
  /// simple int64_t key while the main SDDP LP solves use SDDPTaskKey.
  AdaptiveWorkPool* m_aux_pool_ {nullptr};

  /// LP debug writer — active when lp_debug is enabled and log_directory is
  /// set.  Initialised at the start of solve() and drained at the end.
  LpDebugWriter m_lp_debug_writer_ {};
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
