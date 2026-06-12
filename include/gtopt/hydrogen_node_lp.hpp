/**
 * @file      hydrogen_node_lp.hpp
 * @brief     LP wrapper for ``HydrogenNode`` — carrier-side balance row
 * @date      Sat May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Mirror of ``thermal_node_lp.hpp`` for the hydrogen carrier (MWh_LHV).
 */

#pragma once

#include <gtopt/hydrogen_node.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

class HydrogenNodeLP : public ObjectLP<HydrogenNode>
{
public:
  static constexpr std::string_view BalanceName {"balance"};

  explicit HydrogenNodeLP(const HydrogenNode& phn,
                          [[maybe_unused]] const InputContext& ic)
      : ObjectLP<HydrogenNode>(phn)
  {
  }

  [[nodiscard]] constexpr auto&& hydrogen_node(this auto&& self) noexcept
  {
    return self.object();
  }

  bool add_to_lp(const SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);

  bool add_to_output(OutputContext& out) const;

  [[nodiscard]] const auto& balance_rows_at(const ScenarioLP& scenario,
                                            const StageLP& stage) const
  {
    return balance_rows.at({scenario.uid(), stage.uid()});
  }

private:
  STBIndexHolder<RowIndex> balance_rows;
};

using HydrogenNodeLPId = ObjectId<class HydrogenNodeLP>;
using HydrogenNodeLPSId = ObjectSingleId<class HydrogenNodeLP>;

}  // namespace gtopt
