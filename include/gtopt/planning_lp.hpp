/**
 * @file      planning.hpp
 * @brief     Linear programming planning for power system optimization
 * @author    marcelo
 * @date      Sun Apr  6 18:18:54 2025
 * @copyright BSD-3-Clause
 *
 * This module provides functionality for creating, solving, and analyzing
 * linear programming models for power system planning.
 */

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <gtopt/error.hpp>
#include <gtopt/lp_name_spill.hpp>  // complete type: unique_ptr member + inline ctor
#include <gtopt/planning.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/strong_index_vector.hpp>
#include <gtopt/system_lp.hpp>

namespace gtopt
{

/**
 * @class PlanningLP
 * @brief Linear programming model for power system planning
 *
 * Encapsulates a linear programming formulation of a power system planning
 * problem, including system configurations, simulation scenarios, and solver
 * options.
 */

class PlanningLP
{
public:
  using phase_systems_t = StrongIndexVector<PhaseIndex, SystemLP>;
  using scene_phase_systems_t = StrongIndexVector<SceneIndex, phase_systems_t>;

  /// Sparse store for the SDDP backward-pass aperture systems: a cell is
  /// empty unless an `aperture_system_file` resolves for that phase.  Each
  /// present cell is a `SystemLP` built with `SystemKind::aperture` from the
  /// (path-cached) reduced system, sharing the same `SimulationLP`.
  using phase_ap_systems_t =
      StrongIndexVector<PhaseIndex, std::optional<SystemLP>>;
  using scene_phase_ap_t = StrongIndexVector<SceneIndex, phase_ap_systems_t>;

private:
  static auto create_systems(System& system,
                             SimulationLP& simulation,
                             const PlanningOptionsLP& options,
                             const LpMatrixOptions& flat_opts,
                             LpNameSpillStore* name_store)
      -> scene_phase_systems_t;

  /// Construct the run-lifetime async label-metadata spill store, or return
  /// nullptr when spilling is disabled (names not kept, or monolithic — which
  /// keeps a single resident LP whose label metadata is cheap).  Rooted at a
  /// temp dir under `options.log_directory()`.  Defined out-of-line because
  /// `LpNameSpillStore` is only forward-declared here.
  [[nodiscard]] static auto make_name_store(const LpMatrixOptions& flat_opts,
                                            const PlanningOptionsLP& options)
      -> std::unique_ptr<LpNameSpillStore>;

  /// Build the aperture systems (if any) into `m_aperture_systems_`, owning
  /// the loaded reduced `System`s in `m_aperture_owned_systems_`.  Resolves
  /// each phase's file via `resolve_aperture_system_file` (Phase override →
  /// global `sddp_options`); no-op (leaves the store empty) when no file
  /// resolves or the method is monolithic.  Called from the constructor
  /// body, after `m_systems_` is built.
  void build_aperture_systems(const LpMatrixOptions& flat_opts);

  /// Resolve the deferred state-variable links recorded by every phase
  /// in `phase_systems` during its `add_to_lp` pass.  Each
  /// `PendingStateLink` names a producer `StateVariable` from a previous
  /// phase (via `prev_key`) and a dependent column added to a later
  /// phase; this pass looks the producer up in `simulation`'s registry
  /// and calls `add_dependent_variable` on it.
  ///
  /// Runs after a scene's parallel phase build joins.  Within a single
  /// scene the producer's `StateVariable` is guaranteed to exist by the
  /// time tightening starts (all phases are built), so the lookup is
  /// race-free.  Different scenes touch disjoint `StateVariable`s, so
  /// multiple scene tasks may run their tightening pass concurrently.
  ///
  /// Drains each system's `pending_state_links()` vector as it goes.
  static void tighten_scene_phase_links(phase_systems_t& phase_systems,
                                        SimulationLP& simulation);

  /// Compute adaptive scale_theta from median line reactance when not
  /// explicitly set.  Mutates `planning.options.scale_theta` in-place so
  /// that PlanningOptionsLP picks up the computed value.
  static void auto_scale_theta(Planning& planning);

  /// Compute adaptive energy/flow scales for reservoirs from emax/fmax.
  /// Injects VariableScale entries into `planning.options.variable_scales`
  /// for reservoirs that don't already have explicit entries.
  /// @p solver_infinity is queried from `SolverBackend::infinity()` so
  /// sentinel values like `fmax=1e30` (meaning "no bound") are not folded
  /// into col_scale; otherwise they would propagate into Benders cut
  /// numerics and produce LB compounding (juan/gtopt_iplp regression
  /// 2026-04-30).
  static void auto_scale_reservoirs(Planning& planning, double solver_infinity);

