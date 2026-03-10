/**
 * @file      reservoir_efficiency_lp.hpp
 * @brief     LP representation of reservoir-dependent turbine efficiency
 * @date      Mon Mar 10 17:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides the `ReservoirEfficiencyLP` class which manages the LP
 * coefficient update for turbines whose conversion rate varies with
 * the hydraulic head (reservoir volume).
 *
 * During the initial LP build (`add_to_lp`), this class locates the
 * turbine's conversion-rate constraint row and the waterway flow column,
 * stores their indices, and computes the initial conversion rate from
 * the reservoir's initial volume (`eini`).
 *
 * During SDDP forward-pass iterations, `update_efficiency()` recomputes
 * the conversion rate from the current reservoir volume and calls
 * `LinearInterface::set_coeff()` to update the LP matrix in-place.
 */

#pragma once

#include <gtopt/reservoir_efficiency.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/turbine_lp.hpp>

namespace gtopt
{

/**
 * @brief LP representation of a ReservoirEfficiency element
 *
 * Stores per-(scenario,stage,block) row/column indices for the turbine
 * conversion-rate constraint so that the coefficient can be updated
 * when the reservoir volume changes during SDDP iterations.
 */
class ReservoirEfficiencyLP : public ObjectLP<ReservoirEfficiency>
{
public:
  static constexpr LPClassName ClassName {"ReservoirEfficiency", "ref"};

  explicit ReservoirEfficiencyLP(const ReservoirEfficiency& pre,
                                 [[maybe_unused]] InputContext& ic) noexcept
      : ObjectLP<ReservoirEfficiency>(pre)
  {
  }

  [[nodiscard]] constexpr auto&& efficiency(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto turbine_sid() const noexcept
  {
    return TurbineLPSId {efficiency().turbine};
  }

  [[nodiscard]] constexpr auto reservoir_sid() const noexcept
  {
    return ReservoirLPSId {efficiency().reservoir};
  }

  /// Evaluate the piecewise-linear efficiency at the given volume
  [[nodiscard]] auto compute_efficiency(Real volume) const noexcept -> Real
  {
    return evaluate_efficiency(efficiency().segments, volume);
  }

  /// Return the mean (fallback) efficiency value
  [[nodiscard]] constexpr auto mean_efficiency() const noexcept -> Real
  {
    return efficiency().mean_efficiency;
  }

  /**
   * @brief Register this efficiency element in the LP
   *
   * Locates the turbine's conversion rows and the waterway's flow columns,
   * stores their indices for later coefficient updates.  The initial
   * conversion-rate coefficient is set during TurbineLP::add_to_lp().
   */
  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

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
