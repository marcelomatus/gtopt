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

#include <functional>
#include <optional>

#include <gtopt/basic_types.hpp>
#include <gtopt/flow.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

class ApertureDataCache;  // forward declaration

/// Single-ID alias for referencing a FlowLP in other LP elements
using FlowLPSId = ObjectSingleId<class FlowLP>;

class FlowLP : public ObjectLP<Flow>
{
public:
  static constexpr std::string_view FlowName {"flow"};

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
    return JunctionLPSId {
        require_sid(flow().junction, "FlowLP::junction_sid", "junction")};
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
   * @brief Return the discharge value for a given scenario/stage/block.
   * @param scenario_uid Scenario UID to look up
   * @param stage_uid Stage UID to look up
   * @param block_uid Block UID to look up
   * @return Optional discharge value if present
   */
  [[nodiscard]] std::optional<double> aperture_value(ScenarioUid scenario_uid,
                                                     StageUid stage_uid,
                                                     BlockUid block_uid) const
  {
    return discharge.at(scenario_uid, stage_uid, block_uid);
  }

  /**
   * @brief Update flow column bounds in a cloned LP for an aperture scenario.
   *
   * During the SDDP backward pass with apertures, the flow columns that were
   * originally fixed to the base scenario's discharge values are updated to
   * reflect the aperture scenario's discharge values.  The value_fn callable
   * provides the new discharge value for each (stage, block) pair.
   *
   * @param li            The cloned LinearInterface to modify (in-place).
   * @param base_scenario The scenario used when the LP was originally built.
   * @param value_fn      (StageUid, BlockUid) -> optional<double> provider.
   * @param stage         The stage for which to update the bounds.
   * @return true on success.
   */
  [[nodiscard]] bool update_aperture(
      LinearInterface& li,
      const ScenarioLP& base_scenario,
      const std::function<std::optional<double>(StageUid, BlockUid)>& value_fn,
      const StageLP& stage) const;

private:
  STBRealSched discharge;
  STBIndexHolder<ColIndex> flow_cols;
};

// Pin the data-struct constant value so an accidental rename of the
// `Flow::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"Flow"`).
static_assert(FlowLP::Element::class_name == LPClassName {"Flow"},
              "Flow::class_name must remain \"Flow\"");

}  // namespace gtopt
