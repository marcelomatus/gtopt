#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_turbine.hpp>

TEST_CASE("Turbine daw json test 1")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"TURBINE_A",
    "capacity":100.0,
    "head":50.0
  })";

  gtopt::Turbine turbine = daw::json::from_json<gtopt::Turbine>(json_data);

  REQUIRE(turbine.uid == 5);
  REQUIRE(turbine.name == "TURBINE_A");
  REQUIRE(turbine.capacity.has_value());
  REQUIRE(std::get<double>(turbine.capacity.value()) == 100.0);
  REQUIRE(turbine.head.has_value());
  REQUIRE(std::get<double>(turbine.head.value()) == 50.0);
}

TEST_CASE("Turbine daw json test 2")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"TURBINE_A"
  })";

  gtopt::Turbine turbine = daw::json::from_json<gtopt::Turbine>(json_data);

  REQUIRE(turbine.uid == 5);
  REQUIRE(turbine.name == "TURBINE_A");
  REQUIRE(!turbine.capacity.has_value());
  REQUIRE(!turbine.head.has_value());
}

TEST_CASE("Turbine array json test")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"TURBINE_A"
  },{
    "uid":15,
    "name":"TURBINE_B",
    "capacity":200.0,
    "head":75.0
  }])";

  std::vector<gtopt::Turbine> turbines =
      daw::json::from_json_array<gtopt::Turbine>(json_data);

  REQUIRE(turbines[0].uid == 5);
  REQUIRE(turbines[0].name == "TURBINE_A");
  REQUIRE(!turbines[0].capacity.has_value());
  REQUIRE(!turbines[0].head.has_value());

  REQUIRE(turbines[1].uid == 15);
  REQUIRE(turbines[1].name == "TURBINE_B");
  REQUIRE(turbines[1].capacity.has_value());
  REQUIRE(std::get<double>(turbines[1].capacity.value()) == 200.0);
  REQUIRE(turbines[1].head.has_value());
  REQUIRE(std::get<double>(turbines[1].head.value()) == 75.0);
}

TEST_CASE("Turbine with active property serialization")
{
  using namespace gtopt;

  SUBCASE("With boolean active")
  {
    Turbine turbine;
    turbine.uid = 1;
    turbine.name = "test_turbine";
    turbine.active = True;

    auto json = daw::json::to_json(turbine);
    Turbine roundtrip = daw::json::from_json<Turbine>(json);

    REQUIRE(roundtrip.active.has_value());
    CHECK(std::get<IntBool>(roundtrip.active.value()) == True);
  }

  SUBCASE("With schedule active")
  {
    Turbine turbine;
    turbine.uid = 1;
    turbine.name = "test_turbine";
    turbine.active = std::vector<IntBool>{True, False, True, False};

    auto json = daw::json::to_json(turbine);
    Turbine roundtrip = daw::json::from_json<Turbine>(json);

    REQUIRE(roundtrip.active.has_value());
    const auto& active = std::get<std::vector<IntBool>>(roundtrip.active.value());
    REQUIRE(active.size() == 4);
    CHECK(active[0] == True);
    CHECK(active[1] == False);
    CHECK(active[2] == True);
    CHECK(active[3] == False);
  }
}

TEST_CASE("Turbine with empty optional fields")
{
  using namespace gtopt;

  std::string_view json_data = R"({
    "uid":5,
    "name":"TURBINE_A",
    "active":null,
    "capacity":null,
    "head":null
  })";

  Turbine turbine = daw::json::from_json<Turbine>(json_data);

  CHECK(turbine.uid == 5);
  CHECK(turbine.name == "TURBINE_A");
  CHECK(!turbine.active.has_value());
  CHECK(!turbine.capacity.has_value());
  CHECK(!turbine.head.has_value());
}
