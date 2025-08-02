#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_reservoir.hpp>
#include <gtopt/reservoir.hpp>

using namespace gtopt;

TEST_CASE("Reservoir basic fields deserialization") 
{
  std::string_view json_data = R"({
    "uid":123,
    "name":"TestReservoir",
    "active":true,
    "junction":456
  })";

  Reservoir res = daw::json::from_json<Reservoir>(json_data);

  REQUIRE(res.uid == 123);
  REQUIRE(res.name == "TestReservoir");
  REQUIRE(res.active == true);
  REQUIRE(std::get<Uid>(res.junction) == 456);
}

TEST_CASE("Reservoir optional fields deserialization")
{
  std::string_view json_data = R"({
    "uid":123,
    "capacity":1.0,
    "annual_loss":0.05,
    "vmin":100.0, 
    "vmax":1000.0,
    "vcost":5.0,
    "vini":500.0,
    "vfin":600.0
  })";

  Reservoir res = daw::json::from_json<Reservoir>(json_data);

  REQUIRE(res.capacity.value() == 1.0);
  REQUIRE(res.annual_loss.value() == 0.05);
  REQUIRE(res.vmin.value() == 100.0);
  REQUIRE(res.vmax.value() == 1000.0); 
  REQUIRE(res.vcost.value() == 5.0);
  REQUIRE(res.vini.value() == 500.0);
  REQUIRE(res.vfin.value() == 600.0);
}

TEST_CASE("Reservoir array deserialization")
{
  std::string_view json_data = R"([{
    "uid":123,
    "name":"ReservoirA",
    "junction":456
  },{
    "uid":124,
    "name":"ReservoirB", 
    "junction":457,
    "capacity":1.0,
    "vmax":1000.0
  }])";

  std::vector<Reservoir> reservoirs = 
      daw::json::from_json_array<Reservoir>(json_data);

  REQUIRE(reservoirs.size() == 2);
  REQUIRE(reservoirs[0].uid == 123);
  REQUIRE(reservoirs[0].name == "ReservoirA");
  REQUIRE(std::get<Uid>(reservoirs[0].junction) == 456);

  REQUIRE(reservoirs[1].uid == 124);
  REQUIRE(reservoirs[1].name == "ReservoirB");
  REQUIRE(std::get<Uid>(reservoirs[1].junction) == 457);
  REQUIRE(reservoirs[1].capacity.value() == 1.0);
  REQUIRE(reservoirs[1].vmax.value() == 1000.0);
}

TEST_CASE("Reservoir roundtrip serialization")
{
  Reservoir original {
    .uid = Uid{123},
    .name = "TestReservoir",
    .active = true,
    .junction = SingleId{Uid{456}},
    .capacity = OptTRealFieldSched{1.0},
    .annual_loss = OptTRealFieldSched{0.05},
    .vmin = OptTRealFieldSched{100.0},
    .vmax = OptTRealFieldSched{1000.0},
    .vcost = OptTRealFieldSched{5.0},
    .vini = OptReal{500.0},
    .vfin = OptReal{600.0}
  };

  auto json = daw::json::to_json(original);
  Reservoir roundtrip = daw::json::from_json<Reservoir>(json);

  REQUIRE(roundtrip.uid == original.uid);
  REQUIRE(roundtrip.name == original.name);
  REQUIRE(roundtrip.active == original.active);
  REQUIRE(roundtrip.junction == original.junction);
  REQUIRE(roundtrip.capacity.value() == original.capacity.value());
  REQUIRE(roundtrip.annual_loss.value() == original.annual_loss.value());
  REQUIRE(roundtrip.vmin.value() == original.vmin.value());
  REQUIRE(roundtrip.vmax.value() == original.vmax.value());
  REQUIRE(roundtrip.vcost.value() == original.vcost.value());
  REQUIRE(roundtrip.vini.value() == original.vini.value());
  REQUIRE(roundtrip.vfin.value() == original.vfin.value());
}

TEST_CASE("Reservoir with empty optional fields")
{
  std::string_view json_data = R"({
    "uid":123,
    "name":"TestReservoir",
    "junction":456
  })";

  Reservoir res = daw::json::from_json<Reservoir>(json_data);

  REQUIRE(res.uid == 123);
  REQUIRE(res.name == "TestReservoir");
  REQUIRE(res.active.has_value() == false);
  REQUIRE(res.capacity.has_value() == false);
  REQUIRE(res.vmax.has_value() == false);
}
