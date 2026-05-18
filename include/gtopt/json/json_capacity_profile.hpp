/**
 * @file      json_profile.hpp
 * @brief     JSON serialization for the unified `CapacityProfile` element
 * @date      2026-05-17
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `CapacityProfile` is serialized as:
 * ```json
 * {
 *   "uid": 1, "name": "g3_solar",
 *   "owner_kind": "generator",
 *   "owner": "g3",
 *   "profile": [...]
 * }
 * ```
 *
 * `owner_kind` is a string parsed via the project-wide `require_enum`
 * helper — fail-fast, ASCII case-insensitive, with an
 * `(expected: generator, demand)` hint on typo.  This matches gtopt's
 * `StrictParsePolicy` convention (no silent fallbacks at parse time).
 *
 * Legacy `demand_profile_array` / `generator_profile_array` JSON keys
 * still resolve to the existing `DemandProfile` / `GeneratorProfile`
 * types; the legacy → unified folding happens in Commit 2.
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/capacity_profile.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::CapacityProfile;
using gtopt::ProfileOwnerKind;

/// Custom constructor: parses the `owner_kind` string into the enum via
/// `require_enum` — throws `std::invalid_argument` with an
/// `(expected: generator, demand)` hint on unknown / typo'd values.
/// Matches gtopt's `StrictParsePolicy` fail-fast convention.
struct CapacityProfileConstructor
{
  [[nodiscard]] CapacityProfile operator()(
      Uid uid,
      Name name,
      OptActive active,
      // NOLINTNEXTLINE(performance-unnecessary-value-param)
      Name owner_kind_str,
      SingleId owner,
      STBRealFieldSched profile,
      OptTRealFieldSched scost) const
  {
    CapacityProfile p;
    p.uid = uid;
    p.name = std::move(name);
    p.active = std::move(active);
    p.owner_kind =
        gtopt::require_enum<ProfileOwnerKind>("owner_kind", owner_kind_str);
    p.owner = std::move(owner);
    p.profile = std::move(profile);
    p.scost = std::move(scost);
    return p;
  }
};

template<>
struct json_data_contract<CapacityProfile>
{
  using constructor_t = CapacityProfileConstructor;

  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_string<"owner_kind", Name>,
      json_variant<"owner", SingleId>,
      json_variant<"profile", STBRealFieldSched, jvtl_STBRealFieldSched>,
      json_variant_null<"scost", OptTRealFieldSched, jvtl_TRealFieldSched>>;

  static auto to_json_data(CapacityProfile const& p)
  {
    return std::make_tuple(p.uid,
                           p.name,
                           p.active,
                           std::string(gtopt::enum_name(p.owner_kind)),
                           p.owner,
                           p.profile,
                           p.scost);
  }
};

}  // namespace daw::json
