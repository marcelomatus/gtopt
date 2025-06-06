/**
 * @file      json_battery.hpp
 * @brief     Header of
 * @date      Wed Apr  2 01:54:12 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/battery.hpp>
#include <gtopt/json/json_field_sched.hpp>

namespace daw::json
{
using gtopt::Battery;

template<>
struct json_data_contract<Battery>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant_null<"annual_loss",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"vmin", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"vmax", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"vcost", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_number_null<"vini", OptReal>,
      json_number_null<"vfin", OptReal>,
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

  constexpr static auto to_json_data(Battery const& battery)
  {
    return std::forward_as_tuple(battery.uid,
                                 battery.name,
                                 battery.active,
                                 battery.annual_loss,
                                 battery.vmin,
                                 battery.vmax,
                                 battery.vcost,
                                 battery.vini,
                                 battery.vfin,
                                 battery.capacity,
                                 battery.expcap,
                                 battery.expmod,
                                 battery.capmax,
                                 battery.annual_capcost,
                                 battery.annual_derating);
  }
};
}  // namespace daw::json
