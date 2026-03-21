/**
 * @file      flow_lp.hpp
 * @brief     Linear programming representation of network flows
 * @date      Wed Jul 30 15:54:03 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the FlowLP class which provides methods to:
 * - Represent flows in linear programming problems
 * - Manage flow variables and constraints
 * - Interface with junctions and other network components
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/flow.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/linear_interface.hpp>

namespace gtopt
{

class ApertureDataCache;  // forward declaration

/// Single-ID alias for referencing a FlowLP in other LP elements
using FlowLPSId = ObjectSingleId<class FlowLP>;

class FlowLP : public ObjectLP<Flow>
{
public:
  static constexpr LPClassName ClassName {"Flow", "flw"};

  explicit FlowLP(const Flow& pflow, const InputContext& ic);

  [[nodiscard]] constexpr auto&& flow(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] bool has_junction() const noexcept
  {
    return flow().junction.has_value();
  }

  [[nodiscard]] auto junction_sid() const
  {
    return JunctionLPSId {flow().junction.value()};
  }

  [[nodiscard]] constexpr bool is_input() const noexcept
  {
    return flow().is_input();
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  [[nodiscard]] auto&& flow_cols_at(const ScenarioLP& scenario,
                                    const StageLP& stage) const
  {
    return flow_cols.at({scenario.uid(), stage.uid()});
  }

  /**
   * @brief Update flow column bounds in a cloned LP for an aperture scenario.
   *
   * During the SDDP backward pass with apertures, the flow columns that were
   * originally fixed to the base scenario's discharge values are updated to
   * reflect the aperture scenario's discharge values.  This allows each
   * aperture to represent a different hydrological realisation while sharing
   * the same LP structure.
   *
   * @param li                The cloned LinearInterface to modify (in-place).
   * @param base_scenario     The scenario used when the LP was originally
   *                          built (identifies which column indices to use).
   * @param aperture_scenario The scenario whose discharge values to apply.
   * @param stage             The stage for which to update the bounds.
   * @return true on success; false if no columns are registered for the
   *         (base_scenario, stage) pair (element inactive for that stage).
   */
  [[nodiscard]] bool update_aperture_lp(LinearInterface& li,
                                        const ScenarioLP& base_scenario,
                                        const ScenarioLP& aperture_scenario,
                                        const StageLP& stage) const;

  /// Update flow column bounds from the ApertureDataCache when the
  /// aperture scenario is not in the forward set.
  [[nodiscard]] bool update_aperture_from_cache(LinearInterface& li,
                                                const ScenarioLP& base_scenario,
                                                Uid aperture_scenario_uid,
                                                const ApertureDataCache& cache,
                                                const StageLP& stage) const;

private:
  STBRealSched discharge;
  STBIndexHolder<ColIndex> flow_cols;
};

}  // namespace gtopt