  /// State variable I/O uses the StateVariable map (ColIndex-based)
  /// directly — no column name strings are needed.  This method is
  /// kept for API compatibility but is now a pass-through.
  [[nodiscard]] static constexpr auto enforce_names_for_method(
      const LpMatrixOptions& opts,
      const PlanningOptionsLP& /*plp_opts*/,
      const Planning& /*planning*/) noexcept -> LpMatrixOptions
  {
    return opts;
  }

public:
  /// Validate line reactance values and clamp tiny ones to zero.
  ///
  /// Promotes any line whose effective per-unit susceptance term
  /// `|x_pu| = |X/V²|` falls below
  /// `model_options.dc_line_reactance_threshold` (default `1e-6`) to a
  /// "DC line" by rewriting its reactance schedule to scalar `0.0`.
  /// The Kirchhoff assembler in `kirchhoff_node_angle.cpp` then skips
  /// the θ-row for that line (no KVL coupling, only flow bounds).
  ///
  /// The check is dimensionally correct (susceptance, not raw `X`) so
  /// it handles both per-unit and physical-unit (kV / Ω) inputs and
  /// catches V-vs-kV unit-typo lines that a raw-`|X|` cutoff misses.
  ///
  /// Sources of `x_pu` below threshold are almost always: data-entry
  /// mistakes (V/kV typo), HVDC links / phase-shifters (`X = 0`
  /// sentinels), or short busbar segments whose KVL contribution is
  /// below solver tolerance.  In every case dropping the KVL row is
  /// the correct action — and avoids poisoning LP conditioning with
  /// outlier `x_τ` coefficients.
  ///
  /// Set `model_options.dc_line_reactance_threshold = 0` to disable
  /// the promotion entirely.  Static so it can be exercised by unit
  /// tests without standing up a full PlanningLP.
  ///
  /// Mirrors the battery input/output_efficiency clamping performed at
  /// `scripts/plp2gtopt/battery_writer.py`.
  static void validate_line_reactance(Planning& planning);

  /// Validate line resistance values and clamp tiny ones to zero.
  ///
  /// Promotes any line whose per-unit loss term `|r_pu| = |R/V²|` falls below
  /// `model_options.dc_line_resistance_threshold` (default `1e-6`) to a
  /// lossless line by rewriting its resistance schedule to scalar `0.0`.  The
  /// loss assembler in `line_losses` then takes its `R ≤ 0 → lossless` path and
  /// omits the loss-PWL (both the loss-pricing AND the `line_loss*_link` rows).
  ///
  /// The line loss-link coefficient is `∝ 1/(R/V²)`, so a very-low-R line
  /// injects the matrix's largest coefficient (e.g. 64102 for `R/V² = 5e-8`)
  /// for a negligible physical loss.  The dimensionally-correct check is
  /// `|R/V²|` (X is irrelevant — the `R·X` product would miss low-R/normal-X
  /// lines), mirroring `validate_line_reactance` on `|X/V²|`.  Catches data
  /// V-vs-kV typos, lossless connectors (`R = 0`), and transformer/busbar
  /// segments whose copper loss is below solver tolerance.
  ///
  /// Set `model_options.dc_line_resistance_threshold = 0` to disable.  Static
  /// so it can be exercised by unit tests without a full PlanningLP.
  static void validate_line_resistance(Planning& planning);

  /// Validate generator power bounds and clamp tiny ones to zero.
  ///
  /// Rewrites any `Generator::pmax` / `Generator::pmin` whose raw MW value is
  /// nonzero but below `1e-4` MW to scalar `0.0` (clamping offending entries
  /// inside 2-D vector schedules in place; `FileSched` schedules are skipped —
  /// they can't be validated statically).  Making such a degenerate bound
  /// *exactly* zero lets the LP-assembly zero-column skip eliminate the dead
  /// generation column cleanly.
  ///
  /// Unlike `validate_line_reactance` / `validate_line_resistance` the MW
  /// threshold is a fixed compile-time constant (no voltage to divide by, no
  /// model_options knob).  Static so it can be exercised by unit tests without
  /// standing up a full PlanningLP.
  static void validate_power_bounds(Planning& planning);

