/**
 * @file      volume_right_lp.hpp
 * @brief     LP representation of volume-based water rights
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the VolumeRightLP class which provides methods to:
 * - Represent volume-based water rights in LP problems as storage elements
 * - Track accumulated right volumes via SDDP state variables
 *   (Tilmant's "dummy reservoir" approach)
 * - Penalize unmet volume demands in the objective
 *
 * The volume right is NOT part of the hydrological topology.
 * It creates its own storage balance (rights ledger) without modifying
 * reservoir or junction balance rows.
 */

#pragma once

#include <gtopt/flow_right_lp.hpp>
#include <gtopt/right_bound_rule.hpp>
#include <gtopt/storage_lp.hpp>
#include <gtopt/volume_right.hpp>

namespace gtopt
{

// Forward declaration to avoid circular includes
class SystemLP;
using VolumeRightLPId = ObjectId<class VolumeRightLP>;
using VolumeRightLPSId = ObjectSingleId<class VolumeRightLP>;

/**
 * @brief LP representation of a volume-based water right
 *
 * Extends StorageLP to provide storage-like LP behaviour for water
 * rights accounting.  The "energy" balance tracks accumulated
 * right volume (hm³), not physical water.
 *
 * State variable coupling via SDDP propagates accumulated right volumes
 * across phases, following Tilmant et al. (2008).
 */
class VolumeRightLP : public StorageLP<ObjectLP<VolumeRight>>
{
public:
  static constexpr LPClassName ClassName {"VolumeRight", "vrt"};

  using StorageBase = StorageLP<ObjectLP<VolumeRight>>;

  explicit VolumeRightLP(const VolumeRight& pvol, const InputContext& ic);

  [[nodiscard]] constexpr auto&& volume_right(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto flow_conversion_rate() const noexcept
  {
    return volume_right().flow_conversion_rate.value_or(
        VolumeRight::default_flow_conversion_rate);
  }

  [[nodiscard]] bool add_to_lp(SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// Update volume-dependent column bounds when bound_rule is set.
  /// @return Number of LP column bounds modified (0 if unchanged)
  [[nodiscard]] int update_lp(SystemLP& sys,
                              const ScenarioLP& scenario,
                              const StageLP& stage);

  /// Return the input flow column indices for (scenario, stage).
  /// These are the decision variables for how much right volume is
  /// delivered per block.  External entities can reference these
  /// to couple into this VolumeRight's balance.
  [[nodiscard]] const auto& finp_cols_at(const ScenarioLP& scenario,
                                         const StageLP& stage) const
  {
    return finp_cols_map.at({scenario.uid(), stage.uid()});
  }

  /// @name Parameter accessors for user constraint resolution
  /// @{
  [[nodiscard]] auto param_fmax(StageUid s, BlockUid b) const
  {
    return fmax.at(s, b);
  }
  [[nodiscard]] auto param_demand(StageUid s) const { return demand.at(s); }
  [[nodiscard]] auto param_fail_cost() const { return fail_cost; }
  /// @}

private:
  OptTRealSched demand;
  OptTBRealSched fmax;
  double fail_cost {0.0};
  STBIndexHolder<ColIndex> finp_cols_map;

  /// Cached bound rule evaluation per (scenario, stage).
  struct BoundState
  {
    Real current_bound {0.0};
  };
  IndexHolder2<ScenarioUid, StageUid, BoundState> m_bound_states_;
};

}  // namespace gtopt
