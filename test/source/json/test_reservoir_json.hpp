#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_reservoir.hpp>

TEST_CASE("Reservoir basic fields deserialization")
{
  std::string_view json_data = R"({
    "uid":123,
    "name":"TestReservoir",
    "active":1,
    "junction":456,
    "capacity":null,
    "annual_loss":null,
    "emin":null,
    "emax":null,
    "vcost":null,
    "eini":null,
    "efin":null
  })";

  Reservoir res = daw::json::from_json<Reservoir>(json_data);

  CHECK(res.uid == 123);
  CHECK(res.name == "TestReservoir");
  CHECK(res.active.has_value());
  CHECK(std::get<IntBool>(res.active.value_or(False)));
  CHECK(std::get<Uid>(res.junction) == 456);
  CHECK_FALSE(res.capacity.has_value());
  CHECK_FALSE(res.annual_loss.has_value());
  CHECK_FALSE(res.emin.has_value());
  CHECK_FALSE(res.emax.has_value());
  CHECK_FALSE(res.vcost.has_value());
  CHECK_FALSE(res.eini.has_value());
  CHECK_FALSE(res.efin.has_value());
}

TEST_CASE("Reservoir optional fields deserialization")
{
  std::string_view json_data = R"({
    "uid":123,
    "name":"TestReservoir",
    "junction":12,
    "capacity":1.0,
    "annual_loss":0.05,
    "emin":100.0,
    "emax":1000.0,
    "vcost":5.0,
    "eini":500.0,
    "efin":600.0
  })";

  const Reservoir res = daw::json::from_json<Reservoir>(json_data);
  CHECK(res.capacity.has_value());
  CHECK(std::get<double>(res.capacity.value_or(-1.0)) == 1.0);
  CHECK(std::get<double>(res.annual_loss.value_or(-1.0)) == 0.05);
  CHECK(std::get<double>(res.emin.value_or(-1.0)) == 100.0);
  CHECK(std::get<double>(res.emax.value_or(-1.0)) == 1000.0);
  CHECK(std::get<double>(res.vcost.value_or(-1.0)) == 5.0);
  CHECK(res.eini.value_or(-1.0) == 500.0);
  CHECK(res.efin.value_or(-1.0) == 600.0);
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
    "emax":1000.0
  }])";

  std::vector<Reservoir> reservoirs =
      daw::json::from_json_array<Reservoir>(json_data);

  CHECK(reservoirs.size() == 2);
  CHECK(reservoirs[0].uid == 123);
  CHECK(reservoirs[0].name == "ReservoirA");
  CHECK(std::get<Uid>(reservoirs[0].junction) == 456);

  CHECK(reservoirs[1].uid == 124);
  CHECK(reservoirs[1].name == "ReservoirB");
  CHECK(std::get<Uid>(reservoirs[1].junction) == 457);
  CHECK(std::get<double>(reservoirs[1].capacity.value_or(-1.0)) == 1.0);
  CHECK(std::get<double>(reservoirs[1].emax.value_or(-1.0)) == 1000.0);
}

TEST_CASE("Reservoir roundtrip serialization")
{
  Reservoir original {
      .uid = Uid {123},
      .name = "TestReservoir",
      .active = true,
      .junction = SingleId {Uid {456}},
      .capacity = OptTRealFieldSched {1.0},
      .annual_loss = OptTRealFieldSched {0.05},
      .emin = OptTRealFieldSched {100.0},
      .emax = OptTRealFieldSched {1000.0},
      .vcost = OptTRealFieldSched {5.0},
      .eini = OptReal {500.0},
      .efin = OptReal {600.0},
  };

  auto json = daw::json::to_json(original);

  // Verify JSON contains expected fields
  CHECK(json.find("\"uid\":123") != std::string::npos);
  CHECK(json.find("\"name\":\"TestReservoir\"") != std::string::npos);
  CHECK(json.find("\"active\":1") != std::string::npos);
  CHECK(json.find("\"capacity\":1") != std::string::npos);

  Reservoir roundtrip = daw::json::from_json<Reservoir>(json);

  CHECK(roundtrip.uid == original.uid);
  CHECK(roundtrip.name == original.name);
  CHECK(roundtrip.active.has_value());
  CHECK(original.active.has_value());
  CHECK(std::get<Uid>(roundtrip.junction) == std::get<Uid>(original.junction));
  CHECK(roundtrip.capacity.has_value());
  CHECK(roundtrip.capacity.value_or(-1.0) == original.capacity.value_or(-2.0));
  CHECK(roundtrip.annual_loss.has_value());
  CHECK(roundtrip.annual_loss.value_or(-1.0)
        == original.annual_loss.value_or(-2.0));
  CHECK(roundtrip.emin.has_value());
  CHECK(roundtrip.emin.value_or(-1.0) == original.emin.value_or(-2.0));
  CHECK(roundtrip.emax.has_value());
  CHECK(roundtrip.emax.value_or(-1.0) == original.emax.value_or(-2.0));
  CHECK(roundtrip.vcost.has_value());
  CHECK(roundtrip.vcost.value_or(-1.0) == original.vcost.value_or(-2.0));
  CHECK(roundtrip.eini.has_value());
  CHECK(roundtrip.eini.value_or(-1.0) == original.eini.value_or(-2.0));
  CHECK(roundtrip.efin.has_value());
  CHECK(roundtrip.efin.value_or(-1.0) == original.efin.value_or(-2.0));
}

TEST_CASE("Reservoir with empty optional fields")
{
  std::string_view json_data = R"({
    "uid":123,
    "name":"TestReservoir",
    "junction":456
  })";

  const Reservoir res = daw::json::from_json<Reservoir>(json_data);

  CHECK(res.uid == 123);
  CHECK(res.name == "TestReservoir");
  CHECK_FALSE(res.active.has_value());
  CHECK_FALSE(res.capacity.has_value());
  CHECK_FALSE(res.annual_loss.has_value());
  CHECK_FALSE(res.emin.has_value());
  CHECK_FALSE(res.emax.has_value());
  CHECK_FALSE(res.vcost.has_value());
  CHECK_FALSE(res.eini.has_value());
  CHECK_FALSE(res.efin.has_value());
}
