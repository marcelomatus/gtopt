/**
 * @file      right_junction_lp.hpp
 * @brief     LP representation of water rights balance nodes
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the RightJunctionLP class which creates balance rows
 * for rights accounting.  FlowRight entities with a matching
 * right_junction reference add their flow variables to these rows.
 *
 * This is the rights-domain counterpart of JunctionLP.
 */

#pragma once

#include <gtopt/object_lp.hpp>
#include <gtopt/right_junction.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

using RightJunctionLPSId = ObjectSingleId<class RightJunctionLP>;

class RightJunctionLP : public ObjectLP<RightJunction>
{
public:
  static constexpr LPClassName ClassName {"RightJunction", "rjn"};

  explicit RightJunctionLP(const RightJunction& prj,
                           [[maybe_unused]] const InputContext& ic)
      : ObjectLP<RightJunction>(prj)
  {
  }

  [[nodiscard]] constexpr auto&& right_junction(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto drain() const noexcept
  {
    return right_junction().drain.value_or(true);
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  [[nodiscard]] const auto& balance_rows_at(const ScenarioLP& scenario,
                                            const StageLP& stage) const
  {
    return balance_rows.at({scenario.uid(), stage.uid()});
  }

  [[nodiscard]] const auto& drain_cols_at(const ScenarioLP& scenario,
                                          const StageLP& stage) const
  {
    return drain_cols.at({scenario.uid(), stage.uid()});
  }

private:
  STBIndexHolder<RowIndex> balance_rows;
  STBIndexHolder<ColIndex> drain_cols;
};

}  // namespace gtopt
