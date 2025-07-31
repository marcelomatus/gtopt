#pragma once

#include <daw/json/daw_json_link.h>

#include "daw_json_capacity.hpp"
#include "reservoir.hpp"

namespace daw::json
{
using fesop::Reservoir;

template<>
struct json_data_contract<Reservoir>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"junction", SingleId>,
      json_variant_null<"capacity", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"annual_loss",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"vmin", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"vmax", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"vcost", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_number_null<"vini", OptReal>,
      json_number_null<"vfin", OptReal>>;

  constexpr static auto to_json_data(Reservoir const& reservoir)
  {
    return std::forward_as_tuple(reservoir.uid,
                                 reservoir.name,
                                 reservoir.active,
                                 reservoir.junction,
                                 reservoir.capacity,
                                 reservoir.annual_loss,
                                 reservoir.vmin,
                                 reservoir.vmax,
                                 reservoir.vcost,
                                 reservoir.vini,
                                 reservoir.vfin);
  }
};
}  // namespace daw::json
