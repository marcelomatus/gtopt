/**
 * @file      json_block.hpp
 * @brief     Header of
 * @date      Sun Mar 30 17:33:28 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/json/json_basic_types.hpp>

#include "gtopt/scenery.hpp"

namespace daw::json
{
using gtopt::Scenery;

template<>
struct json_data_contract<Scenery>
{
  using type = json_member_list<json_number<"uid", Uid>,
                                json_number_null<"probability_factor", OptReal>,
                                json_number_null<"active", OptBool>,
                                json_string_null<"name", OptName>>;

  constexpr static auto to_json_data(Scenery const& scenery)
  {
    return std::forward_as_tuple(
        scenery.uid, scenery.probability_factor, scenery.active, scenery.name);
  }
};

}  // namespace daw::json
