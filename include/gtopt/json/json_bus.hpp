#pragma once

#include <list>
#include <string>

#include <daw/json/daw_json_link.h>
#include <gtopt/bus.hpp>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_field_sched.hpp>

namespace daw::json
{

using gtopt::Bus;

template<>
struct json_data_contract<Bus>
{
  using type = json_member_list<
      json_number<"uid", Uid>,
      json_string<"name", Name>,
      json_variant_null<"active", OptActiveFieldSched, jvtl_ActiveFieldSched>,
      json_number_null<"voltage", OptReal>,
      json_number_null<"reference_theta", OptReal>,
      json_bool_null<"skip_kirchhoff", OptBool>>;

  constexpr static auto to_json_data(Bus const& bus)
  {
    return std::forward_as_tuple(bus.uid,
                                 bus.name,
                                 bus.active,
                                 bus.voltage,
                                 bus.reference_theta,
                                 bus.skip_kirchhoff);
  }
};

}  // namespace daw::json
