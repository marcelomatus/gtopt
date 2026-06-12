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
  // Strings are bound as `std::string_view` since the destination
  // `VariableScale` fields are `string_view`.  The parser holds a stable
  // source buffer for the lifetime of the parse; daw::json copies the
  // view into the parsed object's string_view field.  Lookup keys in
  // `VariableScaleMap` are constructed as owning `std::string` copies
  // (`variable_scale.hpp:97-117`), so map lifetime is independent of
  // the parser source buffer.
  // `json_string_raw` binds the string member directly as a
  // `std::string_view` slice into the JSON source buffer (no copy /
  // no escape decoding).  Safe here because class names, variable
  // names, and element names are plain identifiers (no escape
  // sequences).  Caller is responsible for keeping the JSON parser's
  // source buffer alive for the lifetime of the resulting Planning
  // (which gtopt does — `PlanningJsonSource` owns the source string).
  using type = json_member_list<json_string_raw<"class_name", std::string_view>,
                                json_string_raw<"variable", std::string_view>,
                                json_number<"uid", gtopt::Uid>,
                                json_number<"scale", gtopt::Real>,
                                json_string_raw_null<"name", std::string_view>>;

  constexpr static auto to_json_data(VariableScale const& vs)
  {
    return std::forward_as_tuple(
        vs.class_name, vs.variable, vs.uid, vs.scale, vs.name);
  }
};

}  // namespace daw::json
