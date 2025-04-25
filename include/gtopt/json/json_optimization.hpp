/**
 * @file      json_optimization.hpp
 * @brief     JSON serialization/deserialization for the Optimization class
 * @date      Wed Mar 19 22:42:24 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides JSON data contract definitions for the Optimization
 * class, enabling serialization and deserialization of optimization objects.
 */

#pragma once

#include <gtopt/json/json_options.hpp>
#include <gtopt/json/json_simulation.hpp>
#include <gtopt/json/json_system.hpp>
#include <gtopt/optimization.hpp>

namespace daw::json
{

using gtopt::Optimization;

template<>
struct json_data_contract<Optimization>
{
  using type = json_member_list<json_class_null<"options", Options>,
                                json_class_null<"simulation", Simulation>,
                                json_class_null<"system", System>>;

  [[nodiscard]] constexpr static auto to_json_data(
      Optimization const& optimization)
  {
    return std::forward_as_tuple(
        optimization.options, optimization.simulation, optimization.system);
  }
};
}  // namespace daw::json
