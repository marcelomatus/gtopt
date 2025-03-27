#include <doctest/doctest.h>
#include <gtopt/bus.hpp>
#include <gtopt/json/json_bus.hpp>

TEST_CASE("Bus")
{
  using namespace gtopt;

  Bus bus(1, "bus_1");

  CHECK(bus.uid == 1);
  CHECK(bus.name == "bus_1");
}

TEST_CASE("Json Bus 1")
{
  using namespace gtopt;

  std::string_view json_data = R"({
    "uid":5,
    "name":"CRUCERO",
    })";

  Bus bus = daw::json::from_json<Bus>(json_data);

  CHECK(bus.uid == 5);
  CHECK(bus.name == "CRUCERO");
  CHECK(bus.voltage.has_value() == false);
  CHECK(bus.reference_theta.has_value() == false);
  CHECK(bus.use_kirchhoff.has_value() == false);
}

TEST_CASE("Json Bus 2")
{
  using namespace gtopt;

  std::string_view json_data = R"({
    "uid":5,
    "name":"CRUCERO",
    "voltage":200,
    "reference_theta":1,
    "use_kirchhoff":true,
    })";

  Bus bus = daw::json::from_json<Bus>(json_data);

  CHECK(bus.uid == 5);
  CHECK(bus.name == "CRUCERO");
  CHECK(bus.voltage.value() == 200);
  CHECK(bus.reference_theta.value() == 1);
  CHECK(bus.use_kirchhoff.value() == true);
}
