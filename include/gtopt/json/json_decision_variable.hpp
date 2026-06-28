/**
 * @file      json_decision_variable.hpp
 * @brief     JSON serialization for DecisionVariable objects
 * @date      Mon May 19 21:55:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides JSON serialization / deserialization for DecisionVariable
 * objects using DAW JSON.  Fields: uid, name, optional active,
 * optional lower_bound / upper_bound / cost.
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/decision_variable.hpp>
#include <gtopt/json/json_field_sched.hpp>

namespace daw::json
{
using gtopt::DecisionVariable;

template<>
struct json_data_contract<DecisionVariable>
{
  using type =
      json_member_list<json_number<"uid", Uid>,
                       json_string<"name", Name>,
                       json_variant_null<"active", OptActive, jvtl_Active>,
                       json_string_null<"type", OptName>,
                       json_string_null<"description", OptName>,
                       json_number_null<"lower_bound", OptReal>,
                       json_number_null<"upper_bound", OptReal>,
                       json_number_null<"cost", OptReal>,
                       json_string_null<"cost_type", OptName>,
                       json_string_null<"scope", OptName>,
                       json_number_null<"block", OptUid>,
                       json_bool_null<"state", OptBool>,
                       json_bool_null<"link", OptBool>,
                       json_bool_null<"block_state", OptBool>,
                       json_number_null<"initial_value", OptReal>,
                       json_number_null<"obj_constant", OptReal>>;

  constexpr static auto to_json_data(DecisionVariable const& dv)
  {
    return std::forward_as_tuple(dv.uid,
                                 dv.name,
                                 dv.active,
                                 dv.type,
                                 dv.description,
                                 dv.lower_bound,
                                 dv.upper_bound,
                                 dv.cost,
                                 dv.cost_type,
                                 dv.scope,
                                 dv.block,
                                 dv.state,
                                 dv.link,
                                 dv.block_state,
                                 dv.initial_value,
                                 dv.obj_constant);
  }
};

}  // namespace daw::json
