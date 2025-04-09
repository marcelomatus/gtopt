/**
 * @file      json_phase.hpp
 * @brief     Header of
 * @date      Sun Mar 30 17:33:28 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/phase.hpp>

namespace daw::json
{
using gtopt::Phase;

template<>
struct json_data_contract<Phase>
{
  using type = json_member_list<json_number<"uid", Uid>,
                                json_string_null<"name", OptName>,
                                json_number_null<"active", OptBool>,
                                json_number<"first_stage", Size>,
                                json_number<"count_stage", Size>>;

  constexpr static auto to_json_data(Phase const& phase)
  {
    return std::forward_as_tuple(phase.uid,
                                 phase.name,
                                 phase.active,
                                 phase.first_stage,
                                 phase.count_stage);
  }
};
}  // namespace daw::json
