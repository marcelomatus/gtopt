/**
 * @file      reservoir_discharge_limit_lp.hpp
 * @brief     LP representation of a ReservoirDischargeLimit constraint
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The ReservoirDischargeLimitLP class provides the LP formulation for
 * volume-dependent discharge limits.  For each (scenario, stage) pair it
 * creates:
 *
 * 1. A free `qeh` variable (stage-average hourly discharge [m³/s])
 * 2. Per-block averaging constraints linking block flows to `qeh`:
 *    `qeh - (dur_b / dur_stage) × flow_b = 0`
 * 3. A stage-level inequality (volume-dependent discharge cap):
 *
 *    @code{.text}
 *    qeh - slope * energy_scale * 0.5 * eini
 *        - slope * energy_scale * 0.5 * efin  <= intercept
 *    @endcode
 *
 * When the ReservoirDischargeLimit has multiple piecewise segments,
 * `update_lp()` selects the active segment based on the reservoir volume from
 * the previous solve and updates the LP constraint coefficients/RHS.
 */

#pragma once

#include <gtopt/reservoir_discharge_limit.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/sddp_common.hpp>
#include <gtopt/waterway_lp.hpp>

namespace gtopt
{

class SystemLP;

/**
 * @brief LP wrapper for ReservoirDischargeLimit constraints
 */
class ReservoirDischargeLimitLP : public ObjectLP<ReservoirDischargeLimit>
{
public:
  static constexpr LPClassName ClassName {"ReservoirDischargeLimit", "rdl"};

  explicit ReservoirDischargeLimitLP(const ReservoirDischargeLimit& ddl,
                                     InputContext& ic);

  [[nodiscard]] constexpr auto&& reservoir_discharge_limit(
      this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto waterway_sid() const noexcept
  {
    return WaterwayLPSId {reservoir_discharge_limit().waterway};
  }
  [[nodiscard]] constexpr auto reservoir_sid() const noexcept
  {
    return ReservoirLPSId {reservoir_discharge_limit().reservoir};
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /**
   * @brief Update volume-dependent LP coefficients
   *
   * Selects the active piecewise segment based on the reservoir's current
   * volume and updates the slope coefficient on eini/efin columns and the
   * RHS intercept in the volume constraint row.
   *
   * @return Number of LP coefficients/bounds modified (0 if unchanged)
   */
  [[nodiscard]] int update_lp(SystemLP& sys,
                              const ScenarioLP& scenario,
                              const StageLP& stage);

  /// Tracks current LP state for coefficient updates
  struct RDLState
  {
    ColIndex eini_col {};
    ColIndex efin_col {};
    Real current_slope {0.0};
    Real current_rhs {0.0};
  };

private:
  /// qeh column per (scenario, stage)
  STIndexHolder<ColIndex> qeh_cols;

  /// Stage-level averaging row per (scenario, stage)
  STIndexHolder<RowIndex> avg_rows;

  /// Volume constraint row per (scenario, stage)
  STIndexHolder<RowIndex> vol_rows;

  /// Per-(scenario, stage) state for coefficient tracking
  IndexHolder2<ScenarioUid, StageUid, RDLState> m_states_;
};

}  // namespace gtopt
