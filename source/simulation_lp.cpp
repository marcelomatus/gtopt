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
#include <range/v3/all.hpp>
#include <range/v3/view/all.hpp>

namespace gtopt
{

namespace
{
constexpr auto create_block_array(const auto& simulation)
{
  return enumerate<BlockIndex>(simulation.block_array)
      | ranges::views::transform(
             [](auto&& ib)
             {
               auto&& [index, block] = ib;
               return BlockLP {block, index};
             })
      | ranges::to<std::vector>();
}

constexpr auto create_stage_array(const Simulation& simulation,
                                  const OptionsLP& options)
{
  return enumerate_active<StageIndex>(simulation.stage_array)
      | ranges::views::transform(
             [&](auto&& is)
             {
               const auto& [index, stage] = is;
               return StageLP {stage,
                               simulation.block_array,
                               options.annual_discount_rate(),
                               index};
             })
      | ranges::to<std::vector>();
}

constexpr auto create_scenario_array(const auto& simulation)
{
  auto&& scenarios = simulation.scenario_array;

  return active(scenarios)
      | ranges::views::transform([](const auto& s) { return ScenarioLP {s}; })
      | ranges::to<std::vector>();
}

constexpr std::vector<PhaseLP> create_phase_array(const auto& simulation,
                                                  const auto& options)
{
  return enumerate_active<PhaseIndex>(simulation.phase_array)
      | ranges::views::transform(
             [&](auto&& is)
             {
               const auto& [index, phase] = is;
               return PhaseLP {
                   phase,
                   options,
                   simulation,
                   index,
               };
             })
      | ranges::to<std::vector>();
}

constexpr auto create_scene_array(const Simulation& simulation)
{
  return enumerate_active<SceneIndex>(simulation.scene_array)
      | ranges::views::transform(
             [&](const auto& si)
             {
               auto&& [scene_index, scene] = si;
               return SceneLP {scene, simulation, scene_index};
             })
      | ranges::to<std::vector>();
}

}  // namespace

void SimulationLP::validate_components() {}

/**
 * @brief Constructs a simulation object with the given system
 * @param system Power system model to be simulated
 *
 * Initializes the simulation with the provided system model, which contains
 * all components (buses, generators, lines, etc.) and their attributes.
 */

SimulationLP::SimulationLP(const Simulation& simulation,
                           const OptionsLP& options)
    : m_simulation_(simulation)
    , m_options_(options)
    , m_block_array_(create_block_array(simulation))
    , m_stage_array_(create_stage_array(simulation, options))
    , m_phase_array_(create_phase_array(simulation, options))
    , m_scenario_array_(create_scenario_array(simulation))
    , m_scene_array_(create_scene_array(simulation))
{
}

}  // namespace gtopt
