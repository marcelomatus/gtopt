// SPDX-License-Identifier: BSD-3-Clause
//
// Unit tests for the ``AllowancePool`` data struct (Phase 1).
// LP integration tests land in subsequent phases — this commit
// covers defaults, attribute assignment, and JSON round-trip
// only.

#include <doctest/doctest.h>
#include <gtopt/allowance_pool.hpp>

using namespace gtopt;

TEST_CASE("AllowancePool default values")  // NOLINT
{
  const AllowancePool ap;
  CHECK(ap.uid == Uid {unknown_uid});
  CHECK(ap.name == Name {});
  CHECK_FALSE(ap.active.has_value());
  CHECK_FALSE(ap.type.has_value());
  CHECK_FALSE(ap.emission.has_value());

  CHECK_FALSE(ap.emin.has_value());
  CHECK_FALSE(ap.emax.has_value());
  CHECK_FALSE(ap.ecost.has_value());
  CHECK_FALSE(ap.eini.has_value());
  CHECK_FALSE(ap.efin.has_value());
  CHECK_FALSE(ap.efin_cost.has_value());

  CHECK_FALSE(ap.delivery.has_value());
  CHECK_FALSE(ap.auction_price.has_value());
  CHECK_FALSE(ap.auction_cap.has_value());

  CHECK_FALSE(ap.capacity.has_value());
  CHECK_FALSE(ap.use_state_variable.has_value());
  CHECK_FALSE(ap.daily_cycle.has_value());

  CHECK(AllowancePool::class_name == LPClassName {"AllowancePool"});
}

TEST_CASE("AllowancePool attribute assignment — EU ETS scenario")  // NOLINT
{
  // Mock EU-ETS-style configuration:
  //   * 100 Mt CO2 initial bank
  //   * 80 Mt CO2 free allocation per year
  //   * No borrowing (emin = 0)
  //   * Unlimited banking (emax unset → +∞)
  //   * Mandatory 50 Mt CO2 terminal bank with $500/t shortfall
  AllowancePool ap;
  ap.uid = Uid {1};
  ap.name = "EU_ETS";
  ap.type = "eu_ets";
  ap.emission = SingleId {Name {"co2"}};
  ap.eini = 100'000'000.0;  // 100 Mt
  ap.delivery = 80'000'000.0;  // 80 Mt/year free allocation
  ap.efin = 50'000'000.0;  // 50 Mt floor at horizon
  ap.efin_cost = 500.0;  // $500/t
  ap.use_state_variable = true;
  ap.daily_cycle = false;

  CHECK(ap.uid == Uid {1});
  CHECK(ap.name == "EU_ETS");
  CHECK(ap.type.value_or(Name {}) == "eu_ets");
  REQUIRE(ap.emission.has_value());
  CHECK(std::holds_alternative<Name>(*ap.emission));
  CHECK(std::get<Name>(*ap.emission) == "co2");
  CHECK(ap.eini.value_or(-1.0) == doctest::Approx(100'000'000.0));
  CHECK(ap.efin.value_or(-1.0) == doctest::Approx(50'000'000.0));
  CHECK(ap.efin_cost.value_or(-1.0) == doctest::Approx(500.0));
  REQUIRE(ap.use_state_variable.has_value());
  CHECK(ap.use_state_variable.value_or(false) == true);
}

TEST_CASE("AllowancePool borrowing config — emin < 0")  // NOLINT
{
  // Some ETS schemes allow limited borrowing against future
  // allocations.  emin = -10 Mt CO2 means the LP can go up to
  // 10 Mt short for a stage before infeasibility.
  AllowancePool ap;
  ap.uid = Uid {2};
  ap.name = "CA_C&T_with_borrowing";
  ap.emin = -10'000'000.0;  // 10 Mt borrowing allowance

  REQUIRE(ap.emin.has_value());
  CHECK(std::get<Real>(*ap.emin) == doctest::Approx(-10'000'000.0));
}
