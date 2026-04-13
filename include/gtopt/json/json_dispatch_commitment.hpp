/**
 * @file      json_dispatch_commitment.hpp
 * @brief     JSON serialization for DispatchCommitment
 * @date      2025
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#pragma once

#include <gtopt/dispatch_commitment.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::DispatchCommitment;

template<>
struct json_data_contract<DispatchCommitment>
{
  using type =
      json_member_list<json_number<"uid", Uid>,
                       json_string<"name", Name>,
                       json_variant_null<"active", OptActive, jvtl_Active>,
                       json_variant<"generator", SingleId>,
                       json_variant_null<"dispatch_pmin",
                                         OptTBRealFieldSched,
                                         jvtl_TBRealFieldSched>,
                       json_bool_null<"relax", OptBool>,
                       json_bool_null<"must_run", OptBool>>;

  constexpr static auto to_json_data(DispatchCommitment const& obj)
  {
    return std::forward_as_tuple(obj.uid,
                                 obj.name,
                                 obj.active,
                                 obj.generator,
                                 obj.dispatch_pmin,
                                 obj.relax,
                                 obj.must_run);
  }
};

}  // namespace daw::json
