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
using gtopt::OptStartsScope;

// Two-shape lookup for ``Commitment.starts_scope``: either an
// integer hour count (variant alternative 0 — gtopt::Int) or a named
// scope string ("week", "day", ...) (variant alternative 1 —
// gtopt::Name).  daw::json picks the alternative matching the JSON
// token type (number → Int, string → Name).
using jvtl_StartsScope = json_variant_type_list<Int, Name>;

template<>
struct json_data_contract<Commitment>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_string_null<"type", OptName>,
      json_string_null<"description", OptName>,
      json_variant<"generator", SingleId>,
      json_variant_null<"startup_cost",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"shutdown_cost",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_number_null<"noload_cost", OptReal>,
      json_variant_null<"pmin", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_number_null<"min_up_time", OptReal>,
      json_number_null<"min_down_time", OptReal>,
      json_variant_null<"ramp_up", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_variant_null<"ramp_down",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"startup_ramp",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_variant_null<"shutdown_ramp",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_number_null<"initial_status", OptReal>,
      json_number_null<"initial_hours", OptReal>,
      json_number_null<"ini_hours_up", OptReal>,
      json_number_null<"ini_hours_down", OptReal>,
      json_number_null<"initial_power", OptReal>,
      json_bool_null<"relax", OptBool>,
      json_bool_null<"must_run", OptBool>,
      json_variant_null<"fixed_status",
                        OptTBRealFieldSched,
                        jvtl_TBRealFieldSched>,
      json_number_null<"commitment_period", OptReal>,
      json_number_null<"hot_start_cost", OptReal>,
      json_number_null<"warm_start_cost", OptReal>,
      json_number_null<"cold_start_cost", OptReal>,
      json_number_null<"hot_start_time", OptReal>,
      json_number_null<"cold_start_time", OptReal>,
      json_number_null<"max_starts", OptInt>,
      json_number_null<"min_starts", OptInt>,
      json_variant_null<"starts_scope", OptStartsScope, jvtl_StartsScope>>;

  constexpr static auto to_json_data(Commitment const& obj)
  {
    return std::forward_as_tuple(obj.uid,
                                 obj.name,
                                 obj.active,
                                 obj.type,
                                 obj.description,
                                 obj.generator,
                                 obj.startup_cost,
                                 obj.shutdown_cost,
                                 obj.noload_cost,
                                 obj.pmin,
                                 obj.min_up_time,
                                 obj.min_down_time,
                                 obj.ramp_up,
                                 obj.ramp_down,
                                 obj.startup_ramp,
                                 obj.shutdown_ramp,
                                 obj.initial_status,
                                 obj.initial_hours,
                                 obj.ini_hours_up,
                                 obj.ini_hours_down,
                                 obj.initial_power,
                                 obj.relax,
                                 obj.must_run,
                                 obj.fixed_status,
                                 obj.commitment_period,
                                 obj.hot_start_cost,
                                 obj.warm_start_cost,
                                 obj.cold_start_cost,
                                 obj.hot_start_time,
                                 obj.cold_start_time,
                                 obj.max_starts,
                                 obj.min_starts,
                                 obj.starts_scope);
  }
};

}  // namespace daw::json
