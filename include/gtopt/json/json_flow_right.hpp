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
using gtopt::RightBoundRule;

/// Custom constructor so that json_class_null<"bound_rule"> maps
/// absent/null JSON to std::nullopt rather than a default RightBoundRule.
struct FlowRightConstructor
{
  [[nodiscard]] FlowRight operator()(
      Uid uid,
      Name name,
      OptActive active,
      OptName purpose,
      OptSingleId junction,
      OptInt direction,
      STBRealFieldSched discharge,
      OptTBRealFieldSched fmax,
      OptBool consumptive,
      OptBool use_average,
      OptTBRealFieldSched fail_cost,
      OptTBRealFieldSched use_value,
      OptReal priority,
      std::optional<RightBoundRule> bound_rule) const
  {
    return FlowRight {
        .uid = uid,
        .name = std::move(name),
        .active = std::move(active),
        .purpose = std::move(purpose),
        .junction = std::move(junction),
        .direction = direction,
        .discharge = std::move(discharge),
        .fmax = std::move(fmax),
        .consumptive = consumptive,
        .use_average = use_average,
        .fail_cost = std::move(fail_cost),
        .use_value = std::move(use_value),
        .priority = priority,
        .bound_rule = std::move(bound_rule),
    };
  }
};

template<>
struct json_data_contract<FlowRight>
{
  using constructor_t = FlowRightConstructor;

  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_string_null<"purpose", OptName>,
      json_variant_null<"junction", OptSingleId, jvtl_SingleId>,
      json_number_null<"direction", OptInt>,
      json_variant<"discharge", STBRealFieldSched, jvtl_STBRealFieldSched>,
      json_variant_null<"fmax", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_bool_null<"consumptive", OptBool>,
      json_bool_null<"use_average", OptBool>,
      json_variant_null<"fail_cost",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"use_value",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_number_null<"priority", OptReal>,
      json_class_null<"bound_rule", std::optional<RightBoundRule>>>;

  constexpr static auto to_json_data(FlowRight const& fr)
  {
    return std::forward_as_tuple(fr.uid,
                                 fr.name,
                                 fr.active,
                                 fr.purpose,
                                 fr.junction,
                                 fr.direction,
                                 fr.discharge,
                                 fr.fmax,
                                 fr.consumptive,
                                 fr.use_average,
                                 fr.fail_cost,
                                 fr.use_value,
                                 fr.priority,
                                 fr.bound_rule);
  }
};
}  // namespace daw::json
