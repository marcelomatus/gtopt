/**
 * @file      json_user_constraint.hpp
 * @brief     JSON serialization/deserialization for UserConstraint
 * @date      Wed Mar 12 03:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/user_constraint.hpp>

namespace daw::json
{

using gtopt::UserConstraint;

template<>
struct json_data_contract<UserConstraint>
{
  using type = json_member_list<json_number<"uid", Uid>,
                                json_string<"name", Name>,
                                json_bool_null<"active", OptBool>,
                                json_string<"expression", Name>,
                                json_string_null<"description", OptName>>;

  [[nodiscard]] constexpr static auto to_json_data(UserConstraint const& uc)
  {
    return std::forward_as_tuple(
        uc.uid, uc.name, uc.active, uc.expression, uc.description);
  }
};

}  // namespace daw::json
