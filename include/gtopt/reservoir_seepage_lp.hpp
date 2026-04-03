/**
 * @file      reservoir_seepage_lp.hpp
 * @brief     Linear Programming representation of a ReservoirSeepage system
 * @date      Thu Jul 31 01:49:05 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The ReservoirSeepageLP class provides a linear programming (LP) compatible
 * representation of a ReservoirSeepage system for optimization problems.
 *
 * Per-stage slope/constant schedules (from plpmanfi.dat Parquet files)
 * are read at construction time and applied directly to the LP matrix
 * coefficients and RHS during add_to_lp() — analogous to how
 * ReservoirProductionFactor updates the turbine conversion-rate coefficient.
 *
 * When the ReservoirSeepage has piecewise-linear segments, update_lp() selects
 * the active segment based on the reservoir's current volume (vini from
 * the previous phase) and updates the LP constraint coefficients and RHS.
 */

#pragma once

#include <gtopt/reservoir_lp.hpp>
#include <gtopt/reservoir_seepage.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/sddp_common.hpp>
#include <gtopt/waterway_lp.hpp>

namespace gtopt
{

// Forward declaration to avoid circular includes (system_lp.hpp includes
// reservoir_seepage_lp.hpp).
class SystemLP;

/**
 * @brief LP wrapper for ReservoirSeepage systems
 *
 * Provides methods for LP formulation of seepage constraints while
 * maintaining connections to waterways and reservoirs.
 *
 * Per-stage slope/constant schedules are applied directly as LP matrix
 * coefficients during add_to_lp() for each stage.  When the ReservoirSeepage
 * also has piecewise-linear segments, update_lp() is called by the SDDP
 * solver (or monolithic solver between phases) to re-evaluate the
 * seepage function at the current reservoir volume and override the
 * constraint coefficients.
 */
class ReservoirSeepageLP : public ObjectLP<ReservoirSeepage>
{
public:
  static constexpr LPClassName ClassName {"ReservoirSeepage", "fil"};

  /// Constructs a ReservoirSeepageLP from a ReservoirSeepage and input context.
  /// Initialises per-stage slope/constant schedules from the seepage
  /// object's `slope` and `constant` FieldSched fields.
  explicit ReservoirSeepageLP(const ReservoirSeepage& pseepage,
                              InputContext& ic);

  [[nodiscard]] constexpr auto&& seepage(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto reservoir_sid() const noexcept
  {
    return ReservoirLPSId {seepage().reservoir};
  }
  [[nodiscard]] constexpr auto waterway_sid() const noexcept
  {
    return WaterwayLPSId {seepage().waterway};
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /**
   * @brief Get seepage variable columns for a scenario/stage combination
   *
   * Returns the column indices for the seepage flow variables.  These
   * reference the waterway flow columns associated with this seepage.
   */
  [[nodiscard]] const auto& seepage_cols_at(const ScenarioLP& scenario,
                                            const StageLP& stage) const
  {
    return seepage_cols.at({scenario.uid(), stage.uid()});
  }

  /**
   * @brief Update reservoir-dependent LP coefficients for this seepage.
   *
   * When the ReservoirSeepage has piecewise-linear segments, selects the active
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
                              const StageLP& stage);

  /// Tracks the volume columns and current LP state for coefficient updates
  struct ReservoirSeepageState
  {
    ColIndex eini_col {};  ///< Stage eini column
    ColIndex efin_col {};  ///< Stage efin column
    Real current_slope {0.0};  ///< Current physical slope in the LP constraint
    Real current_rhs {0.0};  ///< Current RHS in the LP constraint
  };

private:
  STBIndexHolder<ColIndex> seepage_cols;
  STBIndexHolder<RowIndex> seepage_rows;

  /// Per-stage slope schedule (from ReservoirSeepage::slope FieldSched)
  OptTRealSched m_slope_sched_;
  /// Per-stage constant schedule (from ReservoirSeepage::constant FieldSched)
  OptTRealSched m_constant_sched_;

  /// Per-(scenario, stage) state for coefficient tracking
  IndexHolder2<ScenarioUid, StageUid, ReservoirSeepageState> m_states_;
};

}  // namespace gtopt
