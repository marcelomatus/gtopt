/**
 * @file      json_plant.hpp
 * @brief     JSON serialization for Plant objects
 * @date      Sat May 30 12:55:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides JSON serialization / deserialization for `Plant` objects
 * using DAW JSON.  Required fields: ``uid``, ``name``,
 * ``generator_names``.  Optional fields: ``active``, ``pmax``,
 * ``n_units``, ``commit_coeffs``, ``uniq_mutex``.
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/plant.hpp>

namespace daw::json
{

using gtopt::Plant;

template<>
struct json_data_contract<Plant>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_array_null<"generator_names", Array<Name>, std::string>,
      json_number_null<"pmax", OptReal>,
      json_number_null<"n_units", OptInt>,
      json_array_null<"commit_coeffs", Array<Real>, json_number_no_name<Real>>,
      json_bool_null<"uniq_mutex", OptBool>>;

  constexpr static auto to_json_data(Plant const& plant)
  {
    return std::forward_as_tuple(plant.uid,
                                 plant.name,
                                 plant.active,
                                 plant.generator_names,
                                 plant.pmax,
                                 plant.n_units,
                                 plant.commit_coeffs,
                                 plant.uniq_mutex);
  }
};

}  // namespace daw::json
