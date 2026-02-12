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

namespace gtopt
{

STBUids FlatHelper::stb_uids() const
{
  std::size_t n_scenarios = 0;
  for (auto&& scene : simulation().scenes()) {
    n_scenarios += scene.scenarios().size();
  }
  const auto n_blocks = simulation().blocks().size();

  STBUids res;
  res.reserve(n_scenarios * n_blocks);

  for (auto&& scene : simulation().scenes()) {
    for (auto&& scenario : scene.scenarios()) {
      for (auto&& phase : simulation().phases()) {
        for (auto&& stage : phase.stages()) {
          for (auto&& block : stage.blocks()) {
            res.scenario_uids.emplace_back(scenario.uid());
            res.stage_uids.emplace_back(stage.uid());
            res.block_uids.emplace_back(block.uid());
          }
        }
      }
    }
  }

  return res;
}

STUids FlatHelper::st_uids() const
{
  std::size_t n_scenarios = 0;
  for (auto&& scene : simulation().scenes()) {
    n_scenarios += scene.scenarios().size();
  }
  const auto n_stages = simulation().stages().size();

  STUids res;
  res.reserve(n_scenarios * n_stages);

  for (auto&& scene : simulation().scenes()) {
    for (auto&& scenario : scene.scenarios()) {
      for (auto&& phase : simulation().phases()) {
        for (auto&& stage : phase.stages()) {
          res.scenario_uids.emplace_back(scenario.uid());
          res.stage_uids.emplace_back(stage.uid());
        }
      }
    }
  }

  return res;
}

TUids FlatHelper::t_uids() const
{
  TUids res;
  res.reserve(simulation().stages().size());

  for (auto&& phase : simulation().phases()) {
    for (auto&& stage : phase.stages()) {
      res.stage_uids.emplace_back(stage.uid());
    }
  }

  return res;
}

}  // namespace gtopt
