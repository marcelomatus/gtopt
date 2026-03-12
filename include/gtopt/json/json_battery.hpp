/**
 * @file      json_battery.hpp
 * @brief     JSON serialization support for Battery objects
 * @date      Wed Apr  2 01:54:12 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides JSON serialization and deserialization capabilities
 * for Battery objects using the DAW JSON Link library. It defines a JSON data
 * contract that maps Battery class members to their corresponding JSON fields,
 * enabling seamless conversion between C++ objects and JSON representations.
 *
 * The contract includes support for optional fields with null values and
 * various numeric and string types that are commonly used in battery
 * configuration and optimization scenarios.
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/battery.hpp>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::Battery;

template<>
struct json_data_contract<Battery>
{
  /// @brief Member list defining the JSON-to-C++ field mapping
  using type = json_member_list<
      json_number<"uid", Uid>,  ///< Unique identifier
      json_string<"name", Name>,  ///< Battery name
      json_variant_null<"active", OptActive, jvtl_Active>,  ///< Activation
                                                            ///< status
      json_string_null<"type", OptName>,  ///< Optional battery type tag
      json_variant_null<"bus",
                        OptSingleId,
                        jvtl_SingleId>,  ///< Bus (unified definition)
      json_variant_null<"source_generator",
                        OptSingleId,
                        jvtl_SingleId>,  ///< Source generator for coupled mode
      json_variant_null<"input_efficiency",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,  ///< Input efficiency schedule
      json_variant_null<"output_efficiency",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,  ///< Output efficiency schedule
      json_variant_null<"annual_loss",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,  ///< Annual energy loss factor
      json_variant_null<"emin",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,  ///< Minimum energy level
      json_variant_null<"emax",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,  ///< Maximum energy level
      json_variant_null<"ecost",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,  ///< Voltage cost
      json_number_null<"eini", OptReal>,  ///< Initial energy (optional)
      json_number_null<"efin", OptReal>,  ///< Final energy (optional)
      json_variant_null<"pmax_charge",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,  ///< Max charging power
      json_variant_null<"pmax_discharge",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,  ///< Max discharging power
      json_variant_null<"gcost",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,  ///< Discharge cost
      json_variant_null<"capacity",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,  ///< Capacity
      json_variant_null<"expcap",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,  ///< Expansion capacity
      json_variant_null<"expmod",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,  ///< Expansion model
      json_variant_null<"capmax",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,  ///< Maximum capacity
      json_variant_null<"annual_capcost",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,  ///< Annual capacity cost
      json_variant_null<"annual_derating",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,  ///< Annual derating factor
      json_bool_null<"use_state_variable", OptBool>,  ///< Stage/phase coupling
      json_bool_null<"daily_cycle", OptBool>,  ///< Daily cycle operation
      json_number_null<"energy_scale", OptReal>  ///< Energy scaling factor
      >;

  /**
   * @brief Converts a Battery object to a tuple for JSON serialization
   *
   * @param battery The Battery object to serialize
   * @return constexpr auto Tuple containing all battery members in the order
   * defined in type
   */
  [[nodiscard]] constexpr static auto to_json_data(Battery const& battery)
  {
    return std::forward_as_tuple(battery.uid,
                                 battery.name,
                                 battery.active,
                                 battery.type,
                                 battery.bus,
                                 battery.source_generator,
                                 battery.input_efficiency,
                                 battery.output_efficiency,
                                 battery.annual_loss,
                                 battery.emin,
                                 battery.emax,
                                 battery.ecost,
                                 battery.eini,
                                 battery.efin,
                                 battery.pmax_charge,
                                 battery.pmax_discharge,
                                 battery.gcost,
                                 battery.capacity,
                                 battery.expcap,
                                 battery.expmod,
                                 battery.capmax,
                                 battery.annual_capcost,
                                 battery.annual_derating,
                                 battery.use_state_variable,
                                 battery.daily_cycle,
                                 battery.energy_scale);
  }
};
}  // namespace daw::json
