#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_reserve_zone.hpp>
#include <gtopt/reserve_zone.hpp>

TEST_CASE("ReserveZone daw json test - basic fields")
{
  using namespace gtopt;

  std::string_view json_data = R"({
    "uid":1,
    "name":"ZONE_A",
    "urreq":100.0,
    "drreq":50.0
  })";

  ReserveZone rz = daw::json::from_json<ReserveZone>(json_data);

  CHECK(rz.uid == 1);
  CHECK(rz.name == "ZONE_A");
  CHECK_FALSE(rz.active.has_value());

  REQUIRE(rz.urreq.has_value());
  CHECK(std::get<double>(
            rz.urreq.value())  // NOLINT(bugprone-unchecked-optional-access)
        == doctest::Approx(100.0));

  REQUIRE(rz.drreq.has_value());
  CHECK(std::get<double>(
            rz.drreq.value())  // NOLINT(bugprone-unchecked-optional-access)
        == doctest::Approx(50.0));
}

TEST_CASE("ReserveZone daw json test - with costs")
{
  using namespace gtopt;

  std::string_view json_data = R"({
    "uid":2,
    "name":"ZONE_B",
    "active":1,
    "urreq":200.0,
    "drreq":100.0,
    "urcost":5000.0,
    "drcost":3000.0
  })";

  ReserveZone rz = daw::json::from_json<ReserveZone>(json_data);

  CHECK(rz.uid == 2);
  CHECK(rz.name == "ZONE_B");
  REQUIRE(rz.active.has_value());
  CHECK(std::get<IntBool>(
            rz.active.value())  // NOLINT(bugprone-unchecked-optional-access)
        == True);

  REQUIRE(rz.urcost.has_value());
  CHECK(std::get<double>(
            rz.urcost.value())  // NOLINT(bugprone-unchecked-optional-access)
        == doctest::Approx(5000.0));

  REQUIRE(rz.drcost.has_value());
  CHECK(std::get<double>(
            rz.drcost.value())  // NOLINT(bugprone-unchecked-optional-access)
        == doctest::Approx(3000.0));
}

TEST_CASE("ReserveZone daw json test - minimal fields")
{
  using namespace gtopt;

  std::string_view json_data = R"({
    "uid":3,
    "name":"ZONE_MINIMAL"
  })";

  const ReserveZone rz = daw::json::from_json<ReserveZone>(json_data);

  CHECK(rz.uid == 3);
  CHECK(rz.name == "ZONE_MINIMAL");
  CHECK_FALSE(rz.active.has_value());
  CHECK_FALSE(rz.urreq.has_value());
  CHECK_FALSE(rz.drreq.has_value());
  CHECK_FALSE(rz.urcost.has_value());
  CHECK_FALSE(rz.drcost.has_value());
}

TEST_CASE("ReserveZone array json test")
{
  using namespace gtopt;

  std::string_view json_data = R"([{
    "uid":1,
    "name":"ZONE_A",
    "urreq":100.0
  },{
    "uid":2,
    "name":"ZONE_B",
    "drreq":50.0
  }])";

  std::vector<ReserveZone> zones =
      daw::json::from_json_array<ReserveZone>(json_data);

  REQUIRE(zones.size() == 2);
  CHECK(zones[0].uid == 1);
  CHECK(zones[0].name == "ZONE_A");
  CHECK(zones[1].uid == 2);
  CHECK(zones[1].name == "ZONE_B");
}

TEST_CASE("ReserveZone round-trip serialization")
{
  using namespace gtopt;

  ReserveZone rz;
  rz.uid = 10;
  rz.name = "RT_ZONE";
  rz.active = True;
  rz.urreq = 150.0;
  rz.drreq = 75.0;
  rz.urcost = 4000.0;
  rz.drcost = 2000.0;

  auto json = daw::json::to_json(rz);
  // NOLINTNEXTLINE
  const ReserveZone roundtrip = daw::json::from_json<ReserveZone>(json);

  CHECK(roundtrip.uid == rz.uid);
  CHECK(roundtrip.name == rz.name);
  REQUIRE(roundtrip.urreq.has_value());
  CHECK(std::get<double>(roundtrip.urreq.value_or(0.0))
        == doctest::Approx(150.0));
  REQUIRE(roundtrip.drreq.has_value());
  CHECK(std::get<double>(roundtrip.drreq.value_or(0.0))
        == doctest::Approx(75.0));
}
