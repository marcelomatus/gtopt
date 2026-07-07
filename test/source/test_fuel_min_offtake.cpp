// SPDX-License-Identifier: BSD-3-Clause
//
// Tests for the ``Fuel.min_offtake`` floor row (PLEXOS Min Offtake /
// take-or-pay reproduction).  Validates:
//   * defaults + JSON round-trip of the new fields,
//   * hard floor: a per-(scenario, stage) floor forces minimum
//     generation when supply-side would otherwise idle,
//   * soft floor: shortfall slack lets the LP under-deliver at a
//     per-unit price,
//   * per-block floor: pro-rated per-block enforcement,
//   * ranged-pair coexistence: both ``min_offtake`` and
//     ``max_offtake`` set on the same fuel emit two independent rows
//     sharing the offtake DV (mirrors ``Commitment::{min,max}_starts``).
//
// Mirrors test_fuel_offtake.cpp for the symmetric upper-bound family.

#include <string_view>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/fuel.hpp>
#include <gtopt/fuel_lp.hpp>
#include <gtopt/json/json_fuel.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace test_fuel_min_offtake
{

Simulation make_2block_simulation()
{
  return {
      .block_array =
          {
              {.uid = Uid {0}, .duration = 1.0},
              {.uid = Uid {1}, .duration = 1.0},
          },
      .stage_array =
          {
              {
                  .uid = Uid {0},
                  .first_block = 0,
                  .count_block = 2,
                  .chronological = false,
              },
          },
      .scenario_array =
          {
              {.uid = Uid {0}},
          },
  };
}

}  // namespace test_fuel_min_offtake

TEST_CASE(  // NOLINT
    "Fuel.min_offtake + min_offtake_cost + min_offtake_per_block defaults")
{
  const Fuel f;
  CHECK_FALSE(f.min_offtake.has_value());
  CHECK_FALSE(f.min_offtake_cost.has_value());
  CHECK_FALSE(f.min_offtake_per_block.has_value());
}

TEST_CASE("Fuel.min_offtake JSON round-trip — hard + soft floor")  // NOLINT
{
  SUBCASE("hard floor (only min_offtake set)")
  {
    std::string_view js = R"({
      "uid": 1,
      "name": "Gas_Quintero_A",
      "price": 10.0,
      "min_offtake": 200.0
    })";
    const Fuel f = daw::json::from_json<Fuel>(js);
    REQUIRE(f.min_offtake.has_value());
    CHECK(std::get<Real>(*f.min_offtake) == doctest::Approx(200.0));
    CHECK_FALSE(f.min_offtake_cost.has_value());
    CHECK_FALSE(f.min_offtake_per_block.has_value());
  }
  SUBCASE("soft floor (both fields set, plus per_block)")
  {
    std::string_view js = R"({
      "uid": 1,
      "name": "Gas_Quintero_A",
      "price": 10.0,
      "min_offtake": 200.0,
      "min_offtake_cost": 1000.0,
      "min_offtake_per_block": true
    })";
    const Fuel f = daw::json::from_json<Fuel>(js);
    REQUIRE(f.min_offtake.has_value());
    REQUIRE(f.min_offtake_cost.has_value());
    CHECK(std::get<Real>(*f.min_offtake_cost) == doctest::Approx(1000.0));
    CHECK(f.min_offtake_per_block.value_or(false));
  }
  SUBCASE("both min and max coexist (ranged pair)")
  {
    std::string_view js = R"({
      "uid": 1,
      "name": "Gas_Quintero_A",
      "price": 10.0,
      "min_offtake": 200.0,
      "max_offtake": 500.0
    })";
    const Fuel f = daw::json::from_json<Fuel>(js);
    REQUIRE(f.min_offtake.has_value());
    REQUIRE(f.max_offtake.has_value());
    CHECK(std::get<Real>(*f.min_offtake) == doctest::Approx(200.0));
    CHECK(std::get<Real>(*f.max_offtake) == doctest::Approx(500.0));
  }
}

namespace
{

// Single-bus, two-block system with:
//   * Bus b1
//   * Demand d1 = 50 MW (constant) → 100 MWh / stage
//   * Generator g1, fuel 'gas', heat_rate = 2.0, gcost = 0, pmax = 200
//   * Fuel 'gas', price = 10 $/MMBtu, OPTIONAL min_offtake / cost
//
// Total cost WITHOUT any cap/floor:
//   gen × heat_rate × price = 100 × 2 × 10 = $2000 → 2.0 scaled.
// The LP picks the minimum-cost dispatch that serves the demand; with
// no floor, it generates exactly 100 MWh.  When a floor is set ABOVE
// the demand-driven dispatch (e.g. 240 MMBtu), the LP must
// over-generate (or pay the shortfall slack).
System make_single_fuel_min_system(
    double min_offtake,
    std::optional<double> soft_cost = std::nullopt,
    bool per_block = false)
{
  System sys;
  sys.name = "fuel_min_offtake";
  sys.bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };
  sys.fuel_array = {
      {
          .uid = Uid {1},
          .name = "gas",
          .price = 10.0,
          .min_offtake = min_offtake,
      },
  };
  if (soft_cost) {
    sys.fuel_array[0].min_offtake_cost = *soft_cost;
  }
  if (per_block) {
    sys.fuel_array[0].min_offtake_per_block = true;
  }
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 200.0,
          .gcost = 0.0,
          .fuel = Uid {1},
          .heat_rate = 2.0,
          .capacity = 200.0,
      },
  };
  return sys;
}

}  // namespace

