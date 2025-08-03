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

  /**
   * @brief Merges another simulation into this one
   *
   * This is a unified template method that handles both lvalue and rvalue
   * references. When merging from an rvalue reference, move semantics are used
   * automatically.
   *
   * @tparam T Simulation reference type (can be lvalue or rvalue reference)
   * @param sim The simulation to merge from (will be moved from if it's an
   * rvalue)
   * @return Reference to this simulation after merge
   */
  template<typename T>
  constexpr Simulation& merge(T&& sim)
  {
    // Using std::forward to preserve value category (lvalue vs rvalue)
    gtopt::merge(block_array, std::forward<T>(sim).block_array);
    gtopt::merge(stage_array, std::forward<T>(sim).stage_array);
    gtopt::merge(scenario_array, std::forward<T>(sim).scenario_array);
    gtopt::merge(phase_array, std::forward<T>(sim).phase_array);
    gtopt::merge(scene_array, std::forward<T>(sim).scene_array);

    return *this;
  }
};

}  // namespace gtopt
