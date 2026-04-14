/**
 * @file      json_inertia_provision.hpp
 * @brief     JSON serialization for InertiaProvision
 * @date      2026-04-13
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/inertia_provision.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::InertiaProvision;
using gtopt::String;

template<>
struct json_data_contract<InertiaProvision>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"generator", SingleId>,
      json_string<"inertia_zones", String>,
      json_number_null<"inertia_constant", OptReal>,
      json_number_null<"rated_power", OptReal>,
      json_variant_null<"provision_max",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"provision_factor",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"cost", OptTRealFieldSched, jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(InertiaProvision const& obj)
  {
    return std::forward_as_tuple(obj.uid,
                                 obj.name,
                                 obj.active,
                                 obj.generator,
                                 obj.inertia_zones,
                                 obj.inertia_constant,
                                 obj.rated_power,
                                 obj.provision_max,
                                 obj.provision_factor,
                                 obj.cost);
  }
};

}  // namespace daw::json
