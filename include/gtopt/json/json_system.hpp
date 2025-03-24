/**
 * @file      json_system.hpp
 * @brief     Header of
 * @date      Wed Mar 19 22:42:24 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <daw/json/daw_json_link.h>
#include <daw/json/daw_json_link_types.h>
#include <daw/json/impl/daw_json_link_types_fwd.h>
#include <gtopt/json/json_bus.hpp>
#include <gtopt/system.hpp>

namespace daw::json
{

using gtopt::System;

template<>
struct json_data_contract<System>
{
  using type = json_member_list<json_string_null<"name", Name>,
                                json_string_null<"version", Name>,
                                json_array_null<"bus_array", Array<Bus>, Bus>>;

  constexpr static auto to_json_data(System const& system)
  {
    return std::forward_as_tuple(system.name, system.version, system.bus_array);
  }
};

}  // namespace daw::json
