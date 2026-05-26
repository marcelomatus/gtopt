/**
 * @file      json_emission.hpp
 * @brief     JSON serialization for Emission (pollutant tag)
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Emission carries only `uid` + `name` + `active`.  Cap / price /
 * slack moved to `EmissionZone`; per-generator rates moved to
 * `EmissionSource`.
 */

#pragma once

#include <gtopt/emission.hpp>
#include <gtopt/json/json_field_sched.hpp>

namespace daw::json
{
using gtopt::Emission;

template<>
struct json_data_contract<Emission>
{
  using type =
      json_member_list<json_number<"uid", Uid>,
                       json_string<"name", Name>,
                       json_variant_null<"active", OptActive, jvtl_Active>>;

  constexpr static auto to_json_data(Emission const& e)
  {
    return std::forward_as_tuple(e.uid, e.name, e.active);
  }
};

}  // namespace daw::json
