/**
 * @file      json_commitment.hpp
 * @brief     JSON serialization for Commitment
 * @date      Tue Apr  8 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/commitment.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::Commitment;

template<>
struct json_data_contract<Commitment>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant<"generator", SingleId>,
      json_variant_null<"startup_cost",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_variant_null<"shutdown_cost",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_number_null<"noload_cost", OptReal>,
      json_number_null<"min_up_time", OptReal>,
      json_number_null<"min_down_time", OptReal>,
      json_number_null<"ramp_up", OptReal>,
      json_number_null<"ramp_down", OptReal>,
      json_number_null<"startup_ramp", OptReal>,
      json_number_null<"shutdown_ramp", OptReal>,
      json_number_null<"initial_status", OptReal>,
      json_number_null<"initial_hours", OptReal>,
      json_bool_null<"relax", OptBool>,
      json_bool_null<"must_run", OptBool>,
      json_number_null<"commitment_period", OptReal>,
      json_array_null<"pmax_segments", Array<Real>, json_number_no_name<Real>>,
      json_array_null<"heat_rate_segments",
                      Array<Real>,
                      json_number_no_name<Real>>,
      json_variant_null<"fuel_cost", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"fuel_emission_factor",
                        OptTRealFieldSched,
                        jvtl_TRealFieldSched>,
      json_number_null<"hot_start_cost", OptReal>,
      json_number_null<"warm_start_cost", OptReal>,
      json_number_null<"cold_start_cost", OptReal>,
      json_number_null<"hot_start_time", OptReal>,
      json_number_null<"cold_start_time", OptReal>>;

  constexpr static auto to_json_data(Commitment const& obj)
  {
    return std::forward_as_tuple(obj.uid,
                                 obj.name,
                                 obj.active,
                                 obj.generator,
                                 obj.startup_cost,
                                 obj.shutdown_cost,
                                 obj.noload_cost,
                                 obj.min_up_time,
                                 obj.min_down_time,
                                 obj.ramp_up,
                                 obj.ramp_down,
                                 obj.startup_ramp,
                                 obj.shutdown_ramp,
                                 obj.initial_status,
                                 obj.initial_hours,
                                 obj.relax,
                                 obj.must_run,
                                 obj.commitment_period,
                                 obj.pmax_segments,
                                 obj.heat_rate_segments,
                                 obj.fuel_cost,
                                 obj.fuel_emission_factor,
                                 obj.hot_start_cost,
                                 obj.warm_start_cost,
                                 obj.cold_start_cost,
                                 obj.hot_start_time,
                                 obj.cold_start_time);
  }
};

}  // namespace daw::json