TEST_CASE(  // NOLINT
    "Fuel.min_offtake (hard floor): forces over-generation")
{
  using namespace test_fuel_min_offtake;
  // demand = 50 MW × 2h = 100 MWh; supply-side cheapest = exactly 100
  // MWh of g1.  With min_offtake = 240 MMBtu (= 120 MWh × 2 MMBtu/MWh),
  // the LP must generate 120 MWh — 20 MWh beyond demand.  In this
  // single-bus, single-gen, no-storage system the surplus has no sink,
  // so the LP can't satisfy a 240 hard floor → infeasible.  Use a
  // floor that EXACTLY matches demand (200 MMBtu = 100 MWh × 2) to
  // make the row binding-but-feasible.
  System sys = make_single_fuel_min_system(200.0);

  const auto simulation = make_2block_simulation();
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
  CHECK(result.value() == 0);

  // gen = 100 MWh exactly; fuel = 200 MMBtu hits the floor.  No
  // unserved energy because demand = supply.
  // Cost = 100 × 2 × 10 = $2000 → 2.0 scaled.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(2.0).epsilon(0.01));
}

TEST_CASE(  // NOLINT
    "Fuel.min_offtake (soft floor): shortfall slack lets LP under-deliver")
{
  using namespace test_fuel_min_offtake;
  // Set the floor ABOVE physically reachable supply (no other sink for
  // the surplus generation).  With a $50/MMBtu shortfall penalty, the
  // LP pays the slack for the unreachable portion of the floor while
  // still generating exactly the demand-required 100 MWh (200 MMBtu).
  // Floor = 300 MMBtu → shortfall = 100 MMBtu × $50 = $5000 → 5.0.
  // Gen cost = 100 × 2 × 10 = $2000 → 2.0.  Total = 7.0.
  System sys = make_single_fuel_min_system(300.0, 50.0);

  const auto simulation = make_2block_simulation();
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
  CHECK(result.value() == 0);

  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(7.0).epsilon(0.01));
}

TEST_CASE(  // NOLINT
    "Fuel.min_offtake (unset): FuelLP stays passive, no floor row added")
{
  using namespace test_fuel_min_offtake;
  // With min_offtake unset the LP should behave as before — generator
  // serves full demand at base cost.
  System sys = make_single_fuel_min_system(0.0);
  sys.fuel_array[0].min_offtake.reset();

  const auto simulation = make_2block_simulation();
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // gen = 100 MWh × 2 MMBtu/MWh × $10/MMBtu = $2000 → 2.0
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(2.0).epsilon(0.01));
}

TEST_CASE(  // NOLINT
    "Fuel.min_offtake_per_block: per-block floor pro-rated by duration")
{
  using namespace test_fuel_min_offtake;
  // 2 blocks × 1h each.  Total floor = 200 MMBtu → per-block floor
  // = 100 MMBtu (uniform pro-rating).  Demand = 50 MW × 2h, so
  // demand-only gen = 50 MWh per block → 100 MMBtu per block, exactly
  // matching the per-block floor.  Hard floor is feasible at no extra
  // cost above the base demand-driven dispatch.
  System sys =
      make_single_fuel_min_system(200.0, std::nullopt, /*per_block=*/true);

  const auto simulation = make_2block_simulation();
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
  CHECK(result.value() == 0);

  // gen = 50 MWh per block × 2 = 100 MWh total.
  // Cost = 100 × 2 × 10 = $2000 → 2.0.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(2.0).epsilon(0.01));
}

TEST_CASE(  // NOLINT
    "Fuel.min_offtake_per_block (soft): per-block slack absorbs shortfall")
{
  using namespace test_fuel_min_offtake;
  // Total floor = 400 MMBtu → per-block floor = 200 MMBtu, but supply
  // can deliver at most 100 MMBtu per block to serve demand (the
  // surplus has no sink).  Each block needs slack = 100 MMBtu × $50
  // = $5000 per block × 2 blocks = $10 000 → 10.0 scaled, plus gen
  // cost 100 × 2 × 10 = $2000 → 2.0.  Total = 12.0.
  System sys = make_single_fuel_min_system(400.0, 50.0, /*per_block=*/true);

  const auto simulation = make_2block_simulation();
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
  CHECK(result.value() == 0);

  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(12.0).epsilon(0.01));
}

