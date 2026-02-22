#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_junction.hpp>

TEST_CASE("Junction JSON basic parsing")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"CRUCERO"
  })";

  const gtopt::Junction junction =
      daw::json::from_json<gtopt::Junction>(json_data);

  REQUIRE(junction.uid == 5);
  REQUIRE(junction.name == "CRUCERO");
  REQUIRE_FALSE(junction.drain.has_value());
}

TEST_CASE("Junction JSON with drain")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"CRUCERO",
    "drain":true
  })";

  const gtopt::Junction junction =
      daw::json::from_json<gtopt::Junction>(json_data);

  REQUIRE(junction.uid == 5);
  REQUIRE(junction.name == "CRUCERO");
  REQUIRE(junction.drain.has_value());
  REQUIRE(junction.drain.value_or(false) == true);
}

TEST_CASE("Junction JSON array parsing")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"CRUCERO"
  },{
    "uid":10,
    "name":"PTOMONTT",
    "drain":false
  }])";

  std::vector<gtopt::Junction> junctions =
      daw::json::from_json_array<gtopt::Junction>(json_data);

  REQUIRE(junctions.size() == 2);
  REQUIRE(junctions[0].uid == 5);
  REQUIRE(junctions[0].name == "CRUCERO");
  REQUIRE_FALSE(junctions[0].drain.has_value());

  REQUIRE(junctions[1].uid == 10);
  REQUIRE(junctions[1].name == "PTOMONTT");
  REQUIRE(junctions[1].drain.has_value());
  REQUIRE(junctions[1].drain.value_or(true) == false);
}

TEST_CASE("Junction JSON with active schedule")
{
  std::string_view json_data = R"({
    "uid":1,
    "name":"SCHED_JUNC",
    "active":[1,0,1,0]
  })";

  Junction junction = daw::json::from_json<Junction>(json_data);

  REQUIRE(junction.active.has_value());
  const auto& active =
      std::get<std::vector<IntBool>>(junction.active.value());  // NOLINT
  CHECK(active.size() == 4);
  CHECK(active[0] == True);
  CHECK(active[1] == False);
  CHECK(active[2] == True);
  CHECK(active[3] == False);
}

TEST_CASE("Junction JSON roundtrip serialization")
{
  gtopt::Junction original;
  original.uid = 7;
  original.name = "ROUNDTRIP";
  original.drain = true;

  auto json = daw::json::to_json(original);
  gtopt::Junction roundtrip =
      daw::json::from_json<gtopt::Junction>(json);  // NOLINT

  REQUIRE(roundtrip.uid == 7);
  REQUIRE(roundtrip.name == "ROUNDTRIP");
  REQUIRE(roundtrip.drain.has_value());
  REQUIRE(roundtrip.drain.value() == true);  // NOLINT
}
