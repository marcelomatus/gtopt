/**
 * @file      json_hydrogen_node.hpp
 * @brief     JSON serialisation for ``HydrogenNode``
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Mirror of ``json_thermal_node.hpp`` for the hydrogen carrier.
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/hydrogen_node.hpp>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_field_sched.hpp>

namespace daw::json
{
using gtopt::HydrogenNode;

template<>
struct json_data_contract<HydrogenNode>
{
  using type =
      json_member_list<json_number<"uid", Uid>,
                       json_string<"name", Name>,
                       json_variant_null<"active", OptActive, jvtl_Active>>;

  [[nodiscard]] constexpr static auto to_json_data(
      HydrogenNode const& hn) noexcept
  {
    return std::forward_as_tuple(hn.uid, hn.name, hn.active);
  }
};
}  // namespace daw::json
