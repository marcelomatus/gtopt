/**
 * @file      flat_helper.cpp
 * @brief     Header of
 * @date      Thu May 15 01:12:14 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <gtopt/basic_types.hpp>
#include <gtopt/flat_helper.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage_lp.hpp>
#include <range/v3/view/filter.hpp>

namespace
{
using namespace gtopt;

template<typename Operation = decltype(true_fnc)>
constexpr STBUids make_stb_uids(const SimulationLP& sc, Operation op = true_fnc)
{
  const auto size = sc.scenarios().size() * sc.blocks().size();
  std::vector<Uid> scenery_uids;
  scenery_uids.reserve(size);
  std::vector<Uid> stage_uids;
  stage_uids.reserve(size);
  std::vector<Uid> block_uids;
  block_uids.reserve(size);

  for (auto&& scenery : sc.scenarios() | ranges::views::filter(op)) {
    for (auto&& stage : sc.stages() | ranges::views::filter(op)) {
      for (auto&& block : stage.blocks()) {
        scenery_uids.push_back(scenery.uid());
        stage_uids.push_back(stage.uid());
        block_uids.push_back(block.uid());
      }
    }
  }

  scenery_uids.shrink_to_fit();
  stage_uids.shrink_to_fit();
  block_uids.shrink_to_fit();

  return {
      std::move(scenery_uids), std::move(stage_uids), std::move(block_uids)};
}

template<typename Operation = decltype(true_fnc)>
constexpr STUids make_st_uids(const SimulationLP& sc,
                              Operation op = true_fnc) noexcept
{
  const auto size = sc.scenarios().size() * sc.stages().size();
  std::vector<Uid> scenario_uids;
  scenario_uids.reserve(size);
  std::vector<Uid> stage_uids;
  stage_uids.reserve(size);

  for (auto&& scenario : sc.scenarios() | ranges::views::filter(op)) {
    for (auto&& stage : sc.stages() | ranges::views::filter(op)) {
      scenario_uids.push_back(scenario.uid());
      stage_uids.push_back(stage.uid());
    }
  }

  scenario_uids.shrink_to_fit();
  stage_uids.shrink_to_fit();

  return {std::move(scenario_uids), std::move(stage_uids)};
}

template<typename Operation = decltype(true_fnc)>
constexpr TUids make_t_uids(const SimulationLP& sc,
                            Operation op = true_fnc) noexcept
{
  const auto size = sc.stages().size();
  std::vector<Uid> stage_uids;
  stage_uids.reserve(size);

  for (auto&& stage : sc.stages() | ranges::views::filter(op)) {
    stage_uids.push_back(stage.uid());
  }

  stage_uids.shrink_to_fit();

  return stage_uids;
}

}  // namespace

namespace gtopt
{

STBUids FlatHelper::stb_active_uids() const
{
  return make_stb_uids(simulation(), active_fnc);
}

STBUids FlatHelper::stb_uids() const
{
  return make_stb_uids(simulation());
}

STUids FlatHelper::st_active_uids() const
{
  return make_st_uids(simulation(), active_fnc);
}

STUids FlatHelper::st_uids() const
{
  return make_st_uids(simulation());
}

TUids FlatHelper::t_active_uids() const
{
  return make_t_uids(simulation(), active_fnc);
}

TUids FlatHelper::t_uids() const
{
  return make_t_uids(simulation());
}

}  // namespace gtopt
