#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_reserve_provision.hpp>

TEST_CASE("ReserveProvision daw json test - basic fields")
{
  std::string_view json_data = R"({
    "uid":1,
    "name":"RPROV_A",
    "generator":10,
    "reserve_zones":"ZONE_1,ZONE_2",
    "urmax":50.0,
    "drmax":30.0
  })";

  const ReserveProvision rp =
      daw::json::from_json<ReserveProvision>(json_data);

  CHECK(rp.uid == 1);
  CHECK(rp.name == "RPROV_A");
  CHECK_FALSE(rp.active.has_value());
  CHECK(std::get<Uid>(rp.generator) == 10);
  CHECK(rp.reserve_zones == "ZONE_1,ZONE_2");

  REQUIRE(rp.urmax.has_value());
  CHECK(std::get<double>(rp.urmax.value())  // NOLINT(bugprone-unchecked-optional-access)
        == doctest::Approx(50.0));
  REQUIRE(rp.drmax.has_value());
  CHECK(std::get<double>(rp.drmax.value())  // NOLINT(bugprone-unchecked-optional-access)
        == doctest::Approx(30.0));
}

TEST_CASE("ReserveProvision daw json test - with factors and costs")
{
  std::string_view json_data = R"({
    "uid":2,
    "name":"RPROV_B",
    "active":1,
    "generator":"GEN_COAL",
    "reserve_zones":"ZONE_A",
    "urmax":100.0,
    "drmax":80.0,
    "ur_capacity_factor":0.9,
    "dr_capacity_factor":0.8,
    "ur_provision_factor":0.95,
    "dr_provision_factor":0.85,
    "urcost":1000.0,
    "drcost":800.0
  })";

  ReserveProvision rp = daw::json::from_json<ReserveProvision>(json_data);

  CHECK(rp.uid == 2);
  CHECK(rp.name == "RPROV_B");
  REQUIRE(rp.active.has_value());
  CHECK(std::get<IntBool>(rp.active.value()) == True);  // NOLINT(bugprone-unchecked-optional-access)
  CHECK(std::get<Name>(rp.generator) == "GEN_COAL");
  CHECK(rp.reserve_zones == "ZONE_A");

  REQUIRE(rp.ur_capacity_factor.has_value());
  CHECK(std::get<double>(rp.ur_capacity_factor.value())  // NOLINT(bugprone-unchecked-optional-access)
        == doctest::Approx(0.9));
  REQUIRE(rp.dr_capacity_factor.has_value());
  CHECK(std::get<double>(rp.dr_capacity_factor.value())  // NOLINT(bugprone-unchecked-optional-access)
        == doctest::Approx(0.8));
  REQUIRE(rp.ur_provision_factor.has_value());
  CHECK(std::get<double>(rp.ur_provision_factor.value())  // NOLINT(bugprone-unchecked-optional-access)
        == doctest::Approx(0.95));
  REQUIRE(rp.dr_provision_factor.has_value());
  CHECK(std::get<double>(rp.dr_provision_factor.value())  // NOLINT(bugprone-unchecked-optional-access)
        == doctest::Approx(0.85));

  REQUIRE(rp.urcost.has_value());
  CHECK(std::get<double>(rp.urcost.value())  // NOLINT(bugprone-unchecked-optional-access)
        == doctest::Approx(1000.0));
  REQUIRE(rp.drcost.has_value());
  CHECK(std::get<double>(rp.drcost.value())  // NOLINT(bugprone-unchecked-optional-access)
        == doctest::Approx(800.0));
}

TEST_CASE("ReserveProvision daw json test - minimal fields")
{
  std::string_view json_data = R"({
    "uid":3,
    "name":"RPROV_MINIMAL",
    "generator":5,
    "reserve_zones":"Z1"
  })";

  const ReserveProvision rp = daw::json::from_json<ReserveProvision>(json_data);

  CHECK(rp.uid == 3);
  CHECK(rp.name == "RPROV_MINIMAL");
  CHECK_FALSE(rp.active.has_value());
  CHECK_FALSE(rp.urmax.has_value());
  CHECK_FALSE(rp.drmax.has_value());
  CHECK_FALSE(rp.ur_capacity_factor.has_value());
  CHECK_FALSE(rp.dr_capacity_factor.has_value());
  CHECK_FALSE(rp.ur_provision_factor.has_value());
  CHECK_FALSE(rp.dr_provision_factor.has_value());
  CHECK_FALSE(rp.urcost.has_value());
  CHECK_FALSE(rp.drcost.has_value());
}

TEST_CASE("ReserveProvision array json test")
{
  std::string_view json_data = R"([{
    "uid":1,
    "name":"RPROV_A",
    "generator":10,
    "reserve_zones":"Z1"
  },{
    "uid":2,
    "name":"RPROV_B",
    "generator":20,
    "reserve_zones":"Z1,Z2"
  }])";

  std::vector<ReserveProvision> provisions =
      daw::json::from_json_array<ReserveProvision>(json_data);

  REQUIRE(provisions.size() == 2);
  CHECK(provisions[0].uid == 1);
  CHECK(provisions[0].name == "RPROV_A");
  CHECK(provisions[1].uid == 2);
  CHECK(provisions[1].name == "RPROV_B");
  CHECK(provisions[1].reserve_zones == "Z1,Z2");
}

TEST_CASE("ReserveProvision round-trip serialization")
{
  ReserveProvision rp;
  rp.uid = 10;
  rp.name = "RT_RPROV";
  rp.active = True;
  rp.generator = Uid {42};
  rp.reserve_zones = "ZONE_X,ZONE_Y";
  rp.urmax = 200.0;
  rp.drmax = 150.0;
  rp.urcost = 5000.0;
  rp.drcost = 3000.0;

  auto json = daw::json::to_json(rp);
  ReserveProvision roundtrip = daw::json::from_json<ReserveProvision>(json);

  CHECK(roundtrip.uid == rp.uid);
  CHECK(roundtrip.name == rp.name);
  CHECK(std::get<Uid>(roundtrip.generator) == 42);
  CHECK(roundtrip.reserve_zones == "ZONE_X,ZONE_Y");
  REQUIRE(roundtrip.urmax.has_value());
  CHECK(std::get<double>(roundtrip.urmax.value())  // NOLINT(bugprone-unchecked-optional-access)
        == doctest::Approx(200.0));
  REQUIRE(roundtrip.drmax.has_value());
  CHECK(std::get<double>(roundtrip.drmax.value_or(0.0))
        == doctest::Approx(150.0));
  REQUIRE(roundtrip.urcost.has_value());
  CHECK(std::get<double>(roundtrip.urcost.value_or(0.0))
        == doctest::Approx(5000.0));
  REQUIRE(roundtrip.drcost.has_value());
  CHECK(std::get<double>(roundtrip.drcost.value_or(0.0))
        == doctest::Approx(3000.0));
}
