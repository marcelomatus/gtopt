/**
 * @file      json_user_model.hpp
 * @brief     JSON serialization for UserModel (generic AMPL capture) elements
 * @date      Sun Jun 22 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `variable_array` / `constraint_array` reuse the existing
 * `DecisionVariable` / `UserConstraint` JSON contracts verbatim — no new AST.
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_decision_variable.hpp>
#include <gtopt/json/json_user_constraint.hpp>
#include <gtopt/user_model.hpp>

namespace daw::json
{
using gtopt::DecisionVariable;
using gtopt::UserConstraint;
using gtopt::UserModel;

template<>
struct json_data_contract<UserModel>
{
  using type =
      json_member_list<json_number<"uid", Uid>,
                       json_string<"name", Name>,
                       json_variant_null<"active", OptActive, jvtl_Active>,
                       json_string_null<"description", OptName>,
                       json_string_null<"tag", OptName>,
                       json_array_null<"variable_array",
                                       gtopt::Array<DecisionVariable>,
                                       DecisionVariable>,
                       json_array_null<"constraint_array",
                                       gtopt::Array<UserConstraint>,
                                       UserConstraint>>;

  [[nodiscard]] constexpr static auto to_json_data(UserModel const& um)
  {
    return std::forward_as_tuple(um.uid,
                                 um.name,
                                 um.active,
                                 um.description,
                                 um.tag,
                                 um.variable_array,
                                 um.constraint_array);
  }
};

}  // namespace daw::json
