#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_junction.hpp>

using namespace gtopt;

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
  using namespace gtopt;

  std::string_view json_data = R"({
    "uid":1,
    "name":"SCHED_JUNC",
    "active":[1,0,1,0]
  })";

  const Junction junction = daw::json::from_json<Junction>(json_data);

  REQUIRE(junction.active.has_value());
  const Active active_val =
      junction.active.value_or(Active {std::vector<IntBool> {}});
  const auto& active = std::get<std::vector<IntBool>>(active_val);
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
  const gtopt::Junction roundtrip = daw::json::from_json<gtopt::Junction>(json);

  REQUIRE(roundtrip.uid == 7);
  REQUIRE(roundtrip.name == "ROUNDTRIP");
  REQUIRE(roundtrip.drain.has_value());
  REQUIRE(roundtrip.drain == true);
}

TEST_CASE("Junction JSON drain_capacity + drain_cost optional fields")
{
  SUBCASE("both fields present")
  {
    std::string_view json_data = R"({
      "uid":42,
      "name":"LMAULE",
      "drain":true,
      "drain_capacity":250.0,
      "drain_cost":3.6
    })";

    const auto junction = daw::json::from_json<gtopt::Junction>(json_data);

    CHECK(junction.uid == 42);
    CHECK(junction.name == "LMAULE");
    CHECK(junction.drain.value_or(false) == true);
    CHECK(junction.drain_capacity.value_or(-1.0) == doctest::Approx(250.0));
    CHECK(junction.drain_cost.value_or(-1.0) == doctest::Approx(3.6));
  }

  SUBCASE("drain present, capacity/cost omitted → optional empty")
  {
    std::string_view json_data = R"({
      "uid":1,
      "name":"FREE_DRAIN",
      "drain":true
    })";

    const auto junction = daw::json::from_json<gtopt::Junction>(json_data);

    CHECK(junction.drain.value_or(false) == true);
    CHECK_FALSE(junction.drain_capacity.has_value());
    CHECK_FALSE(junction.drain_cost.has_value());
  }

  SUBCASE("roundtrip preserves the two new fields")
  {
    gtopt::Junction original;
    original.uid = 7;
    original.name = "RT";
    original.drain = true;
    original.drain_capacity = 100.0;
    original.drain_cost = 1.25;

    const auto json = daw::json::to_json(original);
    const auto rt = daw::json::from_json<gtopt::Junction>(json);

    CHECK(rt.drain.value_or(false) == true);
    CHECK(rt.drain_capacity.value_or(0.0) == doctest::Approx(100.0));
    CHECK(rt.drain_cost.value_or(0.0) == doctest::Approx(1.25));
  }
}