  /// Validate line transmission bounds and clamp tiny ones to zero.
  ///
  /// Rewrites any `Line::tmax_ab` / `Line::tmax_ba` whose raw MW value is
  /// nonzero but below `1e-4` MW to scalar `0.0` (same scalar / 2-D-vector /
  /// FileSched handling as `validate_power_bounds`).  Making the degenerate
  /// transfer limit *exactly* zero lets the LP-assembly zero-row/column skip
  /// eliminate the dead flow column / capacity row cleanly.
  ///
  /// Static so it can be exercised by unit tests without standing up a full
  /// PlanningLP.
  static void validate_line_transmission(Planning& planning);

  /// Compute adaptive `scale_loss_link` from `median(R/V²)` when not
  /// explicitly set.  Picks a power-of-10 multiplier `s = 10^round(
  /// −log10(median(R/V²)))` so the smallest segment coefficient
  /// `loss_1 = seg_width · R / V²` is lifted into the O(seg_width · 1)
  /// range in the loss-link row.  Mutates
  /// `planning.options.model_options.scale_loss_link` in-place.
  ///
  /// Static so unit tests can exercise it without standing up a full
  /// PlanningLP, mirroring `validate_line_reactance`.
  static void auto_scale_loss_link(Planning& planning);

  /// Compute adaptive energy scales for LNG terminals from emax.
  /// Same `solver_infinity` semantics as `auto_scale_reservoirs`.
  /// Static so unit tests can exercise it without standing up a
  /// full PlanningLP.
  static void auto_scale_lng_terminals(Planning& planning,
                                       double solver_infinity);

  /// Renormalise ``Scenario::probability_factor`` so that the sum
  /// over scenarios contained in *active* scenes (and themselves
  /// active) equals 1.0.  Implements the long-standing contract
  /// documented on ``Scenario`` itself ("when probability_factor
  /// values across all scenarios do not sum to 1, the solver
  /// normalises them internally").
  ///
  /// Called from the ``PlanningLP`` constructor's ``auto_scale_*``
  /// block, *before* any LP cost coefficient is computed via
  /// ``CostHelper::block_ecost``.  Mutates
  /// ``planning.simulation.scenario_array`` in-place: only the
  /// scenarios that are part of the active subproblem (active flag
  /// AND parent scene active) are rescaled.  Scenarios in inactive
  /// scenes keep their original (unused) probability_factor.
  ///
  /// Idempotent: a second call after the first leaves total
  /// probability at 1.0 and is a no-op.  Skipped silently when the
  /// total mass is already within ``1e-12`` of 1.0.  Logs INFO when
  /// a non-trivial rescale happens; WARN when every active scenario
  /// has zero / missing probability_factor (degenerate input).
  ///
  /// Static so unit tests can exercise it without standing up a
  /// full PlanningLP — mirrors ``auto_scale_*``.
  static void renormalize_scenario_probabilities(Planning& planning);

