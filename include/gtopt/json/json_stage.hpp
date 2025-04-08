/**
 * @file      json_stage.hpp
 * @brief     Header of
 * @date      Sun Mar 30 17:33:28 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/stage.hpp>

namespace daw::json
{
using gtopt::Stage;

template<>
struct json_data_contract<Stage>
{
  using type = json_member_list<json_number<"uid", Uid>,
                                json_string_null<"name", OptName>,
                                json_number_null<"active", OptBool>,
                                json_number<"first_block", Size>,
                                json_number<"count_block", Size>,
                                json_number_null<"discount_factor", OptReal>>;

  constexpr static auto to_json_data(Stage const& stage)
  {
    return std::forward_as_tuple(stage.uid,
                                 stage.name,
                                 stage.active,
                                 stage.first_block,
                                 stage.count_block,
                                 stage.discount_factor);
  }
};
}  // namespace daw::json
