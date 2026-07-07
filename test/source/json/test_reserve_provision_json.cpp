#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_reserve_provision.hpp>

using namespace gtopt;

TEST_CASE("ReserveProvision daw json test - basic fields")
{
  std::string_view json_data = R"({
    "uid":1,
    "name":"RPROV_A",
    "generator":10,
    "reserve_zones":["ZONE_1","ZONE_2"],
    "urmax":50.0,
    "drmax":30.0
  })";

  const ReserveProvision rp = daw::json::from_json<ReserveProvision>(json_data);

  CHECK(rp.uid == 1);
  CHECK(rp.name == "RPROV_A");
  CHECK_FALSE(rp.active.has_value());
  CHECK(std::get<Uid>(rp.generator) == 10);
  REQUIRE(rp.reserve_zones.size() == 2);
  CHECK(std::get<Name>(rp.reserve_zones[0]) == "ZONE_1");
  CHECK(std::get<Name>(rp.reserve_zones[1]) == "ZONE_2");

  REQUIRE(rp.urmax.has_value());
  CHECK(std::get<double>(rp.urmax.value_or(0.0)) == doctest::Approx(50.0));
  REQUIRE(rp.drmax.has_value());
  CHECK(std::get<double>(rp.drmax.value_or(0.0)) == doctest::Approx(30.0));
}

TEST_CASE("ReserveProvision daw json test - with factors and costs")
{
  std::string_view json_data = R"({
    "uid":2,
    "name":"RPROV_B",
    "active":1,
    "generator":"GEN_COAL",
    "reserve_zones":["ZONE_A"],
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
  CHECK(std::get<IntBool>(rp.active.value_or(Active {False})) == True);
  CHECK(std::get<Name>(rp.generator) == "GEN_COAL");
  REQUIRE(rp.reserve_zones.size() == 1);
  CHECK(std::get<Name>(rp.reserve_zones[0]) == "ZONE_A");

  REQUIRE(rp.ur_capacity_factor.has_value());
  CHECK(std::get<double>(rp.ur_capacity_factor.value_or(0.0))
        == doctest::Approx(0.9));
  REQUIRE(rp.dr_capacity_factor.has_value());
  CHECK(std::get<double>(rp.dr_capacity_factor.value_or(0.0))
        == doctest::Approx(0.8));
  REQUIRE(rp.ur_provision_factor.has_value());
  CHECK(std::get<double>(rp.ur_provision_factor.value_or(0.0))
        == doctest::Approx(0.95));
  REQUIRE(rp.dr_provision_factor.has_value());
  CHECK(std::get<double>(rp.dr_provision_factor.value_or(0.0))
        == doctest::Approx(0.85));

  REQUIRE(rp.urcost.has_value());
  CHECK(std::get<double>(rp.urcost.value_or(0.0)) == doctest::Approx(1000.0));
  REQUIRE(rp.drcost.has_value());
  CHECK(std::get<double>(rp.drcost.value_or(0.0)) == doctest::Approx(800.0));
}

TEST_CASE("ReserveProvision daw json test - minimal fields")
{
  std::string_view json_data = R"({
    "uid":3,
    "name":"RPROV_MINIMAL",
    "generator":5,
    "reserve_zones":["Z1"]
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
    "reserve_zones":["Z1"]
  },{
    "uid":2,
    "name":"RPROV_B",
    "generator":20,
    "reserve_zones":["Z1","Z2"]
  }])";

  std::vector<ReserveProvision> provisions =
      daw::json::from_json_array<ReserveProvision>(json_data);

  REQUIRE(provisions.size() == 2);
  CHECK(provisions[0].uid == 1);
  CHECK(provisions[0].name == "RPROV_A");
  CHECK(provisions[1].uid == 2);
  CHECK(provisions[1].name == "RPROV_B");
  REQUIRE(provisions[1].reserve_zones.size() == 2);
  CHECK(std::get<Name>(provisions[1].reserve_zones[0]) == "Z1");
  CHECK(std::get<Name>(provisions[1].reserve_zones[1]) == "Z2");
}

