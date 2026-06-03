/**
 * @file      json_line_commitment.hpp
 * @brief     JSON serialization for LineCommitment (issue #509)
 * @date      2026-06-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/line_commitment.hpp>

namespace daw::json
{
using gtopt::LineCommitment;

template<>
struct json_data_contract<LineCommitment>
{
  using type =
      json_member_list<json_number<"uid", Uid>,
                       json_string<"name", Name>,
                       json_variant_null<"active", OptActive, jvtl_Active>,
                       json_string_null<"type", OptName>,
                       json_string_null<"description", OptName>,
                       json_variant<"line", SingleId>,
                       json_number_null<"initial_status", OptReal>,
                       json_bool_null<"must_run", OptBool>,
                       json_variant_null<"fixed_status",
                                         OptTBRealFieldSched,
                                         jvtl_TBRealFieldSched>,
                       json_bool_null<"relax", OptBool>,
                       json_number_null<"kvl_big_m", OptReal>,
                       json_number_null<"startup_cost", OptReal>,
                       json_number_null<"shutdown_cost", OptReal>>;

  constexpr static auto to_json_data(LineCommitment const& obj)
  {
    return std::forward_as_tuple(obj.uid,
                                 obj.name,
                                 obj.active,
                                 obj.type,
                                 obj.description,
                                 obj.line,
                                 obj.initial_status,
                                 obj.must_run,
                                 obj.fixed_status,
                                 obj.relax,
                                 obj.kvl_big_m,
                                 obj.startup_cost,
                                 obj.shutdown_cost);
  }
};

}  // namespace daw::json
