/**
 * @file      json_user_constraint.hpp
 * @brief     JSON serialization/deserialization for UserConstraint
 * @date      Wed Mar 12 03:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/user_constraint.hpp>

namespace daw::json
{

using gtopt::OptTBRealFieldSched;
using gtopt::UserConstraint;

template<>
struct json_data_contract<UserConstraint>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_bool_null<"active", OptBool>,
      json_string<"expression", Name>,
      json_string_null<"description", OptName>,
      json_string_null<"constraint_type", OptName>,
      json_number_null<"penalty", OptReal>,
      json_string_null<"penalty_class", OptName>,
      json_bool_null<"daily_sum", OptBool>,
      json_variant_null<"rhs", OptTBRealFieldSched, jvtl_TBRealFieldSched>>;

  [[nodiscard]] constexpr static auto to_json_data(UserConstraint const& uc)
  {
    return std::forward_as_tuple(uc.uid,
                                 uc.name,
                                 uc.active,
                                 uc.expression,
                                 uc.description,
                                 uc.constraint_type,
                                 uc.penalty,
                                 uc.penalty_class,
                                 uc.daily_sum,
                                 uc.rhs);
  }
};

}  // namespace daw::json
