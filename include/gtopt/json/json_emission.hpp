/**
 * @file      json_emission.hpp
 * @brief     JSON serialization for Emission
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
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
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant_null<"price", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"cap", OptTRealFieldSched, jvtl_TRealFieldSched>,
      json_variant_null<"cap_cost", OptTRealFieldSched, jvtl_TRealFieldSched>>;

  constexpr static auto to_json_data(Emission const& obj)
  {
    return std::forward_as_tuple(
        obj.uid, obj.name, obj.active, obj.price, obj.cap, obj.cap_cost);
  }
};

}  // namespace daw::json
