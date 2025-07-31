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

#include <gtopt/flow.hpp>
#include <gtopt/junction_lp.hpp>

namespace gtopt
{

class FlowLP : public ObjectLP<Flow>
{
public:
  constexpr static std::string_view ClassName = "Flow";

  explicit FlowLP(Flow pflow, const InputContext& ic);

  [[nodiscard]] constexpr auto&& flow(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto junction() const noexcept
  {
    return JunctionLPSId {flow().junction};
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

private:
  STBRealSched discharge;
  STBIndexHolder<ColIndex> flow_cols;
};

}  // namespace gtopt
