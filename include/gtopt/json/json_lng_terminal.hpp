/**
 * @file      json_lng_terminal.hpp
 * @brief     JSON serialization for LngTerminal and LngGeneratorLink
 * @date      Sun Apr 13 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/lng_terminal.hpp>

namespace daw::json
{
using gtopt::LngGeneratorLink;
using gtopt::LngTerminal;

template<>
struct json_data_contract<LngGeneratorLink>
{
  using type = json_member_list<json_variant<"generator", SingleId>,
                                json_number<"heat_rate", Real>>;

  constexpr static auto to_json_data(LngGeneratorLink const& link)
  {
    return std::forward_as_tuple(link.generator, link.heat_rate);
  }
};

template<>
struct json_data_contract<LngTerminal>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant_null<"emin", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"emax", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"ecost", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_number_null<"eini", OptReal>,
      json_number_null<"efin", OptReal>,
      json_variant_null<"annual_loss",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_number_null<"sendout_max", OptReal>,
      json_number_null<"sendout_min", OptReal>,
      json_variant_null<"delivery", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_number_null<"spillway_cost", OptReal>,
      json_number_null<"spillway_capacity", OptReal>,
      json_bool_null<"use_state_variable", OptBool>,
      json_number_null<"mean_production_factor", OptReal>,
      json_variant_null<"scost", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"soft_emin", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"soft_emin_cost",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_number_null<"flow_conversion_rate", OptReal>,
      json_array_null<"generators", Array<LngGeneratorLink>, LngGeneratorLink>>;

  constexpr static auto to_json_data(LngTerminal const& t)
  {
    return std::forward_as_tuple(t.uid,
                                 t.name,
                                 t.active,
                                 t.emin,
                                 t.emax,
                                 t.ecost,
                                 t.eini,
                                 t.efin,
                                 t.annual_loss,
                                 t.sendout_max,
                                 t.sendout_min,
                                 t.delivery,
                                 t.spillway_cost,
                                 t.spillway_capacity,
                                 t.use_state_variable,
                                 t.mean_production_factor,
                                 t.scost,
                                 t.soft_emin,
                                 t.soft_emin_cost,
                                 t.flow_conversion_rate,
                                 t.generators);
  }
};

}  // namespace daw::json
