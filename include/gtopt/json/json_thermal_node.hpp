/**
 * @file      json_thermal_node.hpp
 * @brief     JSON serialisation for ``ThermalNode``
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * ``ThermalNode`` is a minimal carrier-tagged balance-node identifier
 * (uid + name + active).  The compile-time ``carrier`` tag and the
 * ``class_name`` constant are not serialised — they're implicit in
 * the JSON array name (``thermal_node_array``).
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/thermal_node.hpp>

namespace daw::json
{
using gtopt::ThermalNode;

template<>
struct json_data_contract<ThermalNode>
{
  using type =
      json_member_list<json_number<"uid", Uid>,
                       json_string<"name", Name>,
                       json_variant_null<"active", OptActive, jvtl_Active>>;

  [[nodiscard]] constexpr static auto to_json_data(
      ThermalNode const& tn) noexcept
  {
    return std::forward_as_tuple(tn.uid, tn.name, tn.active);
  }
};
}  // namespace daw::json
