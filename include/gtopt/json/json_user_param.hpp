/**
 * @file      json_user_param.hpp
 * @brief     JSON serialization for UserParam
 * @date      Wed Apr  2 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/user_param.hpp>

namespace daw::json
{

using gtopt::Real;
using gtopt::UserParam;

template<>
struct json_data_contract<UserParam>
{
  using type =
      json_member_list<json_string<"name", gtopt::Name>,
                       json_number_null<"value", gtopt::OptReal>,
                       json_array_null<"monthly",
                                       std::optional<std::vector<Real>>,
                                       json_number_no_name<Real>>>;

  [[nodiscard]] constexpr static auto to_json_data(UserParam const& p)
  {
    return std::forward_as_tuple(p.name, p.value, p.monthly);
  }
};

}  // namespace daw::json
