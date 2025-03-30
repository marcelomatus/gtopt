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

#include <gtopt/block.hpp>
#include <gtopt/json/json_basic_types.hpp>

namespace daw::json
{
using gtopt::Block;

template<>
struct json_data_contract<Block>
{
  using type = json_member_list<json_number<"uid", Uid>,
                                json_number<"duration", Real>,
                                json_string_null<"name", OptName>>;

  constexpr static auto to_json_data(Block const& block)
  {
    return std::forward_as_tuple(block.uid, block.duration, block.name);
  }
};

}  // namespace daw::json
