#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_generator.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Generator daw json test 1")
{
  using Uid = gtopt::Uid;

  std::string_view test_001_t_json_data = R"({
    "uid":5,
    "name":"GUACOLDA",
    "bus":10,
    "capacity":300,
    "pmin":0,
    "pmax":275.5
    })";

  gtopt::Generator gen =
      daw::json::from_json<gtopt::Generator>(test_001_t_json_data);

  REQUIRE(gen.uid == 5);
  REQUIRE(gen.name == "GUACOLDA");
  REQUIRE(std::get<Uid>(gen.bus) == 10);
  REQUIRE(std::get<double>(gen.pmin.value_or(-1.0)) == doctest::Approx(0));
  REQUIRE(std::get<double>(gen.capacity.value_or(-1.0))
          == doctest::Approx(300));
  REQUIRE(std::get<double>(gen.pmax.value_or(-1.0)) == doctest::Approx(275.5));
  CHECK_FALSE(gen.type.has_value());
}

TEST_CASE("Generator daw json test 2")
{
  using Name = gtopt::Name;

  std::string_view test_001_t_json_data = R"({
    "uid":5,
    "name":"GUACOLDA",
    "bus":"GUACOLDA",
    "capacity":300,
    "pmin":0,
    "pmax":275.5
    })";

  gtopt::Generator gen =
      daw::json::from_json<gtopt::Generator>(test_001_t_json_data);

  REQUIRE(gen.uid == 5);
  REQUIRE(gen.name == "GUACOLDA");
  REQUIRE(std::get<Name>(gen.bus) == "GUACOLDA");
  REQUIRE(std::get<double>(gen.pmin.value_or(-1.0)) == doctest::Approx(0));
  REQUIRE(std::get<double>(gen.capacity.value_or(-1.0))
          == doctest::Approx(300));
  REQUIRE(std::get<double>(gen.pmax.value_or(-1.0)) == doctest::Approx(275.5));
}

TEST_CASE("Generator JSON type field round-trip")
{
  using Uid = gtopt::Uid;

  std::string_view json_data = R"({
    "uid": 7,
    "name": "SOLAR_1",
    "type": "solar",
    "bus": 3,
    "capacity": 100
  })";

  const auto gen = daw::json::from_json<gtopt::Generator>(json_data);

  REQUIRE(gen.uid == 7);
  REQUIRE(gen.name == "SOLAR_1");
  REQUIRE(gen.type.value_or("") == "solar");
  REQUIRE(std::get<Uid>(gen.bus) == 3);

  // Round-trip
  const auto json = daw::json::to_json(gen);
  const auto gen2 = daw::json::from_json<gtopt::Generator>(json);
  CHECK(gen2.type.value_or("") == "solar");
}

TEST_CASE("Generator JSON type field absent is nullopt")
{
  std::string_view json_data = R"({
    "uid": 1,
    "name": "G1",
    "bus": 1,
    "capacity": 200
  })";

  const auto gen = daw::json::from_json<gtopt::Generator>(json_data);
  CHECK_FALSE(gen.type.has_value());
}
