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
    "ecost":null,
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
  CHECK_FALSE(res.ecost.has_value());
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
    "ecost":5.0,
    "eini":500.0,
    "efin":600.0
  })";

  const Reservoir res = daw::json::from_json<Reservoir>(json_data);
  CHECK(res.capacity.has_value());
  CHECK(std::get<double>(res.capacity.value_or(-1.0)) == 1.0);
  CHECK(std::get<double>(res.annual_loss.value_or(-1.0)) == 0.05);
  CHECK(std::get<double>(res.emin.value_or(-1.0)) == 100.0);
  CHECK(std::get<double>(res.emax.value_or(-1.0)) == 1000.0);
  CHECK(std::get<double>(res.ecost.value_or(-1.0)) == 5.0);
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
      .ecost = OptTRealFieldSched {5.0},
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
  CHECK(roundtrip.ecost.has_value());
  CHECK(roundtrip.ecost.value_or(-1.0) == original.ecost.value_or(-2.0));
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
  CHECK_FALSE(res.ecost.has_value());
  CHECK_FALSE(res.eini.has_value());
  CHECK_FALSE(res.efin.has_value());
}

TEST_CASE("Reservoir use_state_variable JSON round-trip")  // NOLINT
{
  SUBCASE("null/absent -> nullopt (coupled by default)")
  {
    std::string_view json_data = R"({"uid":1,"name":"r1","junction":1})";
    const Reservoir res = daw::json::from_json<Reservoir>(json_data);
    CHECK_FALSE(res.use_state_variable.has_value());
    // Reservoir LP defaults to coupled when not set
    CHECK(res.use_state_variable.value_or(true) == true);
  }

  SUBCASE("explicit false -> decoupled")
  {
    std::string_view json_data =
        R"({"uid":1,"name":"r1","junction":1,"use_state_variable":false})";
    const Reservoir res = daw::json::from_json<Reservoir>(json_data);
    REQUIRE(res.use_state_variable.has_value());
    CHECK(res.use_state_variable.value_or(true) == false);
  }

  SUBCASE("explicit true -> coupled")
  {
    std::string_view json_data =
        R"({"uid":1,"name":"r1","junction":1,"use_state_variable":true})";
    const Reservoir res = daw::json::from_json<Reservoir>(json_data);
    REQUIRE(res.use_state_variable.has_value());
    CHECK(res.use_state_variable.value_or(false) == true);
  }
}

TEST_CASE("Reservoir soft_emin JSON round-trip")  // NOLINT
{
  SUBCASE("absent -> nullopt")
  {
    std::string_view json_data = R"({"uid":1,"name":"r1","junction":1})";
    const Reservoir res = daw::json::from_json<Reservoir>(json_data);
    CHECK_FALSE(res.soft_emin.has_value());
    CHECK_FALSE(res.soft_emin_cost.has_value());
  }

  SUBCASE("scalar soft_emin and soft_emin_cost")
  {
    std::string_view json_data = R"({
      "uid":1,"name":"r1","junction":1,
      "soft_emin":500.0,"soft_emin_cost":0.1
    })";
    const Reservoir res = daw::json::from_json<Reservoir>(json_data);
    REQUIRE(res.soft_emin.has_value());
    CHECK(std::get<double>(res.soft_emin.value_or(-1.0))
          == doctest::Approx(500.0));
    REQUIRE(res.soft_emin_cost.has_value());
    CHECK(std::get<double>(res.soft_emin_cost.value_or(-1.0))
          == doctest::Approx(0.1));
  }

  SUBCASE("array soft_emin (per-stage)")
  {
    std::string_view json_data = R"({
      "uid":1,"name":"r1","junction":1,
      "soft_emin":[100,200,300],"soft_emin_cost":0.05
    })";
    const Reservoir res = daw::json::from_json<Reservoir>(json_data);
    REQUIRE(res.soft_emin.has_value());
    const auto& vec = std::get<std::vector<double>>(*res.soft_emin);
    CHECK(vec.size() == 3);
    CHECK(vec[0] == doctest::Approx(100.0));
    CHECK(vec[2] == doctest::Approx(300.0));
  }

  SUBCASE("roundtrip")
  {
    Reservoir original {
        .uid = Uid {10},
        .name = "rsv_semin",
        .junction = SingleId {Uid {1}},
        .soft_emin = OptTRealFieldSched {250.0},
        .soft_emin_cost = OptTRealFieldSched {0.1},
    };
    auto json = daw::json::to_json(original);
    Reservoir rt = daw::json::from_json<Reservoir>(json);
    REQUIRE(rt.soft_emin.has_value());
    CHECK(std::get<double>(rt.soft_emin.value_or(-1.0))
          == doctest::Approx(250.0));
    REQUIRE(rt.soft_emin_cost.has_value());
    CHECK(std::get<double>(rt.soft_emin_cost.value_or(-1.0))
          == doctest::Approx(0.1));
  }
}

TEST_CASE("Reservoir daily_cycle JSON round-trip")  // NOLINT
{
  SUBCASE("absent -> nullopt (LP default false)")
  {
    std::string_view json_data = R"({"uid":1,"name":"r1","junction":1})";
    const Reservoir res = daw::json::from_json<Reservoir>(json_data);
    CHECK_FALSE(res.daily_cycle.has_value());
    // Reservoir LP defaults to daily_cycle=false when not set
    CHECK(res.daily_cycle.value_or(false) == false);
  }

  SUBCASE("explicit true -> enabled")
  {
    std::string_view json_data =
        R"({"uid":1,"name":"r1","junction":1,"daily_cycle":true})";
    const Reservoir res = daw::json::from_json<Reservoir>(json_data);
    REQUIRE(res.daily_cycle.has_value());
    CHECK(res.daily_cycle.value_or(false) == true);
  }

  SUBCASE("explicit false -> disabled")
  {
    std::string_view json_data =
        R"({"uid":1,"name":"r1","junction":1,"daily_cycle":false})";
    const Reservoir res = daw::json::from_json<Reservoir>(json_data);
    REQUIRE(res.daily_cycle.has_value());
    CHECK(res.daily_cycle.value_or(true) == false);
  }

  SUBCASE("null -> nullopt")
  {
    std::string_view json_data =
        R"({"uid":1,"name":"r1","junction":1,"daily_cycle":null})";
    const Reservoir res = daw::json::from_json<Reservoir>(json_data);
    CHECK_FALSE(res.daily_cycle.has_value());
  }
}
