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

TEST_CASE("Bus needs_kirchhoff method")
{
  using namespace gtopt;

  SUBCASE("Default values") {
    Bus bus(1, "bus_1");
    CHECK(bus.needs_kirchhoff(0.5) == true);  // Default is true with default voltage > threshold
  }

  SUBCASE("With explicit true") {
    Bus bus(1, "bus_1");
    bus.use_kirchhoff = true;
    CHECK(bus.needs_kirchhoff(0.5) == true);
  }

  SUBCASE("With explicit false") {
    Bus bus(1, "bus_1");
    bus.use_kirchhoff = false;
    CHECK(bus.needs_kirchhoff(0.5) == false);
  }

  SUBCASE("With low voltage") {
    Bus bus(1, "bus_1");
    bus.voltage = 0.1;
    CHECK(bus.needs_kirchhoff(0.5) == false);
  }

  SUBCASE("With high voltage") {
    Bus bus(1, "bus_1");
    bus.voltage = 1.5;
    CHECK(bus.needs_kirchhoff(0.5) == true);
  }
}

TEST_CASE("Bus serialization")
{
  using namespace gtopt;

  SUBCASE("Minimal bus") {
    Bus bus(1, "bus_1");
    auto json = daw::json::to_json(bus);
    auto roundtrip = daw::json::from_json<Bus>(json);
    
    CHECK(roundtrip.uid == 1);
    CHECK(roundtrip.name == "bus_1");
    CHECK(!roundtrip.voltage.has_value());
    CHECK(!roundtrip.reference_theta.has_value());
    CHECK(!roundtrip.use_kirchhoff.has_value());
  }

  SUBCASE("Full bus") {
    Bus bus(5, "CRUCERO");
    bus.voltage = 200;
    bus.reference_theta = 1;
    bus.use_kirchhoff = true;
    
    auto json = daw::json::to_json(bus);
    Bus roundtrip = daw::json::from_json<Bus>(json);
    
    CHECK(roundtrip.uid == 5);
    CHECK(roundtrip.name == "CRUCERO");
    CHECK(roundtrip.voltage.value() == 200);
    CHECK(roundtrip.reference_theta.value() == 1);
    CHECK(roundtrip.use_kirchhoff.value() == true);
  }
}
