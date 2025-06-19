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

using STBUids =
    std::tuple<std::vector<int>, std::vector<int>, std::vector<int>>;
constexpr STBUids make_stb_uids(const SimulationLP& sc)
{
  std::size_t size = 0;
  for (auto&& scene : sc.scenes()) {
    for ([[maybe_unused]] auto&& scenario : scene.scenarios()) {
      for (auto&& phase : sc.phases()) {
        for (auto&& stage : phase.stages()) {
          size += stage.blocks().size();
        }
      }
    }
  }

  std::vector<Uid> scenario_uids;
  scenario_uids.reserve(size);
  std::vector<Uid> stage_uids;
  stage_uids.reserve(size);
  std::vector<Uid> block_uids;
  block_uids.reserve(size);

  for (auto&& scene : sc.scenes()) {
    for (auto&& scenario : scene.scenarios()) {
      for (auto&& phase : sc.phases()) {
        for (auto&& stage : phase.stages()) {
          for (auto&& block : stage.blocks()) {
            scenario_uids.emplace_back(scenario.uid());
            stage_uids.emplace_back(stage.uid());
            block_uids.emplace_back(block.uid());
          }
        }
      }
    }
  }

  return {
      std::move(scenario_uids), std::move(stage_uids), std::move(block_uids)};
}

constexpr STUids make_st_uids(const SimulationLP& sc) noexcept
{
  std::size_t size = 0;
  for (auto&& scene : sc.scenes()) {
    for ([[maybe_unused]] auto&& scenario : scene.scenarios()) {
      for (auto&& phase : sc.phases()) {
        size += phase.stages().size();
      }
    }
  }

  std::vector<Uid> scenario_uids;
  scenario_uids.reserve(size);
  std::vector<Uid> stage_uids;
  stage_uids.reserve(size);

  for (auto&& scene : sc.scenes()) {
    for (auto&& scenario : scene.scenarios()) {
      for (auto&& phase : sc.phases()) {
        for (auto&& stage : phase.stages()) {
          scenario_uids.emplace_back(scenario.uid());
          stage_uids.emplace_back(stage.uid());
        }
      }
    }
  }

  return {std::move(scenario_uids), std::move(stage_uids)};
}

constexpr TUids make_t_uids(const SimulationLP& sc) noexcept
{
  std::size_t size = 0;
  for (auto&& phase : sc.phases()) {
    size += phase.stages().size();
  }

  std::vector<Uid> stage_uids;
  stage_uids.reserve(size);

  for (auto&& phase : sc.phases()) {
    for (auto&& stage : phase.stages()) {
      stage_uids.emplace_back(stage.uid());
    }
  }

  return stage_uids;
}

}  // namespace

namespace gtopt
{

STBUids FlatHelper::stb_uids() const
{
  return make_stb_uids(simulation());
}

STUids FlatHelper::st_uids() const
{
  return make_st_uids(simulation());
}

TUids FlatHelper::t_uids() const
{
  return make_t_uids(simulation());
}

}  // namespace gtopt
