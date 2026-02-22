#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_line.hpp>

TEST_CASE("Line JSON basic parsing")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"LINE_1",
    "bus_a":10,
    "bus_b":20
  })";

  gtopt::Line line = daw::json::from_json<gtopt::Line>(json_data);

  REQUIRE(line.uid == 5);
  REQUIRE(line.name == "LINE_1");
  REQUIRE(std::get<gtopt::Uid>(line.bus_a) == 10);
  REQUIRE(std::get<gtopt::Uid>(line.bus_b) == 20);
  REQUIRE_FALSE(line.capacity.has_value());
}

TEST_CASE("Line JSON with optional fields")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"LINE_1",
    "bus_a":10,
    "bus_b":20,
    "voltage":230.0,
    "resistance":0.05,
    "reactance":0.15,
    "capacity":200.0
  })";

  const gtopt::Line line = daw::json::from_json<gtopt::Line>(json_data);

  REQUIRE(line.uid == 5);
  REQUIRE(line.name == "LINE_1");
  REQUIRE(line.voltage.has_value());
  REQUIRE(line.resistance.has_value());
  REQUIRE(line.reactance.has_value());
  REQUIRE(line.capacity.has_value());
}

TEST_CASE("Line JSON array parsing")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"LINE_1",
    "bus_a":10,
    "bus_b":20
  },{
    "uid":6,
    "name":"LINE_2",
    "bus_a":20,
    "bus_b":30,
    "capacity":200.0
  }])";

  std::vector<gtopt::Line> lines =
      daw::json::from_json_array<gtopt::Line>(json_data);

  REQUIRE(lines.size() == 2);
  REQUIRE(lines[0].uid == 5);
  REQUIRE(lines[0].name == "LINE_1");
  REQUIRE_FALSE(lines[0].capacity.has_value());

  REQUIRE(lines[1].uid == 6);
  REQUIRE(lines[1].name == "LINE_2");
  REQUIRE(lines[1].capacity.has_value());
}

TEST_CASE("Line JSON with active schedule")
{
  std::string_view json_data = R"({
    "uid":1,
    "name":"LINE_1",
    "bus_a":1,
    "bus_b":2,
    "active":[1,0,1,0]
  })";

  Line line = daw::json::from_json<Line>(json_data);

  CHECK(line.active.has_value());
  if (line.active.has_value()) {
    const auto& active = std::get<std::vector<IntBool>>(line.active.value());
    REQUIRE(active.size() == 4);
    CHECK(active[0] == True);
    CHECK(active[1] == False);
    CHECK(active[2] == True);
    CHECK(active[3] == False);
  }
}

TEST_CASE("Line JSON roundtrip serialization")
{
  gtopt::Line original;
  original.uid = 7;
  original.name = "ROUNDTRIP";
  original.bus_a = gtopt::SingleId(static_cast<gtopt::Uid>(1));
  original.bus_b = gtopt::SingleId(static_cast<gtopt::Uid>(2));
  original.capacity = 100.0;

  auto json = daw::json::to_json(original);
  gtopt::Line roundtrip = daw::json::from_json<gtopt::Line>(json);

  REQUIRE(roundtrip.uid == 7);
  REQUIRE(roundtrip.name == "ROUNDTRIP");
  REQUIRE(std::get<gtopt::Uid>(roundtrip.bus_a) == 1);
  REQUIRE(std::get<gtopt::Uid>(roundtrip.bus_b) == 2);
  CHECK(roundtrip.capacity.has_value());
  CHECK(std::get<double>(roundtrip.capacity.value_or(-1.0)) == 100.0);
}

TEST_CASE("Line with empty optional fields")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"LINE_1",
    "bus_a":10,
    "bus_b":20,
    "voltage":null,
    "resistance":null,
    "reactance":null,
    "capacity":null
  })";

  const Line line = daw::json::from_json<Line>(json_data);

  CHECK(line.uid == 5);
  CHECK(line.name == "LINE_1");
  CHECK(line.voltage.has_value() == false);
  CHECK(line.resistance.has_value() == false);
  CHECK(line.reactance.has_value() == false);
  CHECK(line.capacity.has_value() == false);
}
