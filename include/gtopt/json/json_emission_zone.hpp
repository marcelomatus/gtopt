/**
 * @file      json_emission_zone.hpp
 * @brief     JSON serialization for EmissionZone
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/emission_zone.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::EmissionZone;

template<>
struct json_data_contract<EmissionZone>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"emission", SingleId>,
      json_variant_null<"cap", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"cap_cost", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"price", OptTRealFieldSched, jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(EmissionZone const& z)
  {
    return std::forward_as_tuple(
        z.uid, z.name, z.active, z.emission, z.cap, z.cap_cost, z.price);
  }
};

}  // namespace daw::json
