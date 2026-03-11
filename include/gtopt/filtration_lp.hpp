/**
 * @file      filtration_lp.hpp
 * @brief     Linear Programming representation of a Filtration system
 * @date      Thu Jul 31 01:49:05 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The FiltrationLP class provides a linear programming (LP) compatible
 * representation of a Filtration system for optimization problems.
 *
 * When the Filtration has piecewise-linear segments, update_lp() selects
 * the active segment based on the reservoir's current volume (vini from
 * the previous phase) and updates the LP constraint coefficients and RHS.
 */

#pragma once

#include <gtopt/filtration.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/waterway_lp.hpp>

namespace gtopt
{

// Forward declaration to avoid circular includes (system_lp.hpp includes
// filtration_lp.hpp).
class SystemLP;

/**
 * @brief LP wrapper for Filtration systems
 *
 * Provides methods for LP formulation of filtration constraints while
 * maintaining connections to waterways and reservoirs.
 *
 * When the Filtration has piecewise-linear segments, update_lp() is
 * called by the SDDP solver (or monolithic solver between phases) to
 * re-evaluate the filtration function at the current reservoir volume
 * and update the constraint coefficients.
 */
class FiltrationLP : public ObjectLP<Filtration>
{
public:
  static constexpr LPClassName ClassName {"Filtration", "fil"};

  /// Constructs a FiltrationLP from a Filtration and input context
  /// @param pfiltration The filtration system to wrap
  /// @param ic Input context for LP construction
  [[nodiscard]]
  explicit constexpr FiltrationLP(const Filtration& pfiltration,
                                  [[maybe_unused]] InputContext& ic) noexcept
      : ObjectLP<Filtration>(pfiltration)
  {
  }

  [[nodiscard]] constexpr auto&& filtration(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto reservoir_sid() const noexcept
  {
    return ReservoirLPSId {filtration().reservoir};
  }
  [[nodiscard]] constexpr auto waterway_sid() const noexcept
  {
    return WaterwayLPSId {filtration().waterway};
  }

  [[nodiscard]] constexpr auto slope() const noexcept
  {
    return filtration().slope;
  }
  [[nodiscard]] constexpr auto constant() const noexcept
  {
    return filtration().constant;
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /**
   * @brief Update reservoir-dependent LP coefficients for this filtration.
   *
   * When the Filtration has piecewise-linear segments, selects the active
   * segment based on the reservoir's current volume (vini from previous
   * phase) and updates:
   * - The coefficient on eini/efin columns: -slope * 0.5
   * - The RHS (row bounds): intercept = constant_i - slope_i * volume_i
   *
   * Only dispatches set_coeff/set_rhs calls when the new value differs
   * from the previously stored value.
   *
   * @return Number of LP coefficients/bounds modified (0 if unchanged)
   */
  [[nodiscard]] int update_lp(SystemLP& sys,
                              const ScenarioLP& scenario,
                              const StageLP& stage,
                              PhaseIndex phase,
                              int iteration);

  /// Tracks the volume columns and current LP state for coefficient updates
  struct FiltrationState
  {
    ColIndex eini_col {};  ///< Stage eini column
    ColIndex efin_col {};  ///< Stage efin column
    Real current_slope {0.0};  ///< Current slope in the LP constraint
    Real current_rhs {0.0};  ///< Current RHS in the LP constraint
  };

private:
  STBIndexHolder<ColIndex> filtration_cols;
  STBIndexHolder<RowIndex> filtration_rows;

  /// Per-(scenario, stage) state for coefficient tracking
  IndexHolder2<ScenarioUid, StageUid, FiltrationState> m_states_;
};

}  // namespace gtopt
