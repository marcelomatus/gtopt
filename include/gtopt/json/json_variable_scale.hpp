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
  // Strings are bound as OWNING `std::string` (still `json_string_raw`: raw
  // copy, no escape decoding — class/variable/element names are plain
  // identifiers).  They must NOT be `std::string_view`: that binds a
  // zero-copy slice into the transient parse buffer, which
  // `parse_planning_json` frees on return, leaving the `VariableScale`
  // fields dangling (a heap-use-after-free later read by
  // `auto_scale_reservoirs`, caught by ThreadSanitizer).  Copying here is
  // cheap and correct; `VariableScaleMap` still keys off `string_view`s into
  // these now-stable strings.
  using type = json_member_list<json_string_raw<"class_name", std::string>,
                                json_string_raw<"variable", std::string>,
                                json_number<"uid", gtopt::Uid>,
                                json_number<"scale", gtopt::Real>,
                                json_string_raw_null<"name", std::string>>;

  constexpr static auto to_json_data(VariableScale const& vs)
  {
    return std::forward_as_tuple(
        vs.class_name, vs.variable, vs.uid, vs.scale, vs.name);
  }
};

}  // namespace daw::json