  /**
   * @brief Constructs a PlanningLP instance from planning data
   * @param planning The power system planning data
   * @param flat_opts Configuration options (default empty)
   */
  template<typename PlanningT>
    requires std::same_as<std::remove_cvref_t<PlanningT>, gtopt::Planning>
  explicit PlanningLP(PlanningT&& planning,
                      const LpMatrixOptions& flat_opts = {},
                      std::shared_ptr<ArrowIndexCache> shared_index_cache = {})
      : m_planning_(  // NOLINT
            [&]() -> decltype(auto)
            {
              if constexpr (!std::is_const_v<
                                std::remove_reference_t<PlanningT>>) {
                validate_line_reactance(planning);
                validate_line_resistance(planning);
                validate_power_bounds(planning);
                validate_line_transmission(planning);
                auto_scale_theta(planning);
                auto_scale_loss_link(planning);
                // Query the default solver's `+infinity` value from the
                // registry — fast path uses the plugin-level
                // `gtopt_solver_infinity` entry (no backend allocated);
                // older plugins without that entry fall back to a
                // throwaway instance.  auto_scale_* need this BEFORE
                // any LP is built to skip sentinel-valued bounds (e.g.
                // `Reservoir.fmax = 1e30` meaning "no flow bound")
                // that would otherwise be folded into `col_scale =
                // 1e30` and break Benders cut numerics.  See
                // juan/gtopt_iplp regression diagnosis 2026-04-30 (LB
                // compounded ~10x per iter to 1.1B vs UB ~155M).
                auto& reg = SolverRegistry::instance();
                const double solver_infinity = [&]
                {
                  if (auto v = reg.plugin_infinity(); v.has_value()) {
                    return *v;
                  }
                  // Fallback: older plugin without `gtopt_solver_infinity`
                  // entry — instantiate a throwaway backend and query.
                  auto backend = reg.create(reg.default_solver());
                  return backend->infinity();
                }();
                auto_scale_reservoirs(planning, solver_infinity);
                auto_scale_lng_terminals(planning, solver_infinity);
                // Honour the long-standing contract from Scenario.hpp:
                // "when probability_factor values across all scenarios do
                // not sum to 1, the solver normalises them internally."
                // Runs after the auto_scale_* block so the rescale is
                // computed from final scenario data; runs *before*
                // ``SimulationLP`` construction so the per-cell LP cost
                // coefficients (built via ``CostHelper::block_ecost``)
                // see the renormalised probabilities directly — both
                // ``SimulationLP::m_scenario_array_`` and each
                // ``SceneLP::m_scenarios_`` copy from the same
                // ``planning.simulation.scenario_array`` we mutate here.
                renormalize_scenario_probabilities(planning);
              }
              return std::forward<PlanningT>(planning);
            }())
      , m_options_(m_planning_.options)
      , m_simulation_(
            m_planning_.simulation, m_options_, std::move(shared_index_cache))
      , m_name_store_(make_name_store(flat_opts, m_options_))
      , m_systems_(create_systems(
            m_planning_.system,
            m_simulation_,
            m_options_,
            enforce_names_for_method(flat_opts, m_options_, m_planning_),
            m_name_store_.get()))
  {
    // Build the optional SDDP backward-pass aperture systems after the
    // forward systems exist (they share m_simulation_ and register into the
    // parallel SystemKind::aperture registry).  No-op when no
    // aperture_system_file resolves.
    build_aperture_systems(
        enforce_names_for_method(flat_opts, m_options_, m_planning_));
  }

  /**
   * @brief Gets the LP options configuration
   * @return Const reference to PlanningOptionsLP
   */
  [[nodiscard]] constexpr const PlanningOptionsLP& options() const noexcept
  {
    return m_options_;
  }

  /**
   * @brief Gets the underlying planning data
   * @return Const reference to Planning
   */
  [[nodiscard]] constexpr const Planning& planning() const noexcept
  {
    return m_planning_;
  }

  /**
   * @brief Gets the system LP representations
   * @return Const reference to vector of SystemLP
   */
  template<typename Self>
  [[nodiscard]] constexpr auto&& systems(this Self&& self) noexcept
  {
    return std::forward<Self>(self).m_systems_;
  }

  /**
   * @brief Gets the simulation LP model (const or non-const via deducing this).
   * @return Reference to SimulationLP
   */
  template<typename Self>
  [[nodiscard]] constexpr auto&& simulation(this Self&& self) noexcept
  {
    return std::forward<Self>(self).m_simulation_;
  }

  /**
   * @brief Solves the linear programming problem
   * @param lp_opts Solver options (default empty)
   * @return Expected with solution status or error message
   */
  [[nodiscard]] std::expected<int, Error> resolve(
      const SolverOptions& lp_opts = {});

  /**
   * @brief Writes the LP formulation to file
   * @param filename Output file path
   */
  void write_lp(const std::string& filename) const;

  /**
   * @brief Eagerly build every (scene, phase) LP matrix in parallel.
   *
   * Under `LowMemoryMode::compress` this reconstructs each cell from
   * its snapshot.  Under `off` it is a no-op (every backend is already
   * live from construction).
   *
   * Intended use: the `--lp-only` CLI path validates that the whole
   * planning horizon can be built, and optionally dumps every cell via
   * `--lp-file`, without ever running the SDDP iterations.  Completely
   * independent of any `SDDPMethod` instance.
   */
  void build_all_lps_eagerly();

  /**
   * @brief Writes solution output (implementation-defined destination).
   *
   * When a write-out delegate has been installed via
   * `set_output_delegate()` (used by the cascade solver to hand back
   * the final level's LP), this call is forwarded to the delegate and
   * writes the delegate's systems, not this instance's.  Without the
   * delegate, the cascade's mid-loop `release_cells()` would leave
   * the caller PlanningLP with an empty system grid and the final
   * level's LP would die unused.
   */
  void write_out();

