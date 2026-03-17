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
 * Per-stage slope/constant schedules (from plpmanfi.dat Parquet files)
 * are read at construction time and applied directly to the LP matrix
 * coefficients and RHS during add_to_lp() — analogous to how
 * ReservoirEfficiency updates the turbine conversion-rate coefficient.
 *
 * When the Filtration has piecewise-linear segments, update_lp() selects
 * the active segment based on the reservoir's current volume (vini from
 * the previous phase) and updates the LP constraint coefficients and RHS.
 */

#pragma once

#include <gtopt/filtration.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/schedule.hpp>
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
 * Per-stage slope/constant schedules are applied directly as LP matrix
 * coefficients during add_to_lp() for each stage.  When the Filtration
 * also has piecewise-linear segments, update_lp() is called by the SDDP
 * solver (or monolithic solver between phases) to re-evaluate the
 * filtration function at the current reservoir volume and override the
 * constraint coefficients.
 */
class FiltrationLP : public ObjectLP<Filtration>
{
public:
  static constexpr LPClassName ClassName {"Filtration", "fil"};

  /// Constructs a FiltrationLP from a Filtration and input context.
  /// Initialises per-stage slope/constant schedules from the filtration
  /// object's `slope` and `constant` FieldSched fields.
  explicit FiltrationLP(const Filtration& pfiltration, InputContext& ic);

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

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /**
   * @brief Get filtration variable columns for a scenario/stage combination
   *
   * Returns the column indices for the filtration flow variables.  These
   * reference the waterway flow columns associated with this filtration.
   */
  [[nodiscard]] const auto& filtration_cols_at(const ScenarioLP& scenario,
                                               const StageLP& stage) const
  {
    return filtration_cols.at({scenario.uid(), stage.uid()});
  }

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
    double vol_scale {1.0};  ///< Reservoir volume scale factor
    Real current_slope {0.0};  ///< Current physical slope in the LP constraint
    Real current_rhs {0.0};  ///< Current RHS in the LP constraint
  };

private:
  STBIndexHolder<ColIndex> filtration_cols;
  STBIndexHolder<RowIndex> filtration_rows;

  /// Per-stage slope schedule (from Filtration::slope FieldSched)
  OptTRealSched m_slope_sched_;
  /// Per-stage constant schedule (from Filtration::constant FieldSched)
  OptTRealSched m_constant_sched_;

  /// Per-(scenario, stage) state for coefficient tracking
  IndexHolder2<ScenarioUid, StageUid, FiltrationState> m_states_;
};

}  // namespace gtopt
