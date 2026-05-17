/**
 * @file      json_fuel.hpp
 * @brief     JSON serialization for Fuel
 * @date      2026-05-16
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/fuel.hpp>
#include <gtopt/json/json_field_sched.hpp>

namespace daw::json
{
using gtopt::Fuel;

template<>
struct json_data_contract<Fuel>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant_null<"price", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"heat_content",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"combustion_emission_factor",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"upstream_emission_factor",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(Fuel const& obj)
  {
    return std::forward_as_tuple(obj.uid,
                                 obj.name,
                                 obj.active,
                                 obj.price,
                                 obj.heat_content,
                                 obj.combustion_emission_factor,
                                 obj.upstream_emission_factor);
  }
};

}  // namespace daw::json