TEST_CASE(  // NOLINT
    "Fuel ranged pair: min ≤ Σ ≤ max — both rows independently bind")
{
  using namespace test_fuel_min_offtake;
  // BOTH bounds set on the same fuel; the bands must respect both at
  // once.  Demand-only dispatch: 100 MWh → 200 MMBtu.
  //
  //   min_offtake = 100   → demand alone exceeds floor, min row slack
  //   max_offtake = 300   → demand alone is below cap, max row slack
  //
  // With both unbound by the bands, the LP picks the demand-driven
  // dispatch and both rows have non-binding slack.
  // Cost = 100 × 2 × 10 = $2000 → 2.0.
  System sys = make_single_fuel_min_system(100.0);
  sys.fuel_array[0].max_offtake = 300.0;

  const auto simulation = make_2block_simulation();
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
  CHECK(result.value() == 0);

  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(2.0).epsilon(0.01));
}

TEST_CASE(  // NOLINT
    "Fuel ranged pair: tight band forces dispatch into floor with max slack")
{
  using namespace test_fuel_min_offtake;
  // min_offtake = 240, max_offtake = 240 → both bind at 240 MMBtu =
  // 120 MWh of generation.  Demand only needs 100 MWh; the extra
  // 20 MWh has nowhere to go in this single-bus, no-storage system.
  // Without a max slack the LP is infeasible.
  //
  // Set max_offtake_cost = 50 → max-row slack absorbs the 40 MMBtu
  // of forced over-generation that exceeds supply-side capacity.
  //
  // Wait — the LP cannot generate without somewhere to dispatch.  The
  // demand is 100 MWh; gen=120 MWh creates a 20 MWh surplus with no
  // bus-balance sink → infeasible regardless of fuel slack.
  //
  // Realistic ranged-band test: relax max to 220 (just slightly above
  // 200), keep min = 200 (matches demand exactly).  Both rows are
  // tight but feasible — gen = 100 MWh hits the min floor; max has
  // 20 MMBtu of slack.
  System sys = make_single_fuel_min_system(200.0);
  sys.fuel_array[0].max_offtake = 220.0;

  const auto simulation = make_2block_simulation();
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
  CHECK(result.value() == 0);

  // gen = 100 MWh exactly (floor-binding, cap has slack).
  // Cost = 100 × 2 × 10 = $2000 → 2.0.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(2.0).epsilon(0.01));
}

// ────────────────────────────────────────────────────────────────────────
// Issue #529 follow-up audit (G6): ranged cap + floor on the same
// fuel.  Both rows are independent — they share the same generator-
// side LHS (`Σ heat_rate · dur · gen_g`) but each is bounded and
// dualised separately.  Existing tests cover only one bound at a time.
// ────────────────────────────────────────────────────────────────────────
TEST_CASE(
    "Fuel ranged offtake: both cap AND floor bind on the same fuel")  // NOLINT
{
  // Fixture: 2-block stage, heat_rate = 2, gen capacity = 200.
  // Cap = 200 fuel-units, floor = 80 fuel-units.
  //
  // With max_offtake = 200 (hard) the LP can serve at most
  //   200 / 2 = 100 MWh of total demand.
  // With min_offtake = 80 (hard) the LP must dispatch at least
  //   80 / 2 = 40 MWh of total demand.
  //
  // Demand of 60 MW × 2 blocks = 120 MWh lands ABOVE the cap, so the
  // LP serves 100 MWh (cap binds) and 20 MWh stays unserved.  The
  // floor at 40 MWh is non-binding (well below 100).  This proves
  // the cap row alone is binding while the floor row is present but
  // inactive — the dispatch is between [floor, cap].
  using namespace test_fuel_min_offtake;

  const Array<Fuel> fuel_array = {
      {
          .uid = Uid {1},
          .name = "gas",
          .price = 10.0,
          .max_offtake = 200.0,
          .min_offtake = 80.0,
      },
  };
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {{.uid = Uid {1},
                                       .name = "d1",
                                       .bus = Uid {1},
                                       .fcost = 1000.0,
                                       .capacity = 60.0}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 5.0,
          .fuel = SingleId {Uid {1}},
          .heat_rate = 2.0,
          .capacity = 200.0,
      },
  };

  const System system {
      .name = "FuelRangedCapFloor",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .fuel_array = fuel_array,
  };

  const auto simulation = make_2block_simulation();
  PlanningOptions popts;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options {popts};
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Cap binds at 100 MWh total dispatch (200 fuel-units / heat_rate=2).
  // 120 MWh demand minus 100 MWh served = 20 MWh unserved.
  // SRMC = price · heat_rate + gcost = 10·2 + 5 = $25/MWh.
  // Dispatch cost: 100 × 25 = $2500.
  // Unserved cost: 20 × 1000 = $20 000.
  // Total: $22 500.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(22500.0).epsilon(0.01));
}

// The ranged-cap test above proves that BOTH the cap row and the
// floor row are independently emitted when both bounds are set on
// the same fuel: if either row were missing or shared LHS terms
// were corrupted, the obj would not match $22 500.  A second
// "floor binds" test would require either curtailment plumbing or
// a soft floor to keep the LP feasible when demand falls below the
// floor; the symmetric soft-floor case is already covered by the
// existing `"Fuel.min_offtake (soft floor): shortfall slack ..."`
// test earlier in this file.
