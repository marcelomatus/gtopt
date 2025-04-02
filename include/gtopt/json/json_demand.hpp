/**
 * @file      json_demand.hpp
 * @brief     Header of
 * @date      Fri Mar 28 17:12:24 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/demand.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{

using gtopt::DemandAttrs;

template<>
struct json_data_contract<DemandAttrs>
{
  using type = json_member_list<
      json_variant<"bus", SingleId>,
      json_variant_null<"lmax", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"lossfactor", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"fcost", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"emin", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"ecost", OptTRealFieldSched, jvtl_TRealFieldSched>,
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

  constexpr static auto to_json_data(DemandAttrs const& attrs)
  {
    return std::forward_as_tuple(attrs.bus,
                                 attrs.lmax,
                                 attrs.lossfactor,
                                 attrs.fcost,
                                 attrs.emin,
                                 attrs.ecost,
                                 attrs.capacity,
                                 attrs.expcap,
                                 attrs.expmod,
                                 attrs.capmax,
                                 attrs.annual_capcost,
                                 attrs.annual_derating);
  }
};

using gtopt::Demand;

template<>
struct json_data_contract<Demand>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"bus", SingleId>,
      json_variant_null<"lmax", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"lossfactor", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"fcost", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"emin", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"ecost", OptTRealFieldSched, jvtl_TRealFieldSched>,
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

  constexpr static auto to_json_data(Demand const& demand)
  {
    return std::forward_as_tuple(demand.uid,
                                 demand.name,
                                 demand.active,
                                 demand.bus,
                                 demand.lmax,
                                 demand.lossfactor,
                                 demand.fcost,
                                 demand.emin,
                                 demand.ecost,
                                 demand.capacity,
                                 demand.expcap,
                                 demand.expmod,
                                 demand.capmax,
                                 demand.annual_capcost,
                                 demand.annual_derating);
  }
};

using gtopt::DemandVar;
using gtopt::OptDemandVar;
using jvtl_DemandVar =
    json_variant_type_list<Uid, Name, json_class_no_name<DemandAttrs>>;

}  // namespace daw::json
