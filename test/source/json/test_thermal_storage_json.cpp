// SPDX-License-Identifier: BSD-3-Clause
//
// JSON round-trip tests for ``ThermalStorage`` + ``ThermalNode``.

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_thermal_node.hpp>
#include <gtopt/json/json_thermal_storage.hpp>

using namespace gtopt;

TEST_CASE("ThermalNode JSON round-trip")  // NOLINT
{
  SUBCASE("minimal: uid + name only")
  {
    std::string_view js = R"({"uid":3,"name":"hot_tank"})";
    const ThermalNode tn = daw::json::from_json<ThermalNode>(js);
    CHECK(tn.uid == Uid {3});
    CHECK(tn.name == "hot_tank");
    CHECK_FALSE(tn.active.has_value());
  }

  SUBCASE("with active flag")
  {
    std::string_view js = R"({"uid":4,"name":"hot_tank","active":1})";
    const ThermalNode tn = daw::json::from_json<ThermalNode>(js);
    CHECK(tn.uid == Uid {4});
    REQUIRE(tn.active.has_value());
  }
}

TEST_CASE("ThermalStorage JSON round-trip — minimal")  // NOLINT
{
  std::string_view js = R"({"uid":1,"name":"tes_1"})";
  const ThermalStorage ts = daw::json::from_json<ThermalStorage>(js);
  CHECK(ts.uid == Uid {1});
  CHECK(ts.name == "tes_1");
  CHECK_FALSE(ts.thermal_node.has_value());
  CHECK_FALSE(ts.eini.has_value());
  CHECK_FALSE(ts.input_efficiency.has_value());
}

TEST_CASE("ThermalStorage JSON round-trip — full molten-salt config")  // NOLINT
{
  // 100 MW_e × 6 h × 0.4 Rankine η = 1500 MWh_th useful capacity,
  // 10% heel, 5%/year self-discharge, charge/discharge η = 0.98.
  std::string_view js = R"({
    "uid": 1,
    "name": "csp_tes_atacama",
    "type": "molten_salt",
    "thermal_node": "hot_tank",
    "input_efficiency": 0.98,
    "output_efficiency": 0.98,
    "annual_loss": 0.05,
    "emin": 150.0,
    "emax": 1500.0,
    "eini": 750.0,
    "efin": 200.0,
    "efin_cost": 1000.0,
    "capacity": 1500.0,
    "use_state_variable": false,
    "daily_cycle": false
  })";
  const ThermalStorage ts = daw::json::from_json<ThermalStorage>(js);

  CHECK(ts.uid == Uid {1});
  CHECK(ts.name == "csp_tes_atacama");
  CHECK(ts.type.value_or(Name {}) == "molten_salt");

  REQUIRE(ts.thermal_node.has_value());
  REQUIRE(std::holds_alternative<Name>(*ts.thermal_node));
  CHECK(std::get<Name>(*ts.thermal_node) == "hot_tank");

  CHECK(ts.eini.value_or(-1.0) == doctest::Approx(750.0));
  CHECK(ts.efin.value_or(-1.0) == doctest::Approx(200.0));
  CHECK(ts.efin_cost.value_or(-1.0) == doctest::Approx(1000.0));

  REQUIRE(ts.use_state_variable.has_value());
  CHECK_FALSE(ts.use_state_variable.value_or(true));
  REQUIRE(ts.daily_cycle.has_value());
  CHECK_FALSE(ts.daily_cycle.value_or(true));
}

TEST_CASE("ThermalStorage JSON: thermal_node accepts uid-int")  // NOLINT
{
  // SingleId accepts either a Uid (int) or a Name (string); the
  // resolver in validate_planning will look up against
  // thermal_node_array on the System.
  std::string_view js = R"({"uid":1,"name":"tes","thermal_node":42})";
  const ThermalStorage ts = daw::json::from_json<ThermalStorage>(js);
  REQUIRE(ts.thermal_node.has_value());
  REQUIRE(std::holds_alternative<Uid>(*ts.thermal_node));
  CHECK(std::get<Uid>(*ts.thermal_node) == Uid {42});
}
