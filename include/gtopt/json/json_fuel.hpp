/**
 * @file      json_fuel.hpp
 * @brief     JSON serialization for Fuel + FuelEmissionFactor
 * @date      2026-05-16
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/fuel.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::Fuel;
using gtopt::FuelEmissionFactor;

// Per-pollutant emission-factor row on Fuel.emission_factors[].

template<>
struct json_data_contract<FuelEmissionFactor>
{
  using type = json_member_list<
      json_variant<"emission", SingleId>,
      json_variant_null<"combustion", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"upstream", OptTRealFieldSched, jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(FuelEmissionFactor const& f)
  {
    return std::forward_as_tuple(f.emission, f.combustion, f.upstream);
  }
};

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
                        jvtl_TRealFieldSched>,
      json_array_null<"emission_factors",
                      gtopt::Array<FuelEmissionFactor>,
                      FuelEmissionFactor>>;

  constexpr static auto to_json_data(Fuel const& obj)
  {
    return std::forward_as_tuple(obj.uid,
                                 obj.name,
                                 obj.active,
                                 obj.price,
                                 obj.heat_content,
                                 obj.combustion_emission_factor,
                                 obj.upstream_emission_factor,
                                 obj.emission_factors);
  }
};

}  // namespace daw::json
