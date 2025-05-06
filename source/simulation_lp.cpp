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

#include <gtopt/scene.hpp>
#include <gtopt/simulation_lp.hpp>
#include <range/v3/all.hpp>
#include <range/v3/view/all.hpp>

namespace gtopt
{

constexpr std::vector<BlockLP> SimulationLP::create_block_array()
{
  return simulation().block_array | ranges::views::move
      | ranges::views::transform(
             [](auto&& s) { return BlockLP {std::forward<decltype(s)>(s)}; })
      | ranges::to<std::vector>();
}

constexpr std::vector<StageLP> SimulationLP::create_stage_array()
{
  return simulation().stage_array | ranges::views::move
      | ranges::views::transform(
             [this](auto&& s)
             {
               return StageLP {std::forward<decltype(s)>(s),
                               m_block_array_,
                               options().annual_discount_rate()};
             })
      | ranges::to<std::vector>();
}

constexpr std::vector<ScenarioLP> SimulationLP::create_scenario_array(
    const Scene& scene)
{
  return std::span(simulation().scenario_array)
      | ranges::views::drop(scene.first_scenario)
      | ranges::views::take(scene.count_scenario) | ranges::views::move
      | ranges::views::transform(
             [this](auto&& s)
             {
               return ScenarioLP {std::forward<decltype(s)>(s), m_stage_array_};
             })
      | ranges::to<std::vector>();
}

constexpr std::vector<PhaseLP> SimulationLP::create_phase_array()
{
  return simulation().phase_array | ranges::views::move
      | ranges::views::transform(
             [this](auto&& s)
             { return PhaseLP {std::forward<decltype(s)>(s), m_stage_array_}; })
      | ranges::to<std::vector>();
}

constexpr std::vector<SceneLP> SimulationLP::create_scene_array()
{
  return simulation().scene_array | ranges::views::move
      | ranges::views::transform(
             [this](auto&& s)
             {
               return SceneLP {std::forward<decltype(s)>(s), m_scenario_array_};
             })
      | ranges::to<std::vector>();
}

constexpr void SimulationLP::validate_components()
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

SimulationLP::SimulationLP(const Simulation& simulation,
                           const OptionsLP& options,
                           const Scene& scene)
    : m_simulation_(simulation)
    , m_options_(options)
    , m_block_array_(create_block_array())
    , m_stage_array_(create_stage_array())
    , m_scenario_array_(create_scenario_array(scene))
    , m_phase_array_(create_phase_array())
    , m_scene_array_(create_scene_array())
{
  // validate_components();
}

}  // namespace gtopt
