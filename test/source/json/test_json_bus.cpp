#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_bus.hpp>

TEST_CASE("Bus daw json test 1")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"CRUCERO",
    })";

  gtopt::Bus bus_a = daw::json::from_json<gtopt::Bus>(json_data);

  REQUIRE(bus_a.uid == 5);
  REQUIRE(bus_a.name == "CRUCERO");
}

TEST_CASE("Bus daw json test 2")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"CRUCERO",
    })";

  gtopt::Bus bus_a = daw::json::from_json<gtopt::Bus>(json_data);

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
