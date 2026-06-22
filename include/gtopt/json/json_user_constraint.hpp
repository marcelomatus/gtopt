/**
 * @file      json_user_constraint.hpp
 * @brief     JSON serialization/deserialization for UserConstraint
 * @date      Wed Mar 12 03:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/constraint_directive.hpp>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/user_constraint.hpp>

namespace daw::json
{

using gtopt::ConstraintDirective;
using gtopt::DirectiveKind;
using gtopt::OptConstraintDirective;
using gtopt::OptTBRealFieldSched;
using gtopt::UserConstraint;

/// JSON binding for the discriminator enum.  Stored as a lowercase string
/// (``"regrange"``, ``"daily_budget"``, …) — symmetric with how every other
/// NamedEnum is serialised in gtopt (``MaxStartsScope``, ``LineLossesMode``,
/// …) and avoids leaking the numeric ``uint8_t`` value into the wire form.
struct DirectiveKindStringConverter
{
  [[nodiscard]] static constexpr auto operator()(std::string_view name) noexcept
      -> DirectiveKind
  {
    if (const auto v = gtopt::enum_from_name<DirectiveKind>(name);
        v.has_value())
    {
      return *v;
    }
    // Default to RegRange so a malformed enum doesn't silently parse as
    // a different family.  The post-parse ``valid_for_kind()`` check in
    // ``UserConstraintLP`` is the canonical guard against bad inputs.
    return DirectiveKind::RegRange;
  }

  [[nodiscard]] static constexpr auto operator()(DirectiveKind kind) noexcept
      -> std::string_view
  {
    return gtopt::enum_name(kind);
  }
};

template<>
struct json_data_contract<ConstraintDirective>
{
  using type =
      json_member_list<json_custom<"kind",
                                   DirectiveKind,
                                   DirectiveKindStringConverter,
                                   DirectiveKindStringConverter>,
                       json_number_null<"penalty", gtopt::OptReal>,
                       json_string_null<"scope", gtopt::OptName>,
                       json_number_null<"window_hours", gtopt::OptInt>>;

  [[nodiscard]] constexpr static auto to_json_data(ConstraintDirective const& d)
  {
    return std::forward_as_tuple(d.kind, d.penalty, d.scope, d.window_hours);
  }
};

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
      json_variant_null<"rhs", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_class_null<"directive", OptConstraintDirective>,
      json_string_null<"scope", gtopt::OptName>,
      json_string_null<"slack_name", gtopt::OptName>>;

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
                                 uc.rhs,
                                 uc.directive,
                                 uc.scope,
                                 uc.slack_name);
  }
};

}  // namespace daw::json
