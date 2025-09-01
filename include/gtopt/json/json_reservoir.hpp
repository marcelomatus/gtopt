#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/reservoir.hpp>

namespace daw::json
{
using gtopt::Reservoir;

template<>
struct json_data_contract<Reservoir>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"junction", SingleId>,
      json_number_null<"spillway_capacity", OptReal>,
      json_number_null<"spillway_cost", OptReal>,
      json_variant_null<"capacity", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"annual_loss",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"vmin", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"vmax", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"vcost", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_number_null<"vini", OptReal>,
      json_number_null<"vfin", OptReal>,
      json_number_null<"fmin", OptReal>,
      json_number_null<"fmax", OptReal>,
      json_number_null<"vol_scale", OptReal>,
      json_number_null<"flow_conversion_rate", OptReal>>;

  constexpr static auto to_json_data(Reservoir const& reservoir)
  {
    return std::forward_as_tuple(reservoir.uid,
                                 reservoir.name,
                                 reservoir.active,
                                 reservoir.junction,
                                 reservoir.spillway_capacity,
                                 reservoir.spillway_cost,
                                 reservoir.capacity,
                                 reservoir.annual_loss,
                                 reservoir.vmin,
                                 reservoir.vmax,
                                 reservoir.vcost,
                                 reservoir.vini,
                                 reservoir.vfin,
                                 reservoir.fmin,
                                 reservoir.fmax,
                                 reservoir.vol_scale,
                                 reservoir.flow_conversion_rate);
  }
};
}  // namespace daw::json
