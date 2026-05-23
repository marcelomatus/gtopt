/**
 * @file      json_ammonia_storage.hpp
 * @brief     JSON serialisation for ``AmmoniaStorage`` objects
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Mirror of ``json_hydrogen_storage.hpp`` for ammonia.  Only the
 * carrier-side field name differs (``ammonia_node``).
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/ammonia_storage.hpp>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::AmmoniaStorage;

template<>
struct json_data_contract<AmmoniaStorage>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_string_null<"type", OptName>,
      json_variant_null<"ammonia_node", OptSingleId, jvtl_SingleId>,
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
      AmmoniaStorage const& as) noexcept
  {
    return std::forward_as_tuple(as.uid,
                                 as.name,
                                 as.active,
                                 as.type,
                                 as.ammonia_node,
                                 as.input_efficiency,
                                 as.output_efficiency,
                                 as.annual_loss,
                                 as.emin,
                                 as.emax,
                                 as.ecost,
                                 as.eini,
                                 as.efin,
                                 as.efin_cost,
                                 as.soft_emin,
                                 as.soft_emin_cost,
                                 as.capacity,
                                 as.expcap,
                                 as.expmod,
                                 as.capmax,
                                 as.annual_capcost,
                                 as.annual_derating,
                                 as.integer_expmod,
                                 as.use_state_variable,
                                 as.daily_cycle);
  }
};
}  // namespace daw::json
