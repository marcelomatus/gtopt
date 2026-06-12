/**
 * @file      json_hydrogen_storage.hpp
 * @brief     JSON serialisation for ``HydrogenStorage`` objects
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Mirror of ``json_thermal_storage.hpp`` for hydrogen storage.  Only
 * difference is the carrier-side field name (``hydrogen_node`` vs
 * ``thermal_node``).
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/hydrogen_storage.hpp>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::HydrogenStorage;

template<>
struct json_data_contract<HydrogenStorage>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_string_null<"type", OptName>,
      json_variant_null<"hydrogen_node", OptSingleId, jvtl_SingleId>,
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
      HydrogenStorage const& hs) noexcept
  {
    return std::forward_as_tuple(hs.uid,
                                 hs.name,
                                 hs.active,
                                 hs.type,
                                 hs.hydrogen_node,
                                 hs.input_efficiency,
                                 hs.output_efficiency,
                                 hs.annual_loss,
                                 hs.emin,
                                 hs.emax,
                                 hs.ecost,
                                 hs.eini,
                                 hs.efin,
                                 hs.efin_cost,
                                 hs.soft_emin,
                                 hs.soft_emin_cost,
                                 hs.capacity,
                                 hs.expcap,
                                 hs.expmod,
                                 hs.capmax,
                                 hs.annual_capcost,
                                 hs.annual_derating,
                                 hs.integer_expmod,
                                 hs.use_state_variable,
                                 hs.daily_cycle);
  }
};
}  // namespace daw::json
