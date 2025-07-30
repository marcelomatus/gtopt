#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/line.hpp>

namespace daw::json
{
using gtopt::Line;

template<>
struct json_data_contract<Line>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"bus_a", SingleId>,
      json_variant<"bus_b", SingleId>,
      json_variant_null<"voltage", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"resistance", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"reactance", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"lossfactor", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"tmax_ba", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"tmax_ab", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"tcost", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"capacity", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"expcap", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"expmod", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"capmax", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"annual_capcost",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"annual_derating",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(Line const& line)
  {
    return std::forward_as_tuple(line.uid,
                                 line.name,
                                 line.active,
                                 line.bus_a,
                                 line.bus_b,
                                 line.voltage,
                                 line.resistance,
                                 line.reactance,
                                 line.lossfactor,
                                 line.tmax_ba,
                                 line.tmax_ab,
                                 line.tcost,
                                 line.capacity,
                                 line.expcap,
                                 line.expmod,
                                 line.capmax,
                                 line.annual_capcost,
                                 line.annual_derating);
  }
};
}  // namespace daw::json
