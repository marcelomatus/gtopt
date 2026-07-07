// SPDX-License-Identifier: BSD-3-Clause
//
// JSON round-trip tests for ``AmmoniaStorage`` + ``AmmoniaNode``.

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_ammonia_node.hpp>
#include <gtopt/json/json_ammonia_storage.hpp>

using namespace gtopt;

TEST_CASE("AmmoniaNode JSON round-trip")  // NOLINT
{
  std::string_view js = R"({"uid":22,"name":"nh3_node_1"})";
  const AmmoniaNode an = daw::json::from_json<AmmoniaNode>(js);
  CHECK(an.uid == Uid {22});
  CHECK(an.name == "nh3_node_1");
}

TEST_CASE("AmmoniaStorage JSON round-trip — refrigerated tank")  // NOLINT
{
  // 60 kt-NH3 ≈ 310 GWh_LHV.
  std::string_view js = R"({
    "uid": 1,
    "name": "nh3_tank_mejillones",
    "type": "refrigerated",
    "ammonia_node": "nh3_node_1",
    "annual_loss": 0.025,
    "emin": 0.0,
    "emax": 310000.0,
    "eini": 155000.0,
    "efin": 100000.0,
    "capacity": 310000.0,
    "use_state_variable": true,
    "daily_cycle": false
  })";
  const AmmoniaStorage as = daw::json::from_json<AmmoniaStorage>(js);

  CHECK(as.uid == Uid {1});
  CHECK(as.name == "nh3_tank_mejillones");
  CHECK(as.type.value_or(Name {}) == "refrigerated");
  REQUIRE(as.ammonia_node.has_value());
  REQUIRE(std::holds_alternative<Name>(*as.ammonia_node));
  CHECK(std::get<Name>(*as.ammonia_node) == "nh3_node_1");
  CHECK(as.eini.value_or(-1.0) == doctest::Approx(155000.0));
  CHECK(as.efin.value_or(-1.0) == doctest::Approx(100000.0));
}

TEST_CASE("AmmoniaStorage JSON: ammonia_node uid-int form")  // NOLINT
{
  std::string_view js = R"({"uid":1,"name":"tank","ammonia_node":42})";
  const AmmoniaStorage as = daw::json::from_json<AmmoniaStorage>(js);
  REQUIRE(as.ammonia_node.has_value());
  REQUIRE(std::holds_alternative<Uid>(*as.ammonia_node));
  CHECK(std::get<Uid>(*as.ammonia_node) == Uid {42});
}
