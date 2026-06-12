#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/pump.hpp>

namespace daw::json
{
using gtopt::Pump;

template<>
struct json_data_contract<Pump>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"waterway", SingleId>,
      json_variant<"demand", SingleId>,
      json_variant_null<"pump_factor",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"efficiency", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"capacity", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"main_reservoir", OptSingleId, jvtl_SingleId>>;

  constexpr static auto to_json_data(Pump const& pump)
  {
    return std::forward_as_tuple(pump.uid,
                                 pump.name,
                                 pump.active,
                                 pump.waterway,
                                 pump.demand,
                                 pump.pump_factor,
                                 pump.efficiency,
                                 pump.capacity,
                                 pump.main_reservoir);
  }
};
}  // namespace daw::json
