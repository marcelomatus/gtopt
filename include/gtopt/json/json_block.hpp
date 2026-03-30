/**
 * @file      json_block.hpp
 * @brief     JSON serialization for Block
 * @date      Sun Mar 30 17:33:28 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides the daw::json data contract specialization for
 * serializing and deserializing Block objects to and from JSON.
 */

#pragma once

#include <gtopt/block.hpp>
#include <gtopt/json/json_basic_types.hpp>

namespace daw::json
{
using gtopt::Block;

template<>
struct json_data_contract<Block>
{
  using type = json_member_list<json_number<"uid", Uid>,
                                json_string_null<"name", OptName>,
                                json_number<"duration", Real>>;

  constexpr static auto to_json_data(Block const& block)
  {
    return std::forward_as_tuple(block.uid, block.name, block.duration);
  }
};

}  // namespace daw::json
