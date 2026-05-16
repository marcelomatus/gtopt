#pragma once

#include <gtopt/json/json_reserve_zone.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/reserve_provision.hpp>

namespace daw::json
{
using gtopt::Array;
using gtopt::ReserveProvision;
using gtopt::String;

template<>
struct json_data_contract<ReserveProvision>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"generator", SingleId>,
      // Typed array of ReserveZone references — each element is a
      // Uid (number) or Name (string).  Replaces the legacy
      // colon/comma-delimited string form: `"reserve_zones": [1,
      // "ZONE_A"]` rather than `"reserve_zones": "1:ZONE_A"`.
      json_array_null<"reserve_zones",
                      Array<SingleId>,
                      json_variant_no_name<SingleId, jvtl_SingleId>>,
      json_variant_null<"urmax", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"drmax", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"ur_capacity_factor",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"dr_capacity_factor",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"ur_provision_factor",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"dr_provision_factor",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"urcost", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"drcost", OptTRealFieldSched, jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(ReserveProvision const& obj)
  {
    return std::forward_as_tuple(obj.uid,
                                 obj.name,
                                 obj.active,
                                 obj.generator,
                                 obj.reserve_zones,
                                 obj.urmax,
                                 obj.drmax,
                                 obj.ur_capacity_factor,
                                 obj.dr_capacity_factor,
                                 obj.ur_provision_factor,
                                 obj.dr_provision_factor,
                                 obj.urcost,
                                 obj.drcost);
  }
};

}  // namespace daw::json
