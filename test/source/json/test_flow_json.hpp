#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_flow.hpp>

using namespace gtopt;

TEST_CASE("Flow JSON basic parsing")
{
  const std::string_view json_data = R"({
    "uid": 1,
    "name": "inflow",
    "direction": 1,
    "junction": 10,
    "discharge": 25.5
  })";

  const Flow flow = daw::json::from_json<Flow>(json_data);

  CHECK(flow.uid == 1);
  CHECK(flow.name == "inflow");
  REQUIRE(flow.direction.has_value());
  CHECK(flow.direction.value_or(0) == 1);
  CHECK(std::get<Uid>(flow.junction) == 10);
  CHECK(std::get<Real>(flow.discharge) == doctest::Approx(25.5));
}

TEST_CASE("Flow JSON default values")
{
  const std::string_view json_data = R"({
    "uid": 2,
    "name": "outflow",
    "junction": 5,
    "discharge": 10.0
  })";

  const Flow flow = daw::json::from_json<Flow>(json_data);

  CHECK(flow.uid == 2);
  CHECK(flow.name == "outflow");
  // direction defaults to 1 (input) when not specified
}

TEST_CASE("Flow JSON round-trip serialization")
{
  Flow original;
  original.uid = 7;
  original.name = "nat_inflow";
  original.direction = -1;
  original.junction = Uid {3};
  original.discharge = 42.0;

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const Flow roundtrip = daw::json::from_json<Flow>(json);  // NOLINT
  CHECK(roundtrip.uid == 7);
  CHECK(roundtrip.name == "nat_inflow");
  REQUIRE(roundtrip.direction.has_value());
  CHECK(roundtrip.direction.value_or(0) == -1);
  CHECK(std::get<Uid>(roundtrip.junction) == 3);
  CHECK(std::get<Real>(roundtrip.discharge) == doctest::Approx(42.0));
}

TEST_CASE("Flow JSON array parsing")
{
  const std::string_view json_data = R"([
    {"uid": 1, "name": "f1", "junction": 1, "discharge": 10.0},
    {"uid": 2, "name": "f2", "direction": -1, "junction": 2, "discharge": 5.0}
  ])";

  const auto flows = daw::json::from_json_array<Flow>(json_data);

  REQUIRE(flows.size() == 2);
  CHECK(flows[0].uid == 1);
  CHECK(flows[1].uid == 2);
  REQUIRE(flows[1].direction.has_value());
  CHECK(flows[1].direction.value_or(0) == -1);
}

TEST_CASE("Flow is_input method")
{
  Flow flow_in;
  flow_in.direction = 1;
  CHECK(flow_in.is_input());

  Flow flow_out;
  flow_out.direction = -1;
  CHECK_FALSE(flow_out.is_input());

  Flow flow_default;
  flow_default.direction = std::nullopt;
  CHECK(flow_default.is_input());  // defaults to input (1)
}
