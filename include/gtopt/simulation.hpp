/**c
 * @file      simulation.hpp<gtopt>
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
#include <gtopt/phase.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/stage.hpp>

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
};

}  // namespace gtopt
