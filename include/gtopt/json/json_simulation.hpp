/**
 * @file      json_simulation.hpp
 * @brief     JSON serialization/deserialization for the Simulation class
 * @date      Wed Mar 19 22:42:24 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides JSON data contract definitions for the Simulation class,
 * enabling serialization and deserialization of simulation objects with all
 * their components including buses, generators, lines, etc.
 */

#pragma once

#include <gtopt/json/json_aperture.hpp>
#include <gtopt/json/json_block.hpp>
#include <gtopt/json/json_iteration.hpp>
#include <gtopt/json/json_phase.hpp>
#include <gtopt/json/json_scenario.hpp>
#include <gtopt/json/json_scene.hpp>
#include <gtopt/json/json_stage.hpp>
#include <gtopt/json/json_system.hpp>
#include <gtopt/simulation.hpp>

namespace daw::json
{

using gtopt::Simulation;

/// Custom constructor: converts JSON strings → typed enums for Simulation
struct SimulationConstructor
{
  [[nodiscard]] Simulation operator()(Array<Block> block_array,
                                      Array<Stage> stage_array,
                                      Array<Scenario> scenario_array,
                                      Array<Phase> phase_array,
                                      Array<Scene> scene_array,
                                      Array<Aperture> aperture_array,
                                      Array<Iteration> iteration_array,
                                      OptReal annual_discount_rate,
                                      OptName boundary_cuts_file,
                                      OptName boundary_cuts_valuation_str,
                                      OptName probability_rescale_str,
                                      OptName kappa_warning_str,
                                      OptReal kappa_threshold) const
  {
    Simulation sim;
    sim.block_array = std::move(block_array);
    sim.stage_array = std::move(stage_array);
    sim.scenario_array = std::move(scenario_array);
    sim.phase_array = std::move(phase_array);
    sim.scene_array = std::move(scene_array);
    sim.aperture_array = std::move(aperture_array);
    sim.iteration_array = std::move(iteration_array);
    sim.annual_discount_rate = annual_discount_rate;
    sim.boundary_cuts_file = std::move(boundary_cuts_file);
    if (boundary_cuts_valuation_str) {
      sim.boundary_cuts_valuation =
          gtopt::require_enum<gtopt::BoundaryCutsValuation>(
              "boundary_cuts_valuation", *boundary_cuts_valuation_str);
    }
    if (probability_rescale_str) {
      sim.probability_rescale =
          gtopt::require_enum<gtopt::ProbabilityRescaleMode>(
              "probability_rescale", *probability_rescale_str);
    }
    if (kappa_warning_str) {
      sim.kappa_warning = gtopt::require_enum<gtopt::KappaWarningMode>(
          "kappa_warning", *kappa_warning_str);
    }
    sim.kappa_threshold = kappa_threshold;
    return sim;
  }
};

template<>
struct json_data_contract<Simulation>
{
  using constructor_t = SimulationConstructor;

  using type = json_member_list<
      json_array_null<"block_array", Array<Block>, Block>,
      json_array_null<"stage_array", Array<Stage>, Stage>,
      json_array_null<"scenario_array", Array<Scenario>, Scenario>,
      json_array_null<"phase_array", Array<Phase>, Phase>,
      json_array_null<"scene_array", Array<Scene>, Scene>,
      json_array_null<"aperture_array", Array<Aperture>, Aperture>,
      json_array_null<"iteration_array", Array<Iteration>, Iteration>,
      json_number_null<"annual_discount_rate", OptReal>,
      json_string_null<"boundary_cuts_file", OptName>,
      json_string_null<"boundary_cuts_valuation", OptName>,
      json_string_null<"probability_rescale", OptName>,
      json_string_null<"kappa_warning", OptName>,
      json_number_null<"kappa_threshold", OptReal>>;

  static auto to_json_data(Simulation const& simulation)
  {
    return std::make_tuple(
        simulation.block_array,
        simulation.stage_array,
        simulation.scenario_array,
        simulation.phase_array,
        simulation.scene_array,
        simulation.aperture_array,
        simulation.iteration_array,
        simulation.annual_discount_rate,
        simulation.boundary_cuts_file,
        detail::enum_to_opt_name(simulation.boundary_cuts_valuation),
        detail::enum_to_opt_name(simulation.probability_rescale),
        detail::enum_to_opt_name(simulation.kappa_warning),
        simulation.kappa_threshold);
  }
};
}  // namespace daw::json
