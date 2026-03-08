#pragma once

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_battery.hpp>

TEST_CASE("Battery use_state_variable JSON round-trip")  // NOLINT
{
  SUBCASE("absent -> nullopt (decoupled by convention)")
  {
    std::string_view json_data = R"({"uid":1,"name":"b1"})";
    const Battery bat = daw::json::from_json<Battery>(json_data);
    CHECK_FALSE(bat.use_state_variable.has_value());
    // Battery LP defaults to decoupled when not set
    CHECK(bat.use_state_variable.value_or(false) == false);
  }

  SUBCASE("explicit true -> coupled")
  {
    std::string_view json_data =
        R"({"uid":1,"name":"b1","use_state_variable":true})";
    const Battery bat = daw::json::from_json<Battery>(json_data);
    REQUIRE(bat.use_state_variable.has_value());
    CHECK(bat.use_state_variable.value_or(false) == true);
  }

  SUBCASE("explicit false -> decoupled")
  {
    std::string_view json_data =
        R"({"uid":1,"name":"b1","use_state_variable":false})";
    const Battery bat = daw::json::from_json<Battery>(json_data);
    REQUIRE(bat.use_state_variable.has_value());
    CHECK(bat.use_state_variable.value_or(true) == false);
  }

  SUBCASE("null -> nullopt")
  {
    std::string_view json_data =
        R"({"uid":1,"name":"b1","use_state_variable":null})";
    const Battery bat = daw::json::from_json<Battery>(json_data);
    CHECK_FALSE(bat.use_state_variable.has_value());
  }
}
