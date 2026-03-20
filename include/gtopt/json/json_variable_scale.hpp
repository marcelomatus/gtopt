/**
 * @file      json_variable_scale.hpp
 * @brief     JSON serialization for VariableScale
 * @date      Mon Mar 17 04:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/variable_scale.hpp>

namespace daw::json
{
using gtopt::VariableScale;

template<>
struct json_data_contract<VariableScale>
{
  using type = json_member_list<json_string<"class_name", gtopt::Name>,
                                json_string<"variable", gtopt::Name>,
                                json_number<"uid", gtopt::Uid>,
                                json_number<"scale", gtopt::Real>,
                                json_string_null<"name", gtopt::Name>>;

  constexpr static auto to_json_data(VariableScale const& vs)
  {
    return std::forward_as_tuple(
        vs.class_name, vs.variable, vs.uid, vs.scale, vs.name);
  }
};

}  // namespace daw::json