TEST_CASE("ReserveProvision round-trip serialization")
{
  ReserveProvision rp;
  rp.uid = 10;
  rp.name = "RT_RPROV";
  rp.active = True;
  rp.generator = Uid {42};
  rp.reserve_zones = {SingleId {Name {"ZONE_X"}}, SingleId {Name {"ZONE_Y"}}};
  rp.urmax = 200.0;
  rp.drmax = 150.0;
  rp.urcost = 5000.0;
  rp.drcost = 3000.0;

  auto json = daw::json::to_json(rp);
  ReserveProvision roundtrip = daw::json::from_json<ReserveProvision>(json);

  CHECK(roundtrip.uid == rp.uid);
  CHECK(roundtrip.name == rp.name);
  CHECK(std::get<Uid>(roundtrip.generator) == 42);
  REQUIRE(roundtrip.reserve_zones.size() == 2);
  CHECK(std::get<Name>(roundtrip.reserve_zones[0]) == "ZONE_X");
  CHECK(std::get<Name>(roundtrip.reserve_zones[1]) == "ZONE_Y");
  REQUIRE(roundtrip.urmax.has_value());
  CHECK(std::get<double>(roundtrip.urmax.value_or(0.0))
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

// ─────────────────────────────────────────────────────────────────────────
//   reserve_zones typed-array edge cases (post-2026-05-16 schema refactor)
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("ReserveProvision json — empty reserve_zones array")
{
  // Empty array is legal: a provision with no zones contributes no
  // LP rows.  This pins the JSON parser's acceptance of `[]`.
  std::string_view json_data = R"({
    "uid":4,
    "name":"RPROV_EMPTY",
    "generator":7,
    "reserve_zones":[]
  })";

  const ReserveProvision rp = daw::json::from_json<ReserveProvision>(json_data);
  CHECK(rp.uid == 4);
  CHECK(rp.reserve_zones.empty());
}

TEST_CASE(
    "ReserveProvision json — missing reserve_zones field defaults to empty")
{
  // `json_array_null<...>` allows the field to be absent; the
  // default-constructed `Array<SingleId>` is empty.  This is the
  // backward-compat path for partially-populated JSON inputs that
  // never had `reserve_zones`.
  std::string_view json_data = R"({
    "uid":5,
    "name":"RPROV_NO_ZONES",
    "generator":8
  })";

  const ReserveProvision rp = daw::json::from_json<ReserveProvision>(json_data);
  CHECK(rp.uid == 5);
  CHECK(rp.reserve_zones.empty());
}

TEST_CASE("ReserveProvision json — mixed Uid + Name in reserve_zones array")
{
  // The typed-array form supports per-element Uid (number) or Name
  // (string) freely mixed.  This is the key advantage over the
  // legacy delimited string — types are explicit at the array-element
  // level instead of inferred from "is this all digits" heuristics.
  std::string_view json_data = R"({
    "uid":6,
    "name":"RPROV_MIXED",
    "generator":9,
    "reserve_zones":[1, "ZONE_B", 3]
  })";

  const ReserveProvision rp = daw::json::from_json<ReserveProvision>(json_data);
  REQUIRE(rp.reserve_zones.size() == 3);
  CHECK(std::get<Uid>(rp.reserve_zones[0]) == Uid {1});
  CHECK(std::get<Name>(rp.reserve_zones[1]) == "ZONE_B");
  CHECK(std::get<Uid>(rp.reserve_zones[2]) == Uid {3});
}

TEST_CASE("ReserveProvision json — legacy delimited string is rejected")
{
  // The pre-2026-05-16 schema accepted `"reserve_zones": "1:2"`
  // (colon-delimited string).  The post-refactor schema only accepts
  // arrays — feeding a string MUST raise a parse error so silent
  // mis-parses (e.g., the historical comma-vs-colon delimiter bug)
  // cannot recur.
  std::string_view json_data = R"({
    "uid":7,
    "name":"RPROV_LEGACY",
    "generator":10,
    "reserve_zones":"1:2"
  })";
  CHECK_THROWS((void)daw::json::from_json<ReserveProvision>(json_data));
}
