/**
 * @file      json_flow.hpp
 * @brief     Header of
 * @date      Wed Jul 30 21:56:08 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/flow.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/json/json_single_id.hpp>

namespace daw::json
{
using gtopt::Flow;

template<>
struct json_data_contract<Flow>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_number<"direction", Int>,
      json_variant<"junction", SingleId>,
      json_variant<"discharge", STBRealFieldSched, jvtl_STBRealFieldSched>>;

  constexpr static auto to_json_data(Flow const& flow)
  {
    return std::forward_as_tuple(flow.uid,
                                 flow.name,
                                 flow.active,
                                 flow.direction,
                                 flow.junction,
                                 flow.discharge);
  }
};
}  // namespace daw::json
