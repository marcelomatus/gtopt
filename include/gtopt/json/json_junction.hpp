#pragma once

#include <daw/json/daw_json_link.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_field_sched.hpp>
#include <gtopt/junction.hpp>

namespace daw::json
{
using gtopt::Junction;

template<>
struct json_data_contract<Junction>
{
  using type =
      json_member_list<json_number<"uid", Uid>,
                       json_string<"name", Name>,
                       json_variant_null<"active", OptActive, jvtl_Active>,
                       json_bool_null<"drain", OptBool>>;

  constexpr static auto to_json_data(Junction const& junction)
  {
    return std::forward_as_tuple(
        junction.uid, junction.name, junction.active, junction.drain);
  }
};
}  // namespace daw::json
