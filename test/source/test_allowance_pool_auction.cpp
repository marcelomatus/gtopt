// SPDX-License-Identifier: BSD-3-Clause
//
// Phase 4 LP integration tests for AllowancePool auction purchases.
// When `AllowancePool.auction_price` is set, the bank may buy
// allowances on the market (an `auction` column, absolute tCO₂,
// `[0, auction_cap]`, injected as a `-1` inflow into the energy-balance
// row) instead of abating emissions.  The LP abates only while
// abatement is cheaper than buying.
//
// Topology (shared): a dirty-cheap generator g1 ($10/MWh, 0.5 tCO₂/MWh)
// and a clean-expensive g2 ($50/MWh, 0 tCO₂) serve a 10 MW demand over
// a 48 h horizon (480 MWh).  A CO₂ EmissionZone is wired to an
// AllowancePool whose free bank (eini = 100 tCO₂) caps g1 at 200 MWh.
//
// Marginal economics: one extra tCO₂ from g1 frees 2 MWh of g1
// generation that displaces 2 MWh of g2 — a dispatch saving of
// 2·($50−$10) = $80/tCO₂.  So the LP buys allowances iff the auction
// price is below $80/tCO₂.

#include <doctest/doctest.h>
#include <gtopt/allowance_pool_lp.hpp>
#include <gtopt/json/json_allowance_pool.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace test_allowance_pool_auction
{

Simulation make_2stage_24h_simulation()
{
  Simulation sim;
  sim.block_array.reserve(48);
  for (int i = 0; i < 48; ++i) {
    sim.block_array.push_back(Block {.uid = Uid {i}, .duration = 1.0});
  }
  sim.stage_array = {
      {.uid = Uid {0},
       .first_block = 0,
       .count_block = 24,
       .chronological = true},
      {.uid = Uid {1},
       .first_block = 24,
       .count_block = 24,
       .chronological = true},
  };
  sim.scenario_array = {{.uid = Uid {0}}};
  return sim;
}

/// Copperplate system + CO₂ zone wired to a pool.  `auction_price < 0`
/// leaves the auction unset (Phase-3 behaviour); `auction_cap < 0`
/// leaves the cap at +∞.
System make_auction_system(double auction_price, double auction_cap = -1.0)
{
  System sys;
  sys.name = "co2_phase4_auction";
  sys.bus_array = {{.uid = Uid {1}, .name = "b1"}};
  sys.demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 10.0},
  };
  sys.generator_array = {
      {.uid = Uid {1},
       .name = "g1_dirty",
       .bus = Uid {1},
       .pmin = 0.0,
       .pmax = 100.0,
       .gcost = 10.0,
       .capacity = 100.0},
      {.uid = Uid {2},
       .name = "g2_clean",
       .bus = Uid {1},
       .pmin = 0.0,
       .pmax = 100.0,
       .gcost = 50.0,
       .capacity = 100.0},
  };
  sys.emission_array = {{.uid = Uid {1}, .name = "co2"}};
  sys.emission_zone_array = {{
      .uid = Uid {1},
      .name = "global_co2",
      .emissions = {{.emission = Uid {1}, .weight = 1.0}},
      .allowance_pool = OptSingleId {Uid {1}},
  }};
  sys.emission_source_array = {{.uid = Uid {1},
                                .name = "g1_co2",
                                .generator = OptSingleId {Uid {1}},
                                .zone = Uid {1},
                                .emission = Uid {1},
                                .rate = 0.5}};

  AllowancePool pool {
      .uid = Uid {1},
      .name = "co2_pool",
      .emin = 0.0,
      .emax = 1.0e6,
      .eini = 100.0,
      .capacity = 1.0e6,
      .use_state_variable = true,
      .daily_cycle = false,
  };
  if (auction_price >= 0.0) {
    pool.auction_price = auction_price;
  }
  if (auction_cap >= 0.0) {
    pool.auction_cap = auction_cap;
  }
  sys.allowance_pool_array = {std::move(pool)};
  return sys;
}

double solve_obj(const System& sys)
{
  const auto simulation = make_2stage_24h_simulation();
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  if (!result.has_value()) {
    MESSAGE("resolve error code=" << static_cast<int>(result.error().code)
                                  << " message=" << result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);
  return li.get_obj_value_raw();
}

}  // namespace test_allowance_pool_auction

