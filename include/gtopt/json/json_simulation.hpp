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

#include <gtopt/json/json_block.hpp>
#include <gtopt/json/json_phase.hpp>
#include <gtopt/json/json_scenario.hpp>
#include <gtopt/json/json_scene.hpp>
#include <gtopt/json/json_stage.hpp>
#include <gtopt/json/json_system.hpp>
#include <gtopt/simulation.hpp>

namespace daw::json
{

using gtopt::Simulation;

template<>
struct json_data_contract<Simulation>
{
  using type = json_member_list<
      json_array_null<"block_array", Array<Block>, Block>,
      json_array_null<"stage_array", Array<Stage>, Stage>,
      json_array_null<"scenario_array", Array<Scenario>, Scenario>,
      json_array_null<"phase_array", Array<Phase>, Phase>,
      json_array_null<"scene_array", Array<Scene>, Scene>>;

  [[nodiscard]] constexpr static auto to_json_data(Simulation const& simulation)
  {
    return std::forward_as_tuple(simulation.block_array,
                                 simulation.stage_array,
                                 simulation.scenario_array,
                                 simulation.phase_array,
                                 simulation.scene_array);
  }
};
}  // namespace daw::json
