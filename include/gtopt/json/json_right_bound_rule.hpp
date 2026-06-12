/**
 * @file      json_right_bound_rule.hpp
 * @brief     JSON serialization for RightBoundRule and RightBoundSegment
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * All fields are nullable in JSON (missing ≡ use C++ default).
 * A custom constructor_t converts the nullable parse results back to
 * non-optional RightBoundRule members via value_or().
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/enum_option.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/right_bound_rule.hpp>

namespace daw::json
{
using gtopt::BoundRuleAxis;
using gtopt::OptName;
using gtopt::RightBoundRule;
using gtopt::RightBoundSegment;

template<>
struct json_data_contract<RightBoundSegment>
{
  using type = json_member_list<json_number<"volume", Real>,
                                json_number<"slope", Real>,
                                json_number<"constant", Real>>;

  static constexpr auto to_json_data(RightBoundSegment const& seg)
  {
    return std::forward_as_tuple(seg.volume, seg.slope, seg.constant);
  }
};

/// Constructs RightBoundRule from nullable JSON fields, applying defaults.
struct RightBoundRuleConstructor
{
  RightBoundRule operator()(const OptSingleId& reservoir,
                            std::vector<RightBoundSegment> segments,
                            OptReal cap,
                            OptReal floor,
                            const OptName& axis_str) const
  {
    auto axis = BoundRuleAxis::reservoir_volume;
    if (axis_str.has_value() && !axis_str->empty()) {
      axis = gtopt::require_enum<BoundRuleAxis>("axis", *axis_str);
    }
    return RightBoundRule {
        .reservoir = reservoir.value_or(gtopt::SingleId {gtopt::unknown_uid}),
        .segments = std::move(segments),
        .cap = cap,
        .floor = floor,
        .axis = axis,
    };
  }
};

template<>
struct json_data_contract<RightBoundRule>
{
  using constructor_t = RightBoundRuleConstructor;

  using type = json_member_list<
      json_variant_null<"reservoir", OptSingleId, jvtl_SingleId>,
      json_array_null<"segments",
                      std::vector<RightBoundSegment>,
                      RightBoundSegment>,
      json_number_null<"cap", OptReal>,
      json_number_null<"floor", OptReal>,
      json_string_null<"axis", OptName>>;

  static auto to_json_data(RightBoundRule const& rule)
  {
    return std::make_tuple(OptSingleId {rule.reservoir},
                           rule.segments,
                           rule.cap,
                           rule.floor,
                           OptName {std::string {gtopt::enum_name(rule.axis)}});
  }
};

}  // namespace daw::json
