/**
 * @file      json_scenario.hpp
 * @brief     JSON serialization for Scenario
 * @date      Sun Mar 30 17:33:28 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides the daw::json data contract specialization for
 * serializing and deserializing Scenario objects to and from JSON.
 */

#pragma once

#include <gtopt/json/json_basic_types.hpp>

#include "gtopt/scenario.hpp"

namespace daw::json
{
using gtopt::Scenario;

template<>
struct json_data_contract<Scenario>
{
  using type = json_member_list<json_number<"uid", Uid>,
                                json_string_null<"name", OptName>,
                                json_number_null<"active", OptBool>,
                                json_number_null<"probability_factor", OptReal>,
                                json_number_null<"hydrology", OptInt>>;

  constexpr static auto to_json_data(Scenario const& scenario)
  {
    return std::forward_as_tuple(scenario.uid,
                                 scenario.name,
                                 scenario.active,
                                 scenario.probability_factor,
                                 scenario.hydrology);
  }
};

}  // namespace daw::json
