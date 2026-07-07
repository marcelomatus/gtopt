// SPDX-License-Identifier: BSD-3-Clause
//
// JSON round-trip tests for ``AllowancePool`` (Phase 1).

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_allowance_pool.hpp>

using namespace gtopt;

TEST_CASE("AllowancePool JSON round-trip — minimal")  // NOLINT
{
  std::string_view js = R"({"uid":1,"name":"co2_pool"})";
  const AllowancePool ap = daw::json::from_json<AllowancePool>(js);
  CHECK(ap.uid == Uid {1});
  CHECK(ap.name == "co2_pool");
  CHECK_FALSE(ap.emission.has_value());
  CHECK_FALSE(ap.eini.has_value());
  CHECK_FALSE(ap.delivery.has_value());
}

TEST_CASE("AllowancePool JSON round-trip — EU ETS full config")  // NOLINT
{
  // Realistic EU ETS shape: 100 Mt initial bank, 80 Mt/yr free
  // allocation, 50 Mt terminal floor at $500/t, banking enabled.
  std::string_view js = R"({
    "uid": 1,
    "name": "EU_ETS",
    "type": "eu_ets",
    "emission": "co2",
    "emin": 0.0,
    "emax": 1000000000.0,
    "eini": 100000000.0,
    "efin": 50000000.0,
    "efin_cost": 500.0,
    "delivery": 80000000.0,
    "auction_price": 85.0,
    "use_state_variable": true,
    "daily_cycle": false
  })";
  const AllowancePool ap = daw::json::from_json<AllowancePool>(js);

  CHECK(ap.uid == Uid {1});
  CHECK(ap.name == "EU_ETS");
  CHECK(ap.type.value_or(Name {}) == "eu_ets");

  REQUIRE(ap.emission.has_value());
  REQUIRE(std::holds_alternative<Name>(*ap.emission));
  CHECK(std::get<Name>(*ap.emission) == "co2");

  CHECK(ap.eini.value_or(-1.0) == doctest::Approx(100000000.0));
  CHECK(ap.efin.value_or(-1.0) == doctest::Approx(50000000.0));
  CHECK(ap.efin_cost.value_or(-1.0) == doctest::Approx(500.0));

  REQUIRE(ap.use_state_variable.has_value());
  CHECK(ap.use_state_variable.value_or(false) == true);
  REQUIRE(ap.daily_cycle.has_value());
  CHECK_FALSE(ap.daily_cycle.value_or(true));
}

TEST_CASE("AllowancePool JSON: emission accepts uid-int form")  // NOLINT
{
  std::string_view js = R"({"uid":1,"name":"pool","emission":42})";
  const AllowancePool ap = daw::json::from_json<AllowancePool>(js);
  REQUIRE(ap.emission.has_value());
  REQUIRE(std::holds_alternative<Uid>(*ap.emission));
  CHECK(std::get<Uid>(*ap.emission) == Uid {42});
}

TEST_CASE("AllowancePool JSON: borrowing config (emin < 0)")  // NOLINT
{
  std::string_view js = R"({
    "uid": 3,
    "name": "with_borrowing",
    "emin": -10000000.0
  })";
  const AllowancePool ap = daw::json::from_json<AllowancePool>(js);
  REQUIRE(ap.emin.has_value());
  CHECK(std::get<Real>(*ap.emin) == doctest::Approx(-10000000.0));
}
