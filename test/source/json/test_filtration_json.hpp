#pragma once

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_filtration.hpp>

TEST_CASE("Filtration daw json test 1")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"FILTER_A",
    "waterway":10,
    "reservoir":20,
    "slope":1.5,
    "constant":100.0
  })";

  gtopt::Filtration filt = daw::json::from_json<gtopt::Filtration>(json_data);

  REQUIRE(filt.uid == 5);
  REQUIRE(filt.name == "FILTER_A");
  REQUIRE(std::get<Uid>(filt.waterway) == Uid(10));
  REQUIRE(std::get<Uid>(filt.reservoir) == 20);
  REQUIRE(filt.slope == 1.5);
  REQUIRE(filt.constant == 100.0);
}

TEST_CASE("Filtration daw json test 2")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"FILTER_A",
    "waterway":10,
    "reservoir":20
  })";

  gtopt::Filtration filt = daw::json::from_json<gtopt::Filtration>(json_data);

  REQUIRE(filt.uid == 5);
  REQUIRE(filt.name == "FILTER_A");
  REQUIRE(std::get<Uid>(filt.waterway) == 10);
  REQUIRE(std::get<Uid>(filt.reservoir) == 20);
  REQUIRE(filt.slope == 0.0);  // default value
  REQUIRE(filt.constant == 0.0);  // default value
}

TEST_CASE("Filtration array json test")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"FILTER_A",
    "waterway":10,
    "reservoir":20
  },{
    "uid":15,
    "name":"FILTER_B",
    "waterway":30,
    "reservoir":40,
    "slope":2.0,
    "constant":200.0
  }])";

  std::vector<gtopt::Filtration> filts =
      daw::json::from_json_array<gtopt::Filtration>(json_data);

  REQUIRE(filts[0].uid == 5);
  REQUIRE(filts[0].name == "FILTER_A");
  REQUIRE(std::get<Uid>(filts[0].waterway) == 10);
  REQUIRE(std::get<Uid>(filts[0].reservoir) == 20);

  REQUIRE(filts[1].uid == 15);
  REQUIRE(filts[1].name == "FILTER_B");
  REQUIRE(std::get<Uid>(filts[1].waterway) == 30);
  REQUIRE(std::get<Uid>(filts[1].reservoir) == 40);
  REQUIRE(filts[1].slope == 2.0);
  REQUIRE(filts[1].constant == 200.0);
}

TEST_CASE("Filtration with active property serialization")
{
  SUBCASE("With boolean active")
  {
    Filtration filt;
    filt.uid = 1;
    filt.name = "test_filt";
    filt.active = True;

    auto json = daw::json::to_json(filt);
    const Filtration roundtrip = daw::json::from_json<Filtration>(json);

    CHECK(roundtrip.active.has_value());
    CHECK(std::get<IntBool>(roundtrip.active.value_or(False)) == True);
  }

  SUBCASE("With schedule active")
  {
    Filtration filt;
    filt.uid = 1;
    filt.name = "test_filt";
    filt.active = std::vector<IntBool> {True, False, True, False};

    auto json = daw::json::to_json(filt);
    Filtration roundtrip = daw::json::from_json<Filtration>(json);

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
  }
}

TEST_CASE("Filtration with empty optional fields")
{
  std::string_view json_data = R"({
    "uid":5,
    "name":"FILTER_A",
    "waterway":10,
    "reservoir":20
  })";

  const Filtration filt = daw::json::from_json<Filtration>(json_data);

  CHECK(filt.uid == 5);
  CHECK(filt.name == "FILTER_A");
  CHECK(filt.active.has_value() == false);
  CHECK(filt.slope == 0.0);  // null defaults to 0.0
  CHECK(filt.constant == 0.0);  // null defaults to 0.0
}
