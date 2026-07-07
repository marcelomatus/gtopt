// SPDX-License-Identifier: BSD-3-Clause
//
// JSON round-trip tests for ``CarrierConverter``.

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_carrier_converter.hpp>

using namespace gtopt;

TEST_CASE("CarrierConverter JSON round-trip — electrolyser")  // NOLINT
{
  // PEM electrolyser, 100 MW input, η = 0.70.
  std::string_view js = R"({
    "uid": 1,
    "name": "pem_100mw",
    "type": "electrolyser",
    "from_carrier": "electric",
    "to_carrier": "hydrogen",
    "from_node": "b1",
    "to_node": "h2_node",
    "efficiency": 0.70,
    "capacity": 100.0
  })";
  const CarrierConverter cc = daw::json::from_json<CarrierConverter>(js);

  CHECK(cc.uid == Uid {1});
  CHECK(cc.name == "pem_100mw");
  CHECK(cc.type.value_or(Name {}) == "electrolyser");
  CHECK(cc.from_carrier == Carrier::Electric);
  CHECK(cc.to_carrier == Carrier::Hydrogen);
  REQUIRE(cc.from_node.has_value());
  CHECK(std::get<Name>(*cc.from_node) == "b1");
  REQUIRE(cc.to_node.has_value());
  CHECK(std::get<Name>(*cc.to_node) == "h2_node");
}

TEST_CASE("CarrierConverter JSON round-trip — Haber-Bosch")  // NOLINT
{
  std::string_view js = R"({
    "uid": 2,
    "name": "haber_bosch_plant",
    "type": "haber_bosch",
    "from_carrier": "hydrogen",
    "to_carrier": "ammonia",
    "from_node": "h2_node",
    "to_node": "nh3_node",
    "efficiency": 0.70,
    "capacity": 50.0
  })";
  const CarrierConverter cc = daw::json::from_json<CarrierConverter>(js);
  CHECK(cc.from_carrier == Carrier::Hydrogen);
  CHECK(cc.to_carrier == Carrier::Ammonia);
}

TEST_CASE("CarrierConverter JSON: typo on carrier throws")  // NOLINT
{
  std::string_view js = R"({
    "uid": 3,
    "name": "bad",
    "from_carrier": "electricity",
    "to_carrier": "hydrogen"
  })";
  CHECK_THROWS_AS(
      [[maybe_unused]] auto v = daw::json::from_json<CarrierConverter>(js),
      std::invalid_argument);
}
