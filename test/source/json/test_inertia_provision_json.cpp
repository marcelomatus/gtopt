// SPDX-License-Identifier: BSD-3-Clause
//
// JSON serialization tests for InertiaProvision, mirroring the
// ReserveProvision suite next door.  Pinned post-2026-05-16 schema
// refactor that replaced the colon-delimited `inertia_zones` string
// with a typed `Array<SingleId>`.

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_inertia_provision.hpp>

using namespace gtopt;

TEST_CASE("InertiaProvision json — basic fields with Uid zone refs")
{
  std::string_view json_data = R"({
    "uid":1,
    "name":"IPROV_A",
    "generator":10,
    "inertia_zones":[1, 2],
    "inertia_constant":5.0,
    "rated_power":120.0,
    "provision_max":100.0
  })";

  const InertiaProvision ip = daw::json::from_json<InertiaProvision>(json_data);

  CHECK(ip.uid == 1);
  CHECK(ip.name == "IPROV_A");
  CHECK_FALSE(ip.active.has_value());
  CHECK(std::get<Uid>(ip.generator) == 10);
  REQUIRE(ip.inertia_zones.size() == 2);
  CHECK(std::get<Uid>(ip.inertia_zones[0]) == Uid {1});
  CHECK(std::get<Uid>(ip.inertia_zones[1]) == Uid {2});
  REQUIRE(ip.inertia_constant.has_value());
  CHECK(*ip.inertia_constant == doctest::Approx(5.0));
  REQUIRE(ip.rated_power.has_value());
  CHECK(*ip.rated_power == doctest::Approx(120.0));
  REQUIRE(ip.provision_max.has_value());
  CHECK(std::get<double>(ip.provision_max.value_or(0.0))
        == doctest::Approx(100.0));
}

TEST_CASE("InertiaProvision json — Name zone refs + cost")
{
  std::string_view json_data = R"({
    "uid":2,
    "name":"IPROV_B",
    "active":1,
    "generator":"GEN_GAS",
    "inertia_zones":["ZONE_A"],
    "provision_factor":3.5,
    "provision_max":80.0,
    "cost":500.0
  })";

  const InertiaProvision ip = daw::json::from_json<InertiaProvision>(json_data);

  CHECK(ip.uid == 2);
  CHECK(std::get<Name>(ip.generator) == "GEN_GAS");
  REQUIRE(ip.inertia_zones.size() == 1);
  CHECK(std::get<Name>(ip.inertia_zones[0]) == "ZONE_A");
  REQUIRE(ip.provision_factor.has_value());
  CHECK(std::get<double>(ip.provision_factor.value_or(0.0))
        == doctest::Approx(3.5));
  REQUIRE(ip.cost.has_value());
  CHECK(std::get<double>(ip.cost.value_or(0.0)) == doctest::Approx(500.0));
}

TEST_CASE("InertiaProvision json — mixed Uid + Name in inertia_zones")
{
  // Per-element Uid (number) vs Name (string) is determined by the
  // JSON token type, not by string-heuristic.  This pins the
  // unambiguous typing the array form gives us.
  std::string_view json_data = R"({
    "uid":3,
    "name":"IPROV_MIXED",
    "generator":5,
    "inertia_zones":[42, "MY_ZONE", 7],
    "provision_max":50.0
  })";

  const InertiaProvision ip = daw::json::from_json<InertiaProvision>(json_data);

  REQUIRE(ip.inertia_zones.size() == 3);
  CHECK(std::get<Uid>(ip.inertia_zones[0]) == Uid {42});
  CHECK(std::get<Name>(ip.inertia_zones[1]) == "MY_ZONE");
  CHECK(std::get<Uid>(ip.inertia_zones[2]) == Uid {7});
}

TEST_CASE("InertiaProvision json — empty inertia_zones array")
{
  // Empty array is legal — a provision linked to zero zones adds no
  // LP coefficient anywhere.  The structural rows / variables are
  // gated on a non-empty zone list in `add_to_lp`.
  std::string_view json_data = R"({
    "uid":4,
    "name":"IPROV_EMPTY",
    "generator":1,
    "inertia_zones":[]
  })";

  const InertiaProvision ip = daw::json::from_json<InertiaProvision>(json_data);
  CHECK(ip.inertia_zones.empty());
}

TEST_CASE(
    "InertiaProvision json — missing inertia_zones field defaults to empty")
{
  // `json_array_null<...>` accepts a missing field as the default
  // `Array<SingleId>{}` — backward-compat with partial JSON inputs.
  std::string_view json_data = R"({
    "uid":5,
    "name":"IPROV_NO_ZONES",
    "generator":2
  })";

  const InertiaProvision ip = daw::json::from_json<InertiaProvision>(json_data);
  CHECK(ip.inertia_zones.empty());
}

TEST_CASE("InertiaProvision json — legacy delimited string is rejected")
{
  // The pre-2026-05-16 schema accepted `"inertia_zones": "1:2"`
  // (colon-delimited string).  Post-refactor the parser only accepts
  // arrays — feeding a string MUST raise a parse error to prevent
  // silent mis-parsing of legacy inputs.
  std::string_view json_data = R"({
    "uid":6,
    "name":"IPROV_LEGACY",
    "generator":3,
    "inertia_zones":"1:2"
  })";
  CHECK_THROWS((void)daw::json::from_json<InertiaProvision>(json_data));
}

TEST_CASE("InertiaProvision array json roundtrip")
{
  InertiaProvision ip;
  ip.uid = 100;
  ip.name = "RT_IPROV";
  ip.generator = Uid {7};
  ip.inertia_zones = {SingleId {Uid {1}}, SingleId {Name {"ZONE_X"}}};
  ip.inertia_constant = 4.0;
  ip.rated_power = 150.0;
  ip.provision_max = 60.0;
  ip.cost = 250.0;

  const auto json = daw::json::to_json(ip);
  const auto roundtrip = daw::json::from_json<InertiaProvision>(json);

  CHECK(roundtrip.uid == 100);
  REQUIRE(roundtrip.inertia_zones.size() == 2);
  CHECK(std::get<Uid>(roundtrip.inertia_zones[0]) == Uid {1});
  CHECK(std::get<Name>(roundtrip.inertia_zones[1]) == "ZONE_X");
  REQUIRE(roundtrip.inertia_constant.has_value());
  CHECK(*roundtrip.inertia_constant == doctest::Approx(4.0));
  REQUIRE(roundtrip.cost.has_value());
  CHECK(std::get<double>(roundtrip.cost.value_or(0.0))
        == doctest::Approx(250.0));
}
