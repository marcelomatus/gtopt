#pragma once

#include <list>
#include <string>

#include <daw/json/daw_json_link.h>
#include <gtopt/bus.hpp>
#include <gtopt/json/json_basic_types.hpp>

namespace daw::json
{

using gtopt::Bus;

template<>
struct json_data_contract<Bus>
{
  using type = json_member_list<json_number<"uid", Uid>,
                                json_string<"name", Name>,
                                json_number_null<"voltage", OptReal>,
                                json_number_null<"theta_ref", OptReal>,
                                json_bool_null<"use_kirchhoff", OptBool>>;

  constexpr static auto to_json_data(Bus const& bus)
  {
    return std::forward_as_tuple(
        bus.uid, bus.name, bus.voltage, bus.theta_ref, bus.use_kirchhoff);
  }
};

}  // namespace daw::json
