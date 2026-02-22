#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_turbine.hpp>

TEST_CASE("Turbine daw json test 1")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"TURBINE_A",
    "waterway":10,
    "generator":20,
    "capacity":100.0,
    "conversion_rate":50.0,
    "drain":true
  })";

  const gtopt::Turbine turbine = daw::json::from_json<gtopt::Turbine>(json_data);

  CHECK(turbine.uid == 5);
  CHECK(turbine.name == "TURBINE_A");
  CHECK(turbine.capacity.has_value());
  CHECK(std::get<double>(turbine.capacity.value_or(-1.0)) == 100.0);
  CHECK(turbine.conversion_rate.has_value());
  CHECK(std::get<double>(turbine.conversion_rate.value_or(-1.0)) == 50.0);
  CHECK(turbine.drain.has_value());
  CHECK(turbine.drain == true);
}

TEST_CASE("Turbine daw json test 2")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"TURBINE_A",
    "waterway":10,
    "generator":20
  })";

  const gtopt::Turbine turbine =
      daw::json::from_json<gtopt::Turbine>(json_data);

  CHECK(turbine.uid == 5);
  CHECK(turbine.name == "TURBINE_A");
  CHECK(!turbine.capacity.has_value());
  CHECK(!turbine.conversion_rate.has_value());
  CHECK(!turbine.drain.has_value());
}

TEST_CASE("Turbine array json test")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"TURBINE_A",
    "waterway":10,
    "generator":20,
    "drain":false
  },{
    "uid":15,
    "name":"TURBINE_B",
    "capacity":200.0,
    "conversion_rate":75.0,
    "waterway":30,
    "generator":40,
    "drain":true
  }])";

  std::vector<gtopt::Turbine> turbines =
      daw::json::from_json_array<gtopt::Turbine>(json_data);

  CHECK(turbines[0].uid == 5);
  CHECK(turbines[0].name == "TURBINE_A");
  CHECK(!turbines[0].capacity.has_value());
  CHECK(!turbines[0].conversion_rate.has_value());
  CHECK(turbines[0].drain.has_value());
  CHECK(turbines[0].drain == false);

  CHECK(turbines[1].uid == 15);
  CHECK(turbines[1].name == "TURBINE_B");
  CHECK(turbines[1].capacity.has_value());
  CHECK(std::get<double>(turbines[1].capacity.value_or(-1.0)) == 200.0);
  CHECK(turbines[1].conversion_rate.has_value());
  CHECK(std::get<double>(turbines[1].conversion_rate.value_or(-1.0)) == 75.0);
  CHECK(turbines[1].drain.has_value());
  CHECK(turbines[1].drain == true);
}

TEST_CASE("Turbine with active property serialization")
{
  SUBCASE("With boolean active")
  {
    Turbine turbine;
    turbine.uid = 1;
    turbine.name = "test_turbine";
    turbine.active = True;
    turbine.drain = false;

    auto json = daw::json::to_json(turbine);
    const Turbine roundtrip = daw::json::from_json<Turbine>(json);

    CHECK(roundtrip.active.has_value());
    CHECK(std::get<IntBool>(roundtrip.active.value_or(False)) == True);
    CHECK(roundtrip.drain.has_value());
    CHECK(roundtrip.drain == false);
  }

  SUBCASE("With schedule active")
  {
    Turbine turbine;
    turbine.uid = 1;
    turbine.name = "test_turbine";
    turbine.active = std::vector<IntBool> {True, False, True, False};
    turbine.drain = std::nullopt;

    auto json = daw::json::to_json(turbine);
    Turbine roundtrip = daw::json::from_json<Turbine>(json);

    CHECK(roundtrip.active.has_value());
    if (roundtrip.active.has_value()) {
      const auto& active =
          std::get<std::vector<IntBool>>(roundtrip.active.value());
      CHECK(active.size() == 4);
      CHECK(active[0] == True);
      CHECK(active[1] == False);
      CHECK(active[2] == True);
      CHECK(active[3] == False);
    }
    CHECK(!roundtrip.drain.has_value());
  }
}

TEST_CASE("Turbine with empty optional fields")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"TURBINE_A",
    "active":null,
    "capacity":null,
    "waterway":10,
    "generator":20,
    "conversion_rate":null,
    "drain":null
  })";

  const Turbine turbine = daw::json::from_json<Turbine>(json_data);

  CHECK(turbine.uid == 5);
  CHECK(turbine.name == "TURBINE_A");
  CHECK(!turbine.active.has_value());
  CHECK(!turbine.capacity.has_value());
  CHECK(!turbine.conversion_rate.has_value());
  CHECK(!turbine.drain.has_value());
}
