/**
 * @file      json_right_junction.hpp
 * @brief     JSON serialization for RightJunction objects
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_single_id.hpp>
#include <gtopt/right_junction.hpp>

namespace daw::json
{
using gtopt::RightJunction;

template<>
struct json_data_contract<RightJunction>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActive, jvtl_Active>,
      json_variant_null<"junction", OptSingleId, jvtl_SingleId>,
      json_bool_null<"drain", OptBool>>;

  constexpr static auto to_json_data(RightJunction const& rj)
  {
    return std::forward_as_tuple(
        rj.uid, rj.name, rj.active, rj.junction, rj.drain);
  }
};
}  // namespace daw::json
