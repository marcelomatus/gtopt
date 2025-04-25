/**
 * @file      capacity_lp.hpp
 * @brief     Header of
 * @date      Thu Mar 27 21:55:18 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/capacity.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/object_lp.hpp>

namespace gtopt
{

class CapacityLP : public ObjectLP<Capacity>
{
public:
  constexpr static std::string_view ClassName = "Capacity";

  template<typename CapacityT>
  explicit CapacityLP(const InputContext& ic, CapacityT&& pcapacity)
      : ObjectLP<Capacity>(ic, ClassName, std::forward<CapacityT>(pcapacity))
      , capacity(ic, ClassName, id(), std::move(object().capacity))
      , expcap(ic, ClassName, id(), std::move(object().expcap))
      , capmax(ic, ClassName, id(), std::move(object().capmax))
      , annual_capcost(ic, ClassName, id(), std::move(object().annual_capcost))
      , expmod(ic, ClassName, id(), std::move(object().expmod))
      , annual_derating(
            ic, ClassName, id(), std::move(object().annual_derating))
  {
  }

  bool lazy_add_to_lp(const SystemContext& sc,
                      const ScenarioIndex& scenario_index,
                      const StageIndex& stage_index,
                      LinearProblem& lp) const;

  bool add_to_output(OutputContext& out) const;

  constexpr auto capacity_col_at(const SystemContext& sc,
                                 const ScenarioIndex& scenario_index,
                                 const StageIndex& stage_index,
                                 LinearProblem& lp) const
      -> std::optional<size_t>
  {
    const auto ocol = get_optvalue(capacity_cols, stage_index);
    if (ocol.has_value()) [[likely]] {
      return ocol;
    }

    return lazy_add_to_lp(sc, scenario_index, stage_index, lp)
        ? get_optvalue(capacity_cols, stage_index)
        : std::nullopt;
  }

  constexpr auto capacity_at(const StageIndex stage_index) const
  {
    return capacity.at(stage_index);
  }

  constexpr auto capacity_or(const std::optional<StageIndex>& stage_index,
                             const auto def_capacity) const
  {
    return stage_index.has_value() ? capacity.at(stage_index.value())
                                   : def_capacity;
  }

private:
  OptTRealSched capacity;
  OptTRealSched expcap;
  OptTRealSched capmax;
  OptTRealSched annual_capcost;
  OptTRealSched expmod;
  OptTRealSched annual_derating;

  mutable TIndexHolder capacity_cols;
  mutable TIndexHolder capacost_cols;
  mutable TIndexHolder expmod_cols;
  mutable TIndexHolder capacity_rows;
  mutable TIndexHolder capacost_rows;
};

}  // namespace gtopt
