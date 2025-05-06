/**
 * @file      json_planning.hpp
 * @brief     JSON serialization/deserialization for the Planning class
 * @date      Wed Mar 19 22:42:24 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides JSON data contract definitions for the Planning
 * class, enabling serialization and deserialization of planning objects.
 */

#pragma once

#include <gtopt/json/json_options.hpp>
#include <gtopt/json/json_simulation.hpp>
#include <gtopt/json/json_system.hpp>
#include <gtopt/planning.hpp>

namespace daw::json
{

using gtopt::Planning;

template<>
struct json_data_contract<Planning>
{
  using type = json_member_list<json_class_null<"options", Options>,
                                json_class_null<"simulation", Simulation>,
                                json_class_null<"system", System>>;

  [[nodiscard]] constexpr static auto to_json_data(Planning const& planning)
  {
    return std::forward_as_tuple(
        planning.options, planning.simulation, planning.system);
  }
};
}  // namespace daw::json