  /**
   * @brief Emit output for a single (scene, phase) cell.
   *
   * Contains the fast-path dispatch logic that was previously the
   * `emit_cell` lambda inside `write_out()`:
   *   0. output_skipped → no-op
   *   A. output_written → drop snapshot, no re-write
   *   B. low_memory != off + backend released + optimal → write from
   *      cached solution vectors without re-solve
   *   generic → ensure_lp_built, write_out, release_backend
   *
   * Exposed as a public static method so unit tests can verify each
   * fast path in isolation without constructing a full PlanningLP
   * grid.
   */
  static void write_out_cell(SystemLP& system);

  /**
   * @brief Diagnostic: log the resident-memory breakdown across all cells.
   *
   * Read-only.  Sums the per-cell LP retainers — compressed flat-LP
   * snapshots, cached solution vectors, accumulated cuts — and counts how
   * many cells currently hold their XLP collection wrappers
   * (`SystemLP::collections_resident()`), printed alongside the live
   * process RSS.  Lets us attribute the resident floor (compressed LP vs
   * cache vs collection wrappers) without guessing.  Only invoked when the
   * env var `GTOPT_LOG_LP_MEMORY` is set, so normal runs are unaffected.
   *
   * @param label  short context tag (e.g. "post-build", "post-forward").
   */
  void log_lp_memory_breakdown(std::string_view label) const;

  /**
   * @brief Install a surrogate PlanningLP whose systems provide the
   *        solution data for `write_out()`.
   *
   * Intended for the cascade solver: the final-level LP lives in the
   * `CascadePlanningMethod` owned-LPs vector, which is destroyed when
   * `PlanningLP::resolve()` returns.  Before returning, the cascade
   * transfers ownership of the final level's LP here so that its
   * populated systems remain reachable for output writing.
   *
   * The delegate is destroyed when this PlanningLP is destroyed (the
   * destructor is defined out-of-line so the incomplete-type
   * `unique_ptr<PlanningLP>` member compiles cleanly).
   */
  void set_output_delegate(std::unique_ptr<PlanningLP> delegate) noexcept;

  /**
   * @brief Accessor for an installed output delegate.
   *
   * Returns the raw pointer (non-owning) to the LP whose systems are
   * forwarded to by `write_out()` and by the post-solve aggregation
   * helpers in `gtopt_lp_runner.cpp` (the solver-stats summary needs
   * to walk the delegate's grid when this instance was cleared by the
   * cascade level transition).  Returns `nullptr` when no delegate
   * has been installed (the common monolithic / non-cascade case).
   */
  [[nodiscard]] auto output_delegate() const noexcept -> const PlanningLP*
  {
    return m_output_delegate_.get();
  }

  /**
   * @brief Release the per-(scene, phase) LP cells and their solver
   *        backends, freeing the bulk of this object's memory.
   *
   * After this call `systems()` returns an empty container: `write_out()`,
   * `resolve()`, and `aggregate_solver_stats()` become no-ops on this
   * instance.  The `Planning`, `PlanningOptionsLP`, and `SimulationLP`
   * members remain intact so the shell can still be queried for
   * configuration or used as input to a fresh `PlanningLP` built from
   * `planning()`.
   *
   * Intended for multi-level cascade solvers that reused the caller's
   * LP at an early level and want to drop its memory before the next
   * level allocates its own grid of LP matrices.
   */
  void release_cells();

  /**
   * @brief Discard every cell's compressed flat-LP snapshot, keeping
   *        only the Phase-2a cache (col_sol / col_cost / row_dual +
   *        cached scalars) on each `LinearInterface`.
   *
   * Called at the end of `SDDPMethod::simulation_pass`, after Pass 1's
   * retry loop has converged and no further backend reconstruct is
   * possible.  `PlanningLP::write_out` reads solution values from the
   * cache and rebuilds XLP col indices via a flatten of the live
   * `System` element arrays — it does NOT need the snapshot.  Dropping
   * the snapshot here frees tens of MB of compressed LP matrix per
   * cell (hundreds of MB per scene × N scenes), which on juan-scale
   * runs is a meaningful chunk of RAM headroom for the parallel
   * write_out pool.
   *
   * No-op under `low_memory = off` (no snapshot is installed in that
   * mode).  Safe to call multiple times (idempotent).
   */
  void drop_sim_snapshots() noexcept;

  // ── SDDP solve summary ──────────────────────────────────────────────────

