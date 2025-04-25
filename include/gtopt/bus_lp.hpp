/**
 * @file      bus_lp.hpp
 * @brief     Header of
 * @date      Mon Mar 24 09:40:04 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/bus.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

using BusLPId = ObjectId<class BusLP>;
using BusLPSId = ObjectSingleId<class BusLP>;

class BusLP : public ObjectLP<Bus>
{
public:
  constexpr static std::string_view ClassName = "Bus";

  explicit BusLP(const InputContext& /*ic*/, Bus pbus)
      : ObjectLP<Bus>(std::move(pbus))
  {
  }

  [[nodiscard]] constexpr auto bus() const { return object(); }

  [[nodiscard]] constexpr auto reference_theta() const
  {
    return bus().reference_theta;
  }
  [[nodiscard]] constexpr auto voltage() const -> double
  {
    return bus().voltage.value_or(1);
  }
  [[nodiscard]] constexpr auto use_kirchhoff() const -> bool
  {
    return bus().use_kirchhoff.value_or(true);
  }
  [[nodiscard]] auto needs_kirchhoff(const SystemContext& sc) const -> bool;

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioIndex& scenario_index,
                               const StageIndex& stage_index,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  [[nodiscard]] auto&& balance_rows_at(const ScenarioIndex scenario_index,
                                       const StageIndex stage_index) const
  {
    return balance_rows.at({scenario_index, stage_index});
  }

  using BlockSpan = StageLP::BlockSpan;

  [[nodiscard]] auto theta_cols_at(const SystemContext& sc,
                                   const ScenarioIndex& scenario_index,
                                   const StageIndex& stage_index,
                                   LinearProblem& lp,
                                   const BlockSpan& blocks) const
      -> const BIndexHolder&
  {
    const auto oiter = get_optiter(theta_cols, {scenario_index, stage_index});
    return oiter ? oiter.value()->second
                 : lazy_add_theta(sc, scenario_index, stage_index, lp, blocks);
  }

private:
  auto lazy_add_theta(const SystemContext& sc,
                      const ScenarioIndex& scenario_index,
                      const StageIndex& stage_index,
                      LinearProblem& lp,
                      const BlockSpan& blocks) const -> const BIndexHolder&;

  STBIndexHolder balance_rows;

  mutable STBIndexHolder theta_cols;
};

}  // namespace gtopt
