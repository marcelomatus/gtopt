/**
 * @file      simulation.hpp
 * @brief     Header of Simulation class
 * @date      Wed Mar 19 21:59:12 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Simulation class, which contains all the simulation
 * elements.
 */

#pragma once

#include <gtopt/block.hpp>
#include <gtopt/options.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

/**
 * @brief Represents a complete power simulation model
 */
struct Simulation
{
  Array<Block> block_array {};
  Array<Stage> stage_array {};
  Array<Scenario> scenario_array {};
  Array<Phase> phase_array {Phase {}};
  Array<Scene> scene_array {Scene {}};

  constexpr Simulation& merge(Simulation&& sim)  // NOLINT
  {
    gtopt::merge(block_array, std::move(sim.block_array));
    gtopt::merge(stage_array, std::move(sim.stage_array));
    gtopt::merge(scenario_array, std::move(sim.scenario_array));
    gtopt::merge(phase_array, std::move(sim.phase_array));
    gtopt::merge(scene_array, std::move(sim.scene_array));

    return *this;
  }
};

}  // namespace gtopt