  /**
   * @brief Summary of the last SDDP solve (populated by the SDDP solver).
   *
   * Carries the final-iteration gap metrics so that `write_out()` can emit
   * them as extra columns in `solution.csv`.  For monolithic solves this
   * struct is left at its default (all-zero / false) values.
   */
  struct SddpSummary
  {
    double gap {0.0};  ///< Final primary gap (UB−LB)/max(1,|UB|)
    double gap_change {1.0};  ///< Final stationary gap-change metric
    double lower_bound {0.0};  ///< Final lower bound
    double upper_bound {0.0};  ///< Final upper bound
    double max_kappa {-1.0};  ///< Global max condition number (-1 = unknown)
    std::ptrdiff_t iterations {0};  ///< Number of training iterations completed
    bool converged {false};  ///< True if any convergence criterion was met
    bool stationary_converged {
        false,
    };  ///< True if stationary criterion triggered convergence
    bool statistical_converged {
        false,
    };  ///< True if CI-based statistical criterion triggered convergence
  };

  /** @brief Populate the SDDP summary (called by the SDDP solver). */
  void set_sddp_summary(SddpSummary summary) noexcept
  {
    m_sddp_summary_ = summary;
  }

  /** @brief Read the SDDP summary (populated after a successful SDDP solve). */
  [[nodiscard]] const SddpSummary& sddp_summary() const noexcept
  {
    return m_sddp_summary_;
  }

  template<typename Self>
  [[nodiscard]] constexpr auto&& system(this Self&& self,
                                        SceneIndex scene_index,
                                        PhaseIndex phase_index) noexcept
  {
    return std::forward<Self>(self).m_systems_[scene_index][phase_index];
  }

  /// The aperture (backward-pass) system for a cell, or `nullptr` when this
  /// cell has no aperture system — in which case the backward pass falls
  /// back to the regular forward `system(scene, phase)`.
  [[nodiscard]] auto aperture_system(SceneIndex scene_index,
                                     PhaseIndex phase_index) noexcept
      -> SystemLP*
  {
    if (m_aperture_systems_.empty()) {
      return nullptr;
    }
    auto& cell = m_aperture_systems_[scene_index][phase_index];
    return cell.has_value() ? &*cell : nullptr;
  }

private:
  [[nodiscard]] std::expected<void, Error> resolve_scene_phases(
      SceneIndex scene_index,
      phase_systems_t& phase_systems,
      const SolverOptions& lp_opts);

  friend class MonolithicMethod;

  Planning m_planning_;
  PlanningOptionsLP m_options_;
  SimulationLP m_simulation_;

  /// Run-lifetime async store that holds each cell's spilled label metadata
  /// off-heap (Parquet) when names are kept under low-memory.  Declared BEFORE
  /// `m_systems_` so it is constructed first: `create_systems` spills into it
  /// during `m_systems_`' member-init.  Null when spilling is disabled.  Its
  /// destructor joins the worker and removes the temp dir.
  std::unique_ptr<LpNameSpillStore> m_name_store_;

  scene_phase_systems_t m_systems_;

  /// Reduced `System`s loaded from `aperture_system_file`s, owned here so
  /// they outlive the aperture `SystemLP`s in `m_aperture_systems_` (which
  /// hold `const System&` references).  Keyed by resolved file path so a
  /// file shared by several phases is loaded and preprocessed once.
  /// Declared before `m_aperture_systems_` → destroyed after it.
  std::unordered_map<std::string, System> m_aperture_owned_systems_;

  /// Sparse aperture-system store (empty unless an aperture_system_file is
  /// in effect).  Declared after `m_aperture_owned_systems_` so its
  /// `SystemLP`s (which reference those Systems and `m_simulation_`) are
  /// destroyed first.
  scene_phase_ap_t m_aperture_systems_;

  SddpSummary m_sddp_summary_ {};

  /// Surrogate PlanningLP whose systems provide `write_out()` data
  /// when set (see `set_output_delegate`).  nullptr in the common
  /// path; populated by cascade at end of `resolve()` when the final
  /// level built its own LP.  Declared last so it is destroyed first,
  /// before this instance's simulation/options it may reference.
  std::unique_ptr<PlanningLP> m_output_delegate_ {};

public:
  /// Out-of-line to allow `std::unique_ptr<PlanningLP>` as a member
  /// of `PlanningLP` itself — requires the type to be complete at the
  /// definition point of every special member, which is the `.cpp`.
  PlanningLP(const PlanningLP&) = delete;
  PlanningLP& operator=(const PlanningLP&) = delete;
  PlanningLP(PlanningLP&&) = delete;
  PlanningLP& operator=(PlanningLP&&) = delete;
  ~PlanningLP() noexcept;
};

}  // namespace gtopt
