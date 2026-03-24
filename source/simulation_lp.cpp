/**
 * @file      simulation.cpp
 * @brief     Implementation of linear programming simulation
 * @date      Sun Apr  6 22:05:37 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements power system planning through linear programming
 * techniques, supporting multi-phase, multi-scene scenarios. It handles the
 * full planning workflow from model creation through solution and result
 * extraction.
 *
 * @details The simulation framework supports complex modeling structures with:
 * - Multiple phases (time horizon segments)
 * - Multiple scenes (system states or operating conditions)
 * - Multiple scenarios (stochastic representations)
 * - Multiple stages (time steps within phases)
 */

#include <gtopt/scene.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

namespace
{
auto create_block_array(const Simulation& simulation)
{
  auto index = BlockIndex {0};

  return std::ranges::to<std::vector>(
      simulation.stage_array | std::views::filter(&Stage::is_active)
      | std::views::transform(
          [&](const Stage& stage)
          {
            return std::ranges::to<std::vector>(
                std::span(simulation.block_array)
                    .subspan(stage.first_block, stage.count_block)
                | std::views::transform([&](const Block& block)
                                        { return BlockLP {block, index++}; }));
          })
      | std::views::join);
}

auto create_stage_array(const Simulation& simulation, const OptionsLP& options)
{
  return std::ranges::to<std::vector>(
      enumerate_active<StageIndex>(simulation.stage_array)
      | std::ranges::views::transform(
          [&](auto&& is)
          {
            const auto& [index, stage] = is;
            return StageLP {
                stage,
                simulation.block_array,
                options.annual_discount_rate(),
                index,
            };
          }));
}

auto create_scenario_array(const Simulation& simulation)
{
  auto&& scenarios = simulation.scenario_array;

  return std::ranges::to<std::vector>(enumerate_active<ScenarioIndex>(scenarios)
                                      | std::ranges::views::transform(
                                          [](const auto& is)
                                          {
                                            const auto& [index, scenario] = is;
                                            return ScenarioLP {scenario, index};
                                          }));
}

auto create_phase_array(const Simulation& simulation, const OptionsLP& options)
{
  // When phase_array is empty, use a single default Phase with uid=0 so that
  // file names produced by write_lp() are always based on valid UIDs.
  static const Array<Phase> default_phases {
      Phase {
          .uid = 0,
          .name = {},
          .active = {},
          .first_stage = 0,
          .count_stage = std::dynamic_extent,
          .apertures = {},
      },
  };
  const auto& phases =
      simulation.phase_array.empty() ? default_phases : simulation.phase_array;

  return std::ranges::to<std::vector>(enumerate_active<PhaseIndex>(phases)
                                      | std::ranges::views::transform(
                                          [&](auto&& is)
                                          {
                                            const auto& [index, phase] = is;
                                            return PhaseLP {
                                                phase,
                                                options,
                                                simulation,
                                                index,
                                            };
                                          }));
}

auto create_scene_array(const Simulation& simulation)
{
  // When scene_array is empty, use a single default Scene with uid=0 so that
  // file names produced by write_lp() are always based on valid UIDs.
  static const Array<Scene> default_scenes {
      Scene {
          .uid = 0,
          .name = {},
          .active = {},
          .first_scenario = 0,
          .count_scenario = std::dynamic_extent,
      },
  };
  const auto& scenes =
      simulation.scene_array.empty() ? default_scenes : simulation.scene_array;

  return std::ranges::to<std::vector>(enumerate_active<SceneIndex>(scenes)
                                      | std::ranges::views::transform(
                                          [&](const auto& si)
                                          {
                                            auto&& [index, scene] = si;
                                            return SceneLP {
                                                scene,
                                                simulation,
                                                index,
                                            };
                                          }));
}

auto create_iteration_array(const Simulation& simulation)
{
  auto idx = IterationIndex {0};
  return std::ranges::to<std::vector>(
      simulation.iteration_array
      | std::ranges::views::transform(
          [&idx](const Iteration& iteration)
          { return IterationLP {iteration, idx++}; }));
}

}  // namespace

SimulationLP::SimulationLP(const Simulation& simulation,
                           const OptionsLP& options)
    : m_simulation_(simulation)
    , m_options_(options)
    , m_block_array_(create_block_array(simulation))
    , m_stage_array_(create_stage_array(simulation, options))
    , m_phase_array_(create_phase_array(simulation, options))
    , m_scenario_array_(create_scenario_array(simulation))
    , m_scene_array_(create_scene_array(simulation))
    , m_iteration_array_(create_iteration_array(simulation))
    , m_global_variable_map_(std::ranges::to<global_variable_map_t>(
          iota_range<Size>(0, m_scene_array_.size())
          | std::views::transform(
              [&](const auto&)
              {
                return StrongIndexVector<PhaseIndex, state_variable_map_t>(
                    m_phase_array_.size());
              })))
{
}

}  // namespace gtopt
