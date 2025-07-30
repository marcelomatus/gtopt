#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_waterway.hpp>

TEST_CASE("Waterway JSON basic parsing")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"RIVER_1",
    "junction_a":10,
    "junction_b":20
  })";

  gtopt::Waterway waterway = daw::json::from_json<gtopt::Waterway>(json_data);

  REQUIRE(waterway.uid == 5);
  REQUIRE(waterway.name == "RIVER_1");
  REQUIRE(waterway.junction_a.value() == 10);
  REQUIRE(waterway.junction_b.value() == 20);
  REQUIRE_FALSE(waterway.capacity.has_value());
}

TEST_CASE("Waterway JSON with optional fields")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"RIVER_1",
    "junction_a":10,
    "junction_b":20,
    "capacity":100.5,
    "lossfactor":0.05,
    "fmin":-50.0,
    "fmax":100.0
  })";

  gtopt::Waterway waterway = daw::json::from_json<gtopt::Waterway>(json_data);

  REQUIRE(waterway.uid == 5);
  REQUIRE(waterway.name == "RIVER_1");
  REQUIRE(waterway.capacity.has_value());
  REQUIRE(waterway.lossfactor.has_value());
  REQUIRE(waterway.fmin.has_value());
  REQUIRE(waterway.fmax.has_value());
}

TEST_CASE("Waterway JSON array parsing")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"RIVER_1",
    "junction_a":10,
    "junction_b":20
  },{
    "uid":6,
    "name":"RIVER_2",
    "junction_a":20,
    "junction_b":30,
    "capacity":200.0
  }])";

  std::vector<gtopt::Waterway> waterways =
      daw::json::from_json_array<gtopt::Waterway>(json_data);

  REQUIRE(waterways.size() == 2);
  REQUIRE(waterways[0].uid == 5);
  REQUIRE(waterways[0].name == "RIVER_1");
  REQUIRE_FALSE(waterways[0].capacity.has_value());

  REQUIRE(waterways[1].uid == 6);
  REQUIRE(waterways[1].name == "RIVER_2");
  REQUIRE(waterways[1].capacity.has_value());
}

TEST_CASE("Waterway JSON with active schedule")
{
  using namespace gtopt;

  std::string_view json_data = R"({
    "uid":1,
    "name":"CANAL_1",
    "junction_a":1,
    "junction_b":2,
    "active":[1,0,1,0]
  })";

  Waterway waterway = daw::json::from_json<Waterway>(json_data);

  REQUIRE(waterway.active.has_value());
  const auto& active = std::get<std::vector<IntBool>>(waterway.active.value());
  REQUIRE(active.size() == 4);
  CHECK(active[0] == True);
  CHECK(active[1] == False);
  CHECK(active[2] == True);
  CHECK(active[3] == False);
}

TEST_CASE("Waterway JSON roundtrip serialization")
{
  gtopt::Waterway original;
  original.uid = 7;
  original.name = "ROUNDTRIP";
  original.junction_a = 1;
  original.junction_b = 2;
  original.capacity = 100.0;

  auto json = daw::json::to_json(original);
  gtopt::Waterway roundtrip = daw::json::from_json<gtopt::Waterway>(json);

  REQUIRE(roundtrip.uid == 7);
  REQUIRE(roundtrip.name == "ROUNDTRIP");
  REQUIRE(roundtrip.junction_a.value() == 1);
  REQUIRE(roundtrip.junction_b.value() == 2);
  REQUIRE(roundtrip.capacity.has_value());
  REQUIRE(roundtrip.capacity.value() == 100.0);
}

TEST_CASE("Waterway with empty optional fields")
{
  using namespace gtopt;

  std::string_view json_data = R"({
    "uid":5,
    "name":"RIVER_1",
    "junction_a":10,
    "junction_b":20,
    "capacity":null,
    "lossfactor":null,
    "fmin":null,
    "fmax":null
  })";

  Waterway waterway = daw::json::from_json<Waterway>(json_data);

  CHECK(waterway.uid == 5);
  CHECK(waterway.name == "RIVER_1");
  CHECK(waterway.capacity.has_value() == false);
  CHECK(waterway.lossfactor.has_value() == false);
  CHECK(waterway.fmin.has_value() == false);
  CHECK(waterway.fmax.has_value() == false);
}
