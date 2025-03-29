/**
 * @file      json_capacity.hpp
 * @brief     Header of
 * @date      Fri Mar 28 17:12:24 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/capacity.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::CapacityId;

template<>
struct json_data_contract<CapacityId>
{
  using type = json_member_list<json_number_null<"uid", OptUid>,
                                json_string_null<"name", OptName>>;

  constexpr static auto to_json_data(CapacityId const& capacity)
  {
    return std::forward_as_tuple(capacity.uid, capacity.name);
  }
};

using gtopt::CapacityValue;
using gtopt::OptCapacityValue;

using jvtl_CapacityValue =
    json_variant_type_list<Real, json_class_no_name<CapacityId>>;

using gtopt::CapacityAttrs;

template<>
struct json_data_contract<CapacityAttrs>
{
  using type = json_member_list<
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
  constexpr static auto to_json_data(CapacityAttrs const& attrs)
  {
    return std::forward_as_tuple(attrs.capacity,
                                 attrs.expcap,
                                 attrs.expmod,
                                 attrs.capmax,
                                 attrs.annual_capcost,
                                 attrs.annual_derating);
  }
};

using gtopt::Capacity;

template<>
struct json_data_contract<Capacity>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
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
  constexpr static auto to_json_data(Capacity const& capacity)
  {
    return std::forward_as_tuple(capacity.uid,
                                 capacity.name,
                                 capacity.active,
                                 capacity.capacity,
                                 capacity.expcap,
                                 capacity.expmod,
                                 capacity.capmax,
                                 capacity.annual_capcost,
                                 capacity.annual_derating);
  }
};

}  // namespace daw::json
