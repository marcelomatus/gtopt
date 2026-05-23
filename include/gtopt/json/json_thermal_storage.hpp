/**
 * @file      json_thermal_storage.hpp
 * @brief     JSON serialisation for ``ThermalStorage`` objects
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Mirror of ``json_battery.hpp`` for the thermal-carrier storage
 * peer.  Two fields differ from Battery:
 *   * ``thermal_node`` replaces ``bus`` (resolves against
 *     ``thermal_node_array``, not ``bus_array``).
 *   * No ``source_generator`` / ``commitment`` fields (out of MVP
 *     scope; the power-block UC lives on a downstream Generator).
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/thermal_storage.hpp>

namespace daw::json
{
using gtopt::ThermalStorage;

template<>
struct json_data_contract<ThermalStorage>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_string_null<"type", OptName>,
      json_variant_null<"thermal_node", OptSingleId, jvtl_SingleId>,
      json_variant_null<"input_efficiency",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"output_efficiency",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"annual_loss",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"emin", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"emax", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"ecost", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_number_null<"eini", OptReal>,
      json_number_null<"efin", OptReal>,
      json_number_null<"efin_cost", OptReal>,
      json_variant_null<"soft_emin",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"soft_emin_cost",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"capacity", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"expcap", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"expmod", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"capmax", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"annual_capcost",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"annual_derating",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_bool_null<"integer_expmod", OptBool>,
      json_bool_null<"use_state_variable", OptBool>,
      json_bool_null<"daily_cycle", OptBool>>;

  [[nodiscard]] constexpr static auto to_json_data(
      ThermalStorage const& ts) noexcept
  {
    return std::forward_as_tuple(ts.uid,
                                 ts.name,
                                 ts.active,
                                 ts.type,
                                 ts.thermal_node,
                                 ts.input_efficiency,
                                 ts.output_efficiency,
                                 ts.annual_loss,
                                 ts.emin,
                                 ts.emax,
                                 ts.ecost,
                                 ts.eini,
                                 ts.efin,
                                 ts.efin_cost,
                                 ts.soft_emin,
                                 ts.soft_emin_cost,
                                 ts.capacity,
                                 ts.expcap,
                                 ts.expmod,
                                 ts.capmax,
                                 ts.annual_capcost,
                                 ts.annual_derating,
                                 ts.integer_expmod,
                                 ts.use_state_variable,
                                 ts.daily_cycle);
  }
};
}  // namespace daw::json
