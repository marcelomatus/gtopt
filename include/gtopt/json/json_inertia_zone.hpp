/**
 * @file      json_inertia_zone.hpp
 * @brief     JSON serialization for InertiaZone
 * @date      2026-04-13
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/inertia_zone.hpp>
#include <gtopt/json/json_field_sched.hpp>

namespace daw::json
{
using gtopt::InertiaZone;

template<>
struct json_data_contract<InertiaZone>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant_null<"requirement",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"cost", OptTRealFieldSched, jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(InertiaZone const& obj)
  {
    return std::forward_as_tuple(
        obj.uid, obj.name, obj.active, obj.requirement, obj.cost);
  }
};

}  // namespace daw::json
