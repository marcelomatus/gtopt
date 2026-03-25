/**
 * @file      reservoir_production_factor_lp.hpp
 * @brief     LP representation of reservoir-dependent turbine efficiency
 * @date      Mon Mar 10 17:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides the `ReservoirProductionFactorLP` class which manages the LP
 * coefficient update for turbines whose conversion rate varies with
 * the hydraulic head (reservoir volume).
 *
 * During the initial LP build (`add_to_lp`), this class locates the
 * turbine's conversion-rate constraint row and the waterway flow column,
 * stores their indices, and computes the initial conversion rate from
 * the reservoir's initial volume (`eini`).
 *
 * During SDDP forward-pass iterations, `update_conversion_coeff()`
 * recomputes the conversion rate from the current reservoir volume and
 * calls `LinearInterface::set_coeff()` to update the LP matrix in-place.
 */

#pragma once

#include <gtopt/planning_options_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/reservoir_production_factor.hpp>
#include <gtopt/sddp_common.hpp>
#include <gtopt/turbine_lp.hpp>

namespace gtopt
{

class LinearInterface;

/**
 * @brief LP representation of a ReservoirProductionFactor element
 *
 * Stores per-(scenario,stage,block) row/column indices for the turbine
 * conversion-rate constraint so that the coefficient can be updated
 * when the reservoir volume changes during SDDP iterations.
 */
class ReservoirProductionFactorLP : public ObjectLP<ReservoirProductionFactor>
{
public:
  static constexpr LPClassName ClassName {"ReservoirProductionFactor", "ref"};

  explicit ReservoirProductionFactorLP(
      const ReservoirProductionFactor& pre,
      [[maybe_unused]] InputContext& ic) noexcept
      : ObjectLP<ReservoirProductionFactor>(pre)
  {
  }

  [[nodiscard]] constexpr auto&& production_factor(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto turbine_sid() const noexcept
  {
    return TurbineLPSId {production_factor().turbine};
  }

  [[nodiscard]] constexpr auto reservoir_sid() const noexcept
  {
    return ReservoirLPSId {production_factor().reservoir};
  }

  /// Evaluate the piecewise-linear efficiency at the given volume
  [[nodiscard]] auto compute_production_factor(Real volume) const noexcept
      -> Real
  {
    return evaluate_production_factor(production_factor().segments, volume);
  }

  /// Return the mean (fallback) efficiency value
  [[nodiscard]] constexpr auto mean_production_factor() const noexcept -> Real
  {
    return production_factor().mean_production_factor;
  }

  /**
   * @brief Register this production factor element in the LP
   *
   * Locates the turbine's conversion rows and the waterway's flow columns,
   * stores their indices for later coefficient updates.  The initial
   * conversion-rate coefficient is set during TurbineLP::add_to_lp().
   */
  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] static bool add_to_output(OutputContext& out);

  /**
   * @brief Update the conversion-rate LP coefficient based on reservoir volume
   *
   * Determines the current reservoir volume from the previous LP solution
   * (`vavg = (vini + vfin) / 2`), falling back to the JSON `eini` value when
   * no previous solution is available (first stage of first phase).
   *
   * Evaluates the piecewise-linear efficiency at that volume and sets the
   * turbine conversion coefficient to `-efficiency` for every (row, col)
   * pair stored for the given (scenario, stage).
   *
   * @return Number of LP coefficients modified (0 if unchanged or skipped)
   */
  [[nodiscard]] int update_lp(SystemLP& sys,
                              const ScenarioLP& scenario,
                              const StageLP& stage);

  /**
   * @brief Update the conversion-rate LP coefficient for a given volume
   *
   * Evaluates the piecewise-linear efficiency at @p volume and sets the
   * coefficient to `-efficiency` for every (row, col) pair stored for the
   * given (scenario, stage).  The negative sign matches the turbine
   * conversion-row convention: `generation − conversion_rate × flow = 0`.
   *
   * @param li   The linear interface to modify
   * @param suid Scenario UID
   * @param tuid Stage UID
   * @param volume Current reservoir volume [dam³]
   * @return Number of coefficients updated
   */
  auto update_conversion_coeff(LinearInterface& li,
                               ScenarioUid suid,
                               StageUid tuid,
                               Real volume) const -> int;

  /// Per-block conversion row and flow column indices for coefficient updates
  struct CoeffIndex
  {
    RowIndex row;  ///< Conversion-rate constraint row
    ColIndex col;  ///< Waterway flow column
  };

  using BCoeffMap = flat_map<BlockUid, CoeffIndex>;

  /// Access stored coefficient indices for a given (scenario, stage)
  [[nodiscard]] auto coeff_indices_at(ScenarioUid suid, StageUid tuid) const
      -> const BCoeffMap&
  {
    return m_coeff_indices_.at({suid, tuid});
  }

  /// Check if coefficient indices are available for a given (scenario, stage)
  [[nodiscard]] auto has_coeff_indices(ScenarioUid suid, StageUid tuid) const
      -> bool
  {
    return m_coeff_indices_.contains({suid, tuid});
  }

private:
  /// Stored row/column indices indexed by (scenario, stage) → block
  IndexHolder2<ScenarioUid, StageUid, BCoeffMap> m_coeff_indices_;
};

}  // namespace gtopt
