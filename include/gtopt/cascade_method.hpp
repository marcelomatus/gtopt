/**
 * @file      cascade_method.hpp
 * @brief     Multi-level cascade method with configurable LP formulations
 * @date      2026-03-22
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements a multi-level hybrid algorithm where each level can have a
 * different LP formulation (single bus, transport, full network) and solver
 * configuration (Benders vs SDDP, iteration limits, tolerances).
 *
 * Between levels, state variable values and/or named cuts are transferred
 * to guide the next level's solution trajectory.  When a level's LP options
 * differ from the previous level, a new PlanningLP is built automatically.
 * When LP options are absent, the previous LP and solver are reused.
 *
 * When no levels are specified (empty CascadeOptions), the solver
 * behaves as a single level using the base SDDP options and the
 * caller's PlanningLP directly.
 */

#pragma once

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/planning_method.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/sddp_method.hpp>

namespace gtopt
{

// ─── State variable target ──────────────────────────────────────────────────

/// State variable target for cross-LP transfer (using structured key).
/// Matches by (class_name, col_name, uid) — no LP name strings needed.
struct StateTarget
{
  std::string class_name {};  ///< Element class (e.g. "Reservoir")
  std::string col_name {};  ///< Variable name (e.g. "efin")
  Uid uid {unknown_uid};  ///< Element UID
  LpContext context {};  ///< LP hierarchy context
  SceneIndex scene_index {};
  PhaseIndex phase_index {};
  double target_value {};  ///< Value from previous level's forward pass
  double var_scale {1.0};  ///< Physical-to-LP scale
};

// ─── Per-level statistics ────────────────────────────────────────────────────

/// Statistics collected for each cascade level after solving.
struct CascadeLevelStats
{
  std::string name {};  ///< Level name (from config or auto-generated)
  int iterations {};  ///< Number of SDDP iterations (excludes final fwd pass)
  double lower_bound {};  ///< Final lower bound
  double upper_bound {};  ///< Final upper bound
  double gap {};  ///< Final relative gap
  bool converged {};  ///< Whether convergence tolerance was met
  double elapsed_s {};  ///< Wall-clock time for this level
  int cuts_added {};  ///< Total cuts added across all iterations
};

// ─── CascadePlanningMethod ──────────────────────────────────────────────────

/**
 * @class CascadePlanningMethod
 * @brief Multi-level solver with configurable LP formulations per level
 *
 * Each level can specify LP construction options, solver parameters,
 * and transition rules.  The solver automatically rebuilds the PlanningLP
 * when a level's LP options differ from the previous level.
 */
class CascadePlanningMethod final : public PlanningMethod
{
public:
  explicit CascadePlanningMethod(SDDPOptions base_opts,
                                 CascadeOptions cascade_opts) noexcept;

  [[nodiscard]] auto solve(PlanningLP& planning_lp, const SolverOptions& opts)
      -> std::expected<int, Error> override;

  /// Access all iteration results across all cascade levels.
  [[nodiscard]] const auto& all_results() const noexcept
  {
    return m_all_results_;
  }

  /// Access per-level statistics (populated after solve).
  [[nodiscard]] const auto& level_stats() const noexcept
  {
    return m_level_stats_;
  }

private:
  /// Build SDDPOptions for a level, overriding base with level solver opts.
  /// @param level_solver      Per-level solver configuration (may be absent).
  /// @param remaining_budget  Global iteration budget remaining (-1 = no cap).
  [[nodiscard]] auto build_level_sddp_opts(
      const std::optional<CascadeLevelMethod>& level_solver,
      int remaining_budget) const -> SDDPOptions;

  /// Clone Planning data with model option overrides applied.
  [[nodiscard]] static auto clone_planning_with_overrides(
      const Planning& source, const ModelOptions& model_opts) -> Planning;

  /// Collect state variable targets from a solved level.
  [[nodiscard]] static auto collect_state_targets(const SDDPMethod& solver,
                                                  const PlanningLP& planning_lp)
      -> std::vector<StateTarget>;

  /// Add elastic target constraints to a PlanningLP using state targets.
  static void add_elastic_targets(PlanningLP& planning_lp,
                                  const std::vector<StateTarget>& targets,
                                  const CascadeTransition& transition);

  /// Clear all cut rows (>= base_nrows) from every (scene, phase) LP.
  static void clear_all_cuts(PlanningLP& planning_lp, const SDDPMethod& solver);

  SDDPOptions m_base_opts_;
  CascadeOptions m_cascade_opts_;
  std::vector<SDDPIterationResult> m_all_results_ {};
  std::vector<CascadeLevelStats> m_level_stats_ {};
  /// Owns PlanningLPs built for levels that need different LP formulations.
  std::vector<std::unique_ptr<PlanningLP>> m_owned_lps_ {};
  /// Temp file path holding previous level's state variable solutions.
  /// Populated after each non-final level solves; consumed by the next level
  /// via name-based load_state_csv to seed warm column solutions.
  std::string m_prev_state_file_ {};
};

}  // namespace gtopt
