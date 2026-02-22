#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_bus.hpp>

using namespace gtopt;

TEST_CASE("Bus daw json test 1")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"CRUCERO",
    })";

  const gtopt::Bus bus_a = daw::json::from_json<gtopt::Bus>(json_data);

  REQUIRE(bus_a.uid == 5);
  REQUIRE(bus_a.name == "CRUCERO");
}

TEST_CASE("Bus daw json test 2")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"CRUCERO",
    })";

  const gtopt::Bus bus_a = daw::json::from_json<gtopt::Bus>(json_data);

  REQUIRE(bus_a.uid == 5);
  REQUIRE(bus_a.name == "CRUCERO");
}

TEST_CASE("Bus daw json test 3")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"CRUCERO",
    },{
    "uid":10,
    "name":"PTOMONTT",
    }])";

  std::vector<gtopt::Bus> bus_a =
      daw::json::from_json_array<gtopt::Bus>(json_data);

  REQUIRE(bus_a[0].uid == 5);
  REQUIRE(bus_a[0].name == "CRUCERO");

  REQUIRE(bus_a[1].uid == 10);
  REQUIRE(bus_a[1].name == "PTOMONTT");
}

TEST_CASE("Bus daw json test 4")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"CRUCERO",
    "active": [0,0,1,1]
    },{
    "uid":10,
    "name":"PTOMONTT",
    }])";

  std::vector<gtopt::Bus> bus_a =
      daw::json::from_json_array<gtopt::Bus>(json_data);

  REQUIRE(bus_a[0].uid == 5);
  REQUIRE(bus_a[0].name == "CRUCERO");

  REQUIRE(bus_a[1].uid == 10);
  REQUIRE(bus_a[1].name == "PTOMONTT");

  using gtopt::False;
  using gtopt::True;

  const std::vector<gtopt::IntBool> empty;
  const std::vector<gtopt::IntBool> active = {False, False, True, True};
  REQUIRE(std::get<std::vector<gtopt::IntBool>>(bus_a[0].active.value_or(empty))
          == active);
}

TEST_CASE("Bus with active property serialization")
{
  SUBCASE("With boolean active")
  {
    Bus bus(1, "test_bus");
    bus.active = True;

    auto json = daw::json::to_json(bus);
    Bus roundtrip = daw::json::from_json<Bus>(json);

    REQUIRE(roundtrip.active.has_value());
    CHECK(std::get<IntBool>(roundtrip.active.value()) == True);  // NOLINT
  }

  SUBCASE("With schedule active")
  {
    Bus bus(1, "test_bus");
    bus.active = std::vector<IntBool> {True, False, True, False};

    auto json = daw::json::to_json(bus);
    Bus roundtrip = daw::json::from_json<Bus>(json);

    REQUIRE(roundtrip.active.has_value());
    const auto& active =
        std::get<std::vector<IntBool>>(roundtrip.active.value());  // NOLINT
    REQUIRE(active.size() == 4);
    CHECK(active[0] == True);
    CHECK(active[1] == False);
    CHECK(active[2] == True);
    CHECK(active[3] == False);
  }
}

TEST_CASE("Bus with empty optional fields")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"CRUCERO",
    "voltage":null,
    "reference_theta":null,
    "use_kirchhoff":null
    })";

  const Bus bus = daw::json::from_json<Bus>(json_data);

  CHECK(bus.uid == 5);
  CHECK(bus.name == "CRUCERO");
  CHECK(bus.voltage.has_value() == false);
  CHECK(bus.reference_theta.has_value() == false);
  CHECK(bus.use_kirchhoff.has_value() == false);
}
