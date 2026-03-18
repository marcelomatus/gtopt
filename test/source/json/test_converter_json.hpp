/**
 * @file      test_converter_json.hpp
 * @brief     JSON serialization tests for Converter
 * @date      2026-03-18
 * @copyright BSD-3-Clause
 */

#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_converter.hpp>

TEST_CASE("Converter JSON basic parsing")
{
  const std::string_view json_data = R"({
    "uid": 1,
    "name": "conv1",
    "battery": 10,
    "generator": 20,
    "demand": 30,
    "conversion_rate": 0.95
  })";

  const Converter conv = daw::json::from_json<Converter>(json_data);

  CHECK(conv.uid == 1);
  CHECK(conv.name == "conv1");
  CHECK(std::get<Uid>(conv.battery) == 10);
  CHECK(std::get<Uid>(conv.generator) == 20);
  CHECK(std::get<Uid>(conv.demand) == 30);
  REQUIRE(conv.conversion_rate.has_value());
  CHECK(std::get<Real>(conv.conversion_rate.value_or(RealFieldSched {0.0}))
        == doctest::Approx(0.95));
}

TEST_CASE("Converter JSON minimal fields")
{
  const std::string_view json_data = R"({
    "uid": 2,
    "name": "conv_min",
    "battery": 1,
    "generator": 2,
    "demand": 3
  })";

  const Converter conv = daw::json::from_json<Converter>(json_data);

  CHECK(conv.uid == 2);
  CHECK(conv.name == "conv_min");
  CHECK_FALSE(conv.conversion_rate.has_value());
  CHECK_FALSE(conv.capacity.has_value());
  CHECK_FALSE(conv.expcap.has_value());
  CHECK_FALSE(conv.annual_capcost.has_value());
}

TEST_CASE("Converter JSON with expansion fields")
{
  const std::string_view json_data = R"({
    "uid": 3,
    "name": "conv_exp",
    "battery": 1,
    "generator": 2,
    "demand": 3,
    "capacity": 100.0,
    "expcap": 50.0,
    "expmod": 5,
    "capmax": 350.0,
    "annual_capcost": 1000.0,
    "annual_derating": 0.02
  })";

  const Converter conv = daw::json::from_json<Converter>(json_data);

  CHECK(conv.uid == 3);
  REQUIRE(conv.capacity.has_value());
  CHECK(std::get<Real>(conv.capacity.value_or(RealFieldSched {0.0}))
        == doctest::Approx(100.0));
  REQUIRE(conv.expcap.has_value());
  CHECK(std::get<Real>(conv.expcap.value_or(RealFieldSched {0.0}))
        == doctest::Approx(50.0));
  REQUIRE(conv.annual_capcost.has_value());
  CHECK(std::get<Real>(conv.annual_capcost.value_or(RealFieldSched {0.0}))
        == doctest::Approx(1000.0));
  REQUIRE(conv.annual_derating.has_value());
  CHECK(std::get<Real>(conv.annual_derating.value_or(RealFieldSched {0.0}))
        == doctest::Approx(0.02));
}

TEST_CASE("Converter JSON array parsing")
{
  const std::string_view json_data = R"([
    {"uid": 1, "name": "c1", "battery": 1, "generator": 1, "demand": 1},
    {"uid": 2, "name": "c2", "battery": 2, "generator": 2, "demand": 2,
     "conversion_rate": 0.9}
  ])";

  const auto convs = daw::json::from_json_array<Converter>(json_data);

  REQUIRE(convs.size() == 2);
  CHECK(convs[0].uid == 1);
  CHECK(convs[0].name == "c1");
  CHECK(convs[1].uid == 2);
  REQUIRE(convs[1].conversion_rate.has_value());
}

TEST_CASE("Converter JSON round-trip serialization")
{
  Converter original;
  original.uid = 7;
  original.name = "roundtrip";
  original.battery = Uid {10};
  original.generator = Uid {20};
  original.demand = Uid {30};
  original.conversion_rate = 0.85;
  original.capacity = 200.0;

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const Converter roundtrip = daw::json::from_json<Converter>(json);
  CHECK(roundtrip.uid == 7);
  CHECK(roundtrip.name == "roundtrip");
  CHECK(std::get<Uid>(roundtrip.battery) == 10);
  CHECK(std::get<Uid>(roundtrip.generator) == 20);
  CHECK(std::get<Uid>(roundtrip.demand) == 30);
  REQUIRE(roundtrip.conversion_rate.has_value());
  CHECK(std::get<Real>(roundtrip.conversion_rate.value_or(RealFieldSched {0.0}))
        == doctest::Approx(0.85));
}

TEST_CASE("Converter JSON with active schedule")
{
  const std::string_view json_data = R"({
    "uid": 1,
    "name": "conv_active",
    "battery": 1,
    "generator": 2,
    "demand": 3,
    "active": [1, 0, 1]
  })";

  const Converter conv = daw::json::from_json<Converter>(json_data);

  REQUIRE(conv.active.has_value());
  const Active active_val =
      conv.active.value_or(Active {std::vector<IntBool> {}});
  const auto& active = std::get<std::vector<IntBool>>(active_val);
  CHECK(active.size() == 3);
  CHECK(active[0] == True);
  CHECK(active[1] == False);
  CHECK(active[2] == True);
}

TEST_CASE("Converter JSON with named references")
{
  const std::string_view json_data = R"({
    "uid": 1,
    "name": "conv_named",
    "battery": "bess1",
    "generator": "gen_discharge",
    "demand": "dem_charge"
  })";

  const Converter conv = daw::json::from_json<Converter>(json_data);

  CHECK(conv.uid == 1);
  CHECK(std::get<Name>(conv.battery) == "bess1");
  CHECK(std::get<Name>(conv.generator) == "gen_discharge");
  CHECK(std::get<Name>(conv.demand) == "dem_charge");
}