TEST_CASE(
    "AllowancePool JSON — auction_price / auction_cap round-trip")  // NOLINT
{
  constexpr std::string_view js = R"({
    "uid": 5,
    "name": "ets_pool",
    "eini": 100.0,
    "auction_price": 30.0,
    "auction_cap": 2.0
  })";
  const auto p = daw::json::from_json<AllowancePool>(js);
  REQUIRE(p.auction_price.has_value());
  REQUIRE(p.auction_cap.has_value());

  const auto js2 = daw::json::to_json(p);
  const auto p2 = daw::json::from_json<AllowancePool>(js2);
  CHECK(p2.auction_price.has_value());
  CHECK(p2.auction_cap.has_value());
}

TEST_CASE(
    "AllowancePool auction: cheap price → LP buys instead of abating")  // NOLINT
{
  using namespace test_allowance_pool_auction;
  // auction_price = $30/tCO₂ < $80 marginal abatement value ⇒ the LP
  // buys allowances and runs the dirty-cheap g1 for the whole 480 MWh.
  //   dispatch = 480·$10 = $4 800
  //   emissions = 240 tCO₂; free bank = 100 ⇒ buy 140 @ $30 = $4 200
  //   obj = (4 800 + 4 200)/1000 = 9.0   (vs 16.0 with no auction)
  const auto obj = solve_obj(make_auction_system(/*auction_price=*/30.0));
  CHECK(obj == doctest::Approx(9.0).epsilon(0.01));
}

TEST_CASE(
    "AllowancePool auction: dear price → LP abates instead of buying")  // NOLINT
{
  using namespace test_allowance_pool_auction;
  // auction_price = $200/tCO₂ > $80 ⇒ never worth buying.  The LP
  // falls back to the Phase-3 outcome: g1 capped at 200 MWh by the
  // free bank, g2 covers 280 MWh.
  //   obj = (200·$10 + 280·$50)/1000 = 16.0
  const auto obj = solve_obj(make_auction_system(/*auction_price=*/200.0));
  CHECK(obj == doctest::Approx(16.0).epsilon(0.01));
}

TEST_CASE("AllowancePool auction: cap limits purchases")  // NOLINT
{
  using namespace test_allowance_pool_auction;
  // Cheap price ($30) but auction_cap = 2 tCO₂/block ⇒ at most
  // 48·2 = 96 tCO₂ bought over the horizon.  Total allowances =
  // 100 (free) + 96 (auction) = 196 ⇒ g1 ≤ 392 MWh, g2 = 88 MWh.
  //   dispatch = 392·$10 + 88·$50 = $3 920 + $4 400 = $8 320
  //   auction  = 96·$30 = $2 880
  //   obj = (8 320 + 2 880)/1000 = 11.2
  const auto obj = solve_obj(make_auction_system(/*auction_price=*/30.0,
                                                 /*auction_cap=*/2.0));
  CHECK(obj == doctest::Approx(11.2).epsilon(0.01));
}

TEST_CASE(
    "AllowancePool auction: zero free bank → every tonne is bought")  // NOLINT
{
  using namespace test_allowance_pool_auction;
  // Empty initial bank, no free allocation — every tCO₂ must be bought
  // at the $30 market price.  g1 still serves all 480 MWh because the
  // all-in cost ($10 dispatch + $30·0.5 = $25/MWh) beats clean g2 ($50).
  //   emissions = 480·0.5 = 240 tCO₂, all auctioned
  //   obj = (480·$10 + 240·$30)/1000 = (4800 + 7200)/1000 = 12.0
  // Also pins the energy-row sign: a +1 (outflow) coefficient bug would
  // make the bank unfillable, forcing g2 to serve all → obj 24.0.
  auto sys = make_auction_system(/*auction_price=*/30.0);
  sys.allowance_pool_array[0].eini = 0.0;
  CHECK(solve_obj(sys) == doctest::Approx(12.0).epsilon(0.01));
}

TEST_CASE(
    "AllowancePool auction: unset price → no auction column built")  // NOLINT
{
  using namespace test_allowance_pool_auction;
  // With auction_price unset the auction branch is skipped entirely;
  // the pool reverts to the Phase-3 binding bank (eini=100 caps g1 at
  // 200 MWh, g2 covers 280 MWh).
  //   obj = (200·$10 + 280·$50)/1000 = 16.0
  CHECK(solve_obj(make_auction_system(/*auction_price=*/-1.0))
        == doctest::Approx(16.0).epsilon(0.01));
}
