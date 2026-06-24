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

// ─── Per-level statistics ────────────────────────────────────────────────────

/// Statistics collected for each cascade level after solving.
struct CascadeLevelStats
{
  std::string name {};  ///< Level name (from config or auto-generated)
  int iterations {};  ///< Number of SDDP iterations (excludes final fwd pass)
  double lower_bound {};  ///< Final lower bound
  double upper_bound {};  ///< Final upper bound
  double gap {};  ///< Final relative gap
  double gap_initial {};  ///< First training iter's gap (level entry gap)
  double gap_delta {};  ///< `gap − gap_initial` (signed; negative = closed)
  bool have_gap_delta {false};  ///< False when the level produced ≤1 iter
                                ///< (then `gap_initial` / `gap_delta` are
                                ///< undefined and the per-level / summary
                                ///< logs must suppress them).
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
// ``final`` removed (2026-05): unit tests in
// ``test/source/test_cascade_method.cpp::TestableCascade`` derive from
// this class to wrap the protected ``build_level_sddp_opts`` helper
// and pin the 3-layer priority chain (base SDDPOptions →
// cascade-global → per-level override).  Keep the class behaviour
// the same — derived overrides are not part of any production path.
class CascadePlanningMethod : public PlanningMethod
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

  /// Number of PlanningLPs owned by the cascade solver.  One entry is
  /// added per level that required a fresh LP build; level 0 is skipped
  /// when the caller-supplied PlanningLP is reused (no model-option
  /// overrides from cascade globals or level 0).
  [[nodiscard]] std::size_t owned_lps_count() const noexcept
  {
    return m_owned_lps_.size();
  }

protected:
  /// Build SDDPOptions for a level, overriding base with level solver opts.
  /// @param level_solver      Per-level solver configuration (may be absent).
  /// @param remaining_budget  Global iteration budget remaining (-1 = no cap).
  ///
  /// ``protected`` (not ``private``) so unit-test subclasses can wrap
  /// it via a public ``test_build_level_sddp_opts`` shim and pin the
  /// 3-layer priority chain (base → cascade-global → per-level) —
  /// see ``test/source/test_cascade_method.cpp::TestableCascade``.
  [[nodiscard]] auto build_level_sddp_opts(
      const std::optional<CascadeLevelMethod>& level_solver,
      int remaining_budget) const -> SDDPOptions;

private:
  /// Clone Planning data with model option overrides applied.
  [[nodiscard]] static auto clone_planning_with_overrides(
      const Planning& source, const ModelOptions& model_opts) -> Planning;

  /// Clear all cut rows (>= base_nrows) from every (scene, phase) LP.
  static void clear_all_cuts(PlanningLP& planning_lp, const SDDPMethod& solver);

  SDDPOptions m_base_opts_;
  CascadeOptions m_cascade_opts_;
  std::vector<SDDPIterationResult> m_all_results_ {};
  std::vector<CascadeLevelStats> m_level_stats_ {};
  /// Owns PlanningLPs built for levels that need different LP formulations.
  std::vector<std::unique_ptr<PlanningLP>> m_owned_lps_ {};
  /// Temp file path holding previous level's cuts, pre-filtered according
  /// to the NEXT level's transition.inherit_* settings.  Populated at end
  /// of level N so the owned LP and solver can be released before level
  /// N+1 allocates its own LP.  Consumed by the next level.
  std::string m_prev_cuts_file_ {};
};

}  // namespace gtopt
