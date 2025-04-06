#pragma once

#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/reserve_zone.hpp>

namespace daw::json
{
using gtopt::ReserveZone;

template<>
struct json_data_contract<ReserveZone>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant_null<"urreq", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"drreq", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"urcost", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"drcost", OptTRealFieldSched, jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(ReserveZone const& obj)
  {
    return std::forward_as_tuple(obj.uid,
                                 obj.name,
                                 obj.active,
                                 obj.urreq,
                                 obj.drreq,
                                 obj.urcost,
                                 obj.drcost);
  }
};

}  // namespace daw::json
