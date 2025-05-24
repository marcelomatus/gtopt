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

#include <span>

#include "gtopt/simulation.hpp"

#include <gtopt/scene.hpp>
#include <gtopt/simulation_lp.hpp>
#include <range/v3/all.hpp>
#include <range/v3/view/all.hpp>

namespace gtopt
{

namespace
{
constexpr std::vector<BlockLP> create_block_array(const auto& simulation)
{
  return simulation.block_array | ranges::views::move
      | ranges::views::transform(
             [](auto&& s) { return BlockLP {std::forward<decltype(s)>(s)}; })
      | ranges::to<std::vector>();
}

constexpr std::vector<StageLP> create_stage_array(const auto& simulation,
                                                  const auto& options,
                                                  const auto& block_array)
{
  return simulation.stage_array | ranges::views::move
      | ranges::views::transform(
             [&](auto&& s)
             {
               return StageLP {std::forward<decltype(s)>(s),
                               block_array,
                               options.annual_discount_rate()};
             })
      | ranges::to<std::vector>();
}

constexpr std::vector<ScenarioLP> create_scenario_array(const auto& simulation,
                                                        const auto& stage_array,
                                                        const Scene& scene)
{
  return std::span(simulation.scenario_array)
      | ranges::views::drop(scene.first_scenario)
      | ranges::views::take(scene.count_scenario) | ranges::views::move
      | ranges::views::transform(
             [&](auto&& s)
             { return ScenarioLP {std::forward<decltype(s)>(s), stage_array}; })
      | ranges::to<std::vector>();
}

constexpr std::vector<PhaseLP> create_phase_array(const auto& simulation,
                                                  const auto& stage_array)
{
  return simulation.phase_array | ranges::views::move
      | ranges::views::transform(
             [&](auto&& s)
             { return PhaseLP {std::forward<decltype(s)>(s), stage_array}; })
      | ranges::to<std::vector>();
}

constexpr std::vector<SceneLP> create_scene_array(const auto& simulation,
                                                  const auto& scenario_array)
{
  return simulation.scene_array | ranges::views::move
      | ranges::views::transform(
             [&](auto&& s)
             { return SceneLP {std::forward<decltype(s)>(s), scenario_array}; })
      | ranges::to<std::vector>();
}

}  // namespace

void SimulationLP::validate_components()
{
  if (m_block_array_.empty() || m_stage_array_.empty()
      || m_scenario_array_.empty())
  {
    throw std::runtime_error(
        "System must contain at least one block, stage, and scenario");
  }

  const auto nblocks = std::accumulate(m_stage_array_.begin(),  // NOLINT
                                       m_stage_array_.end(),
                                       0U,
                                       [](size_t a, const auto& s)
                                       { return a + s.blocks().size(); });

  if (nblocks != m_block_array_.size()) {
    throw std::runtime_error(
        "Number of blocks in stages doesn't match the total number of "
        "blocks");
  }
}

/**
 * @brief Constructs a simulation object with the given system
 * @param system Power system model to be simulated
 *
 * Initializes the simulation with the provided system model, which contains
 * all components (buses, generators, lines, etc.) and their attributes.
 */

SimulationLP::SimulationLP(const Simulation& psimulation,
                           const OptionsLP& poptions,
                           PlanningLP& planning,
                           const Scene& pscene)
    : m_simulation_(psimulation)
    , m_options_(poptions)
    , m_planning_(planning)
    , m_block_array_(create_block_array(simulation()))
    , m_stage_array_(
          create_stage_array(simulation(), options(), m_block_array_))
    , m_scenario_array_(
          create_scenario_array(simulation(), m_stage_array_, pscene))
    , m_phase_array_(create_phase_array(simulation(), m_stage_array_))
    , m_scene_array_(create_scene_array(simulation(), m_scenario_array_))
{
  // validate_components();
}

}  // namespace gtopt
