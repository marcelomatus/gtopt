#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_demand_profile.hpp>

using namespace gtopt;

TEST_CASE("DemandProfile daw json test - basic fields")
{
  std::string_view json_data = R"({
    "uid":1,
    "name":"PROFILE_A",
    "demand":10,
    "profile":0.85
  })";

  DemandProfile dp = daw::json::from_json<DemandProfile>(json_data);

  CHECK(dp.uid == 1);
  CHECK(dp.name == "PROFILE_A");
  CHECK_FALSE(dp.active.has_value());
  CHECK(std::get<Uid>(dp.demand) == 10);
  CHECK(std::get<double>(dp.profile) == doctest::Approx(0.85));
}

TEST_CASE("DemandProfile daw json test - with cost")
{
  std::string_view json_data = R"({
    "uid":2,
    "name":"PROFILE_B",
    "active":1,
    "demand":"DEMAND_REF",
    "profile":1.0,
    "scost":500.0
  })";

  DemandProfile dp = daw::json::from_json<DemandProfile>(json_data);

  CHECK(dp.uid == 2);
  CHECK(dp.name == "PROFILE_B");
  REQUIRE(dp.active.has_value());
  CHECK(std::get<IntBool>(dp.active.value()) == True);
  CHECK(std::get<Name>(dp.demand) == "DEMAND_REF");
  REQUIRE(dp.scost.has_value());
  CHECK(std::get<double>(dp.scost.value()) == doctest::Approx(500.0));
}

TEST_CASE("DemandProfile array json test")
{
  std::string_view json_data = R"([{
    "uid":1,
    "name":"DP_A",
    "demand":10,
    "profile":0.5
  },{
    "uid":2,
    "name":"DP_B",
    "demand":20,
    "profile":0.9
  }])";

  std::vector<DemandProfile> profiles =
      daw::json::from_json_array<DemandProfile>(json_data);

  REQUIRE(profiles.size() == 2);
  CHECK(profiles[0].uid == 1);
  CHECK(profiles[0].name == "DP_A");
  CHECK(profiles[1].uid == 2);
  CHECK(profiles[1].name == "DP_B");
}

TEST_CASE("DemandProfile round-trip serialization")
{
  DemandProfile dp;
  dp.uid = 5;
  dp.name = "RT_PROFILE";
  dp.demand = Uid {42};
  dp.profile = 0.75;
  dp.scost = 100.0;

  auto json = daw::json::to_json(dp);
  DemandProfile roundtrip = daw::json::from_json<DemandProfile>(json);

  CHECK(roundtrip.uid == dp.uid);
  CHECK(roundtrip.name == dp.name);
  CHECK(std::get<Uid>(roundtrip.demand) == 42);
  CHECK(std::get<double>(roundtrip.profile) == doctest::Approx(0.75));
  REQUIRE(roundtrip.scost.has_value());
  CHECK(std::get<double>(roundtrip.scost.value()) == doctest::Approx(100.0));
}
