/**
 * @file      json_period.hpp
 * @brief     Header of
 * @date      Sun Mar 30 17:33:28 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/period.hpp>

namespace daw::json
{
using gtopt::Period;

template<>
struct json_data_contract<Period>
{
  using type = json_member_list<json_number<"uid", Uid>,
                                json_string_null<"name", OptName>,
                                json_number_null<"active", OptBool>,
                                json_number<"first_stage", Size>,
                                json_number<"count_stage", Size>>;

  constexpr static auto to_json_data(Period const& period)
  {
    return std::forward_as_tuple(period.uid,
                                 period.name,
                                 period.active,
                                 period.first_stage,
                                 period.count_stage);
  }
};
}  // namespace daw::json
