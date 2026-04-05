#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_battery.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

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

TEST_CASE("Battery daily_cycle JSON round-trip")  // NOLINT
{
  SUBCASE("absent -> nullopt (LP default true)")
  {
    std::string_view json_data = R"({"uid":1,"name":"b1"})";
    const Battery bat = daw::json::from_json<Battery>(json_data);
    CHECK_FALSE(bat.daily_cycle.has_value());
    // Battery LP defaults to daily_cycle=true when not set
    CHECK(bat.daily_cycle.value_or(true) == true);
  }

  SUBCASE("explicit true -> enabled")
  {
    std::string_view json_data = R"({"uid":1,"name":"b1","daily_cycle":true})";
    const Battery bat = daw::json::from_json<Battery>(json_data);
    REQUIRE(bat.daily_cycle.has_value());
    CHECK(bat.daily_cycle.value_or(false) == true);
  }

  SUBCASE("explicit false -> disabled")
  {
    std::string_view json_data = R"({"uid":1,"name":"b1","daily_cycle":false})";
    const Battery bat = daw::json::from_json<Battery>(json_data);
    REQUIRE(bat.daily_cycle.has_value());
    CHECK(bat.daily_cycle.value_or(true) == false);
  }

  SUBCASE("null -> nullopt")
  {
    std::string_view json_data = R"({"uid":1,"name":"b1","daily_cycle":null})";
    const Battery bat = daw::json::from_json<Battery>(json_data);
    CHECK_FALSE(bat.daily_cycle.has_value());
  }
}
