/**
 * @file      json_flow.hpp
 * @brief     JSON serialization for Flow objects
 * @date      Wed Jul 30 21:56:08 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides JSON serialization/deserialization for Flow objects using DAW JSON.
 * Handles all Flow fields including:
 * - UID and name
 * - Active status
 * - Direction
 * - Junction association
 * - Discharge schedule
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/flow.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::Flow;
using gtopt::InflowModel;

template<>
struct json_data_contract<InflowModel>
{
  using type = json_member_list<json_string_null<"type", OptName>,
                                json_number_null<"phi", OptReal>,
                                json_number_null<"sigma", OptReal>>;

  constexpr static auto to_json_data(InflowModel const& im)
  {
    return std::forward_as_tuple(im.type, im.phi, im.sigma);
  }
};

template<>
struct json_data_contract<Flow>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_string_null<"type", OptName>,
      json_string_null<"description", OptName>,
      json_number_null<"direction", OptInt>,
      json_variant_null<"junction", OptSingleId, jvtl_SingleId>,
      json_variant_null<"discharge",
                        OptSTBRealFieldSched,
                        jvtl_STBRealFieldSched>,
      json_variant_null<"fcost", OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      json_class_null<"inflow_model", gtopt::OptInflowModel>>;

  constexpr static auto to_json_data(Flow const& flow)
  {
    return std::forward_as_tuple(flow.uid,
                                 flow.name,
                                 flow.active,
                                 flow.type,
                                 flow.description,
                                 flow.direction,
                                 flow.junction,
                                 flow.discharge,
                                 flow.fcost,
                                 flow.inflow_model);
  }
};
}  // namespace daw::json
