/**
 * @file      json_scene.hpp
 * @brief     JSON serialization for Scene objects
 * @date      Sun Mar 30 17:33:28 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides JSON serialization and deserialization for Scene objects
 * using the daw::json library.
 */

#pragma once

#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/scene.hpp>

namespace daw::json
{
using gtopt::Scene;

template<>
struct json_data_contract<Scene>
{
  using type = json_member_list<json_number<"uid", Uid>,
                                json_string_null<"name", OptName>,
                                json_number_null<"active", OptBool>,
                                json_number<"first_scenery", Size>,
                                json_number<"count_scenery", Size>>;

  [[nodiscard]] constexpr static auto to_json_data(Scene const& scene)
  {
    return std::forward_as_tuple(scene.uid,
                                 scene.name,
                                 scene.active,
                                 scene.first_scenery,
                                 scene.count_scenery);
  }
};
}  // namespace daw::json
