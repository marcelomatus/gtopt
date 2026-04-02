/**
 * @file      json_flow_right.hpp
 * @brief     JSON serialization for FlowRight objects
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/flow_right.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_right_bound_rule.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::FlowRight;

template<>
struct json_data_contract<FlowRight>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_string_null<"purpose", OptName>,
      json_variant_null<"junction", OptSingleId, jvtl_SingleId>,
      json_variant_null<"right_junction", OptSingleId, jvtl_SingleId>,
      json_number_null<"direction", OptInt>,
      json_variant<"discharge", STBRealFieldSched, jvtl_STBRealFieldSched>,
      json_variant_null<"fmax", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_bool_null<"consumptive", OptBool>,
      json_bool_null<"use_average", OptBool>,
      json_number_null<"fail_cost", OptReal>,
      json_number_null<"priority", OptReal>,
      json_class_null<"bound_rule", RightBoundRule>>;

  constexpr static auto to_json_data(FlowRight const& fr)
  {
    return std::forward_as_tuple(fr.uid,
                                 fr.name,
                                 fr.active,
                                 fr.purpose,
                                 fr.junction,
                                 fr.right_junction,
                                 fr.direction,
                                 fr.discharge,
                                 fr.fmax,
                                 fr.consumptive,
                                 fr.use_average,
                                 fr.fail_cost,
                                 fr.priority,
                                 fr.bound_rule);
  }
};
}  // namespace daw::json
