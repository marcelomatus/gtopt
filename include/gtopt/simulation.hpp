/**
 * @file      simulation.hpp
 * @brief     Header of Simulation class
 * @date      Wed Mar 19 21:59:12 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Simulation class which contains all time-structure
 * elements: blocks, stages, scenarios, phases, and scenes. Together these
 * define the multi-stage, multi-scenario optimization horizon.
 *
 * ### Time-structure hierarchy
 * ```
 * Scenario  (probability_factor)
 *   └─ Phase
 *        └─ Stage  (discount_factor, first_block, count_block)
 *             └─ Block  (duration [h])
 * ```
 *
 * @see Block, Stage, Scenario, Phase, Scene for element definitions
 */

#pragma once

#include <gtopt/aperture.hpp>
#include <gtopt/block.hpp>
#include <gtopt/iteration.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

/**
 * @brief Complete time-structure of a planning simulation
 *
 * A Simulation bundles all temporal elements.  When a field is absent from
 * the input JSON, gtopt uses a default single scenario, single stage/block
 * configuration so that simple single-snapshot cases require minimal input.
 *
 * @note `phase_array` and `scene_array` default to empty.  When they are
 * empty, `SimulationLP` automatically falls back to a single default `Phase{}`
 * / `Scene{}` so the LP can always be assembled.  Provide explicit entries in
 * these arrays only when you need multiple phases or scenes.
 *
 * Multiple JSON files can be merged with `Planning::merge()`, allowing the
 * time structure to be split across files (e.g. blocks in one file, stages
 * in another).
 */
struct Simulation
{
  Array<Block> block_array {};  ///< Ordered list of time blocks
  Array<Stage> stage_array {};  ///< Ordered list of planning stages
  Array<Scenario> scenario_array {};  ///< List of stochastic scenarios
  Array<Phase> phase_array {};  ///< List of planning phases
  Array<Scene> scene_array {};  ///< List of scene combinations
  Array<Aperture> aperture_array {};  ///< Aperture definitions for SDDP
                                      ///< backward pass (optional)
  Array<Iteration> iteration_array {};  ///< Per-iteration solver control
                                        ///< (optional, keyed by index)

  constexpr void merge(Simulation&& sim)
  {
    gtopt::merge(block_array, std::move(sim.block_array));
    gtopt::merge(stage_array, std::move(sim.stage_array));
    gtopt::merge(scenario_array, std::move(sim.scenario_array));
    gtopt::merge(phase_array, std::move(sim.phase_array));
    gtopt::merge(scene_array, std::move(sim.scene_array));
    gtopt::merge(aperture_array, std::move(sim.aperture_array));
    gtopt::merge(iteration_array, std::move(sim.iteration_array));

    auto _ = std::move(sim);  // move/clear sim
  }
};

}  // namespace gtopt
