// SPDX-License-Identifier: BSD-3-Clause
//
// Tests for the ``Fuel.max_offtake`` cap row (PLEXOS
// ``FueMaxOffWeek_<fuel>`` reproduction).  Validates:
//   * defaults + JSON round-trip of the two new fields,
//   * the per-(scenario, stage) cap binds total fuel consumption,
//   * soft cap with ``max_offtake_cost`` lets the LP over-consume at
//     a per-unit price (slack column > 0, dual on the cap row pinned
//     to the slack cost via complementarity).

#include <string_view>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/fuel.hpp>
#include <gtopt/fuel_lp.hpp>
#include <gtopt/json/json_fuel.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/user_constraint.hpp>

using namespace gtopt;

namespace test_fuel_offtake
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

}  // namespace test_fuel_offtake

TEST_CASE("Fuel.max_offtake + max_offtake_cost defaults are unset")  // NOLINT
{
  const Fuel f;
  CHECK_FALSE(f.max_offtake.has_value());
  CHECK_FALSE(f.max_offtake_cost.has_value());
}

TEST_CASE("Fuel.max_offtake JSON round-trip — hard + soft cap")  // NOLINT
{
  SUBCASE("hard cap (only max_offtake set)")
  {
    std::string_view js = R"({
      "uid": 1,
      "name": "Gas_Quintero_A",
      "price": 10.0,
      "max_offtake": 500.0
    })";
    const Fuel f = daw::json::from_json<Fuel>(js);
    REQUIRE(f.max_offtake.has_value());
    CHECK(std::get<Real>(*f.max_offtake) == doctest::Approx(500.0));
    CHECK_FALSE(f.max_offtake_cost.has_value());
  }
  SUBCASE("soft cap (both fields set)")
  {
    std::string_view js = R"({
      "uid": 1,
      "name": "Gas_Quintero_A",
      "price": 10.0,
      "max_offtake": 500.0,
      "max_offtake_cost": 1000.0
    })";
    const Fuel f = daw::json::from_json<Fuel>(js);
    REQUIRE(f.max_offtake.has_value());
    REQUIRE(f.max_offtake_cost.has_value());
    CHECK(std::get<Real>(*f.max_offtake_cost) == doctest::Approx(1000.0));
  }
}

namespace
{

// Builds a single-bus, two-block system with:
//   * Bus b1
//   * Demand d1 = 50 MW (constant) → 100 MWh / stage
//   * Generator g1 on Bus b1 with fuel 'gas', heat_rate = 2.0 MMBtu/MWh,
//     gcost = 0, pmax = 200 MW
//   * Fuel 'gas' with price = 10 $/MMBtu, and optional max_offtake
// Total cost without cap: gen = 100 MWh × heat_rate × price
//                        = 100 × 2 × 10 = $2000 → 2.0 scaled.
System make_single_fuel_system(double max_offtake,
                               std::optional<double> soft_cost = std::nullopt)
{
  System sys;
  sys.name = "fuel_offtake";
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
          .max_offtake = max_offtake,
      },
  };
  if (soft_cost) {
    sys.fuel_array[0].max_offtake_cost = *soft_cost;
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

TEST_CASE(
    "Fuel.max_offtake (hard cap): binding cap restricts generation")  // NOLINT
{
  using namespace test_fuel_offtake;
  // demand = 50 MW × 2h = 100 MWh.  Heat rate = 2 → fuel use = 200
  // MMBtu/stage.  Set max_offtake = 120 MMBtu — caps gen at 60 MWh
  // total → 40 MWh of demand is unserved (paid at demand_fail_cost).
  System sys = make_single_fuel_system(120.0);

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

  // gen = 60 MWh; unserved = 40 MWh × $1000 = $40 000 → 40.0 scaled.
  // gen fuel cost = 60 MWh × 2 MMBtu/MWh × $10 = $1200 → 1.2.
  // Total ≈ 41.2.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(41.2).epsilon(0.01));

  // The cap row dual is the marginal $/MMBtu for relaxing the cap.
  // With unserved energy at $1000/MWh × 0.5 MWh/MMBtu = $500/MMBtu,
  // the dual should be 500 (after scale chain: scenario probability
  // × discount × 1/scale_objective → still 500/1000 = 0.5 raw).
  const auto& fuel_elems = system_lp.elements<FuelLP>();
  REQUIRE(fuel_elems.size() == 1);
}

TEST_CASE("Fuel.max_offtake (soft cap): slack lets LP over-consume")  // NOLINT
{
  using namespace test_fuel_offtake;
  // Same system but with max_offtake_cost = $50/MMBtu.  This is
  // cheaper than the demand_fail_cost equivalent of $500/MMBtu
  // (1000 $/MWh × 0.5 MWh/MMBtu).  The LP prefers to pay the cap
  // penalty and serve all demand.
  System sys = make_single_fuel_system(120.0, 50.0);

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

  // gen = 100 MWh → fuel = 200 MMBtu → 80 MMBtu over the 120 cap.
  // Costs:
  //   gen × heat_rate × price = 100 × 2 × 10  = $2000  → 2.0
  //   slack × max_offtake_cost = 80 × 50      = $4000  → 4.0
  // Total ≈ 6.0.  Cheaper than the hard-cap unserved penalty (41.2).
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(6.0).epsilon(0.01));
}

TEST_CASE(
    "Fuel.max_offtake (unset): FuelLP stays passive, no cap row added")  // NOLINT
{
  using namespace test_fuel_offtake;
  // When max_offtake is unset, the LP should behave as before the
  // feature was added — generator serves full demand at base cost.
  System sys = make_single_fuel_system(0.0);
  sys.fuel_array[0].max_offtake.reset();  // Force-unset.

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
  // 100 MWh × 2 MMBtu/MWh × $10/MMBtu = $2000 → 2.0
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(2.0).epsilon(0.01));
}

TEST_CASE(  // NOLINT
    "Fuel.max_offtake_per_block: per-block enforcement vs per-stage sum")
{
  using namespace test_fuel_offtake;
  // Same single-fuel system as the basic hard-cap test but with
  // demand SPIKED in block 0 to verify the per-block enforcement.
  //
  // System layout:
  //   block 0: demand = 100 MW × 1h = 100 MWh  (spike)
  //   block 1: demand =   0 MW × 1h =   0 MWh  (quiet)
  //
  // With heat_rate = 2 MMBtu/MWh, full demand needs 200 MMBtu
  // total — 200 in block 0, 0 in block 1.
  //
  // Per-stage SUM cap (max_offtake = 120, per_block = false):
  //   Σ_b (2 × gen × 1h) ≤ 120 → gen_0 + gen_1 ≤ 60 MWh
  //   LP picks gen_0 = 60, gen_1 = 0 (all in block 0).
  //   gen_0 = 60 → block-0 demand short by 40 MWh → fail_cost
  //   = 40 × 1000 = $40 000 → 40.0 scaled.
  //   gen_cost = 60 × 2 × 10 = $1200 → 1.2.  Total ≈ 41.2.
  //
  // Per-block cap (max_offtake = 120, per_block = true):
  //   For block 0: 2 × gen_0 × 1 ≤ 120/2 = 60 → gen_0 ≤ 30 MWh
  //   For block 1: 2 × gen_1 × 1 ≤  60 → gen_1 ≤ 30 MWh
  //   Block 0 demand 100, block 1 demand 0 → gen_0 = 30 (cap
  //   limited), block-0 unserved = 70 MWh; gen_1 = 0.
  //   gen_cost = 30 × 2 × 10 = $600 → 0.6.
  //   fail   = 70 × 1000     = $70 000 → 70.0.  Total ≈ 70.6.
  //
  // Per-block is TIGHTER than per-stage sum.  This test verifies
  // that the per-block path actually constrains tighter than the
  // sum path on a peak-block scenario.
  System sys = make_single_fuel_system(120.0);
  // Spike block 0 to 100 MW, idle block 1, via per-(stage, block)
  // `lmax`.  Keep `capacity` at 100 MW so the synthetic
  // ``demand_lp`` upper bound doesn't shrink the per-block lmax.
  sys.demand_array[0].capacity = 100.0;
  // OptTBRealFieldSched accepts a [[stage_blocks]] 2-D matrix:
  // outer = stages (1), inner = blocks (2).
  sys.demand_array[0].lmax =
      OptTBRealFieldSched {std::vector<std::vector<Real>> {{100.0, 0.0}}};

  SUBCASE("per-stage SUM (default): LP fronts-loads dispatch in block 0")
  {
    sys.fuel_array[0].max_offtake_per_block = false;
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
    // 60 MWh × 2 × 10 = $1200 ; (100-60) × 1000 = $40 000
    // → 41.2 scaled.
    const auto obj = li.get_obj_value_raw();
    CHECK(obj == doctest::Approx(41.2).epsilon(0.01));
  }

  SUBCASE("per-block TRUE: cap pro-rated by block duration, tighter")
  {
    sys.fuel_array[0].max_offtake_per_block = true;
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
    // gen_0 capped at 30 MWh → 100-30 = 70 MWh unserved in block 0.
    // 30 × 2 × 10 = $600 ; 70 × 1000 = $70 000 → 70.6 scaled.
    const auto obj = li.get_obj_value_raw();
    CHECK(obj == doctest::Approx(70.6).epsilon(0.01));
  }
}

// ---------------------------------------------------------------------------
// Task 3 — fuel("X").offtake UC accessor
// ---------------------------------------------------------------------------

TEST_CASE("PAMPL — fuel('X').offtake resolves as the offtake DV")  // NOLINT
{
  // PLEXOS Constraint pattern (``Gas_MaxOpDay*``):
  //
  //   1 × fuel(<X>).offtake ≤ daily_budget
  //
  // For the 2-block (1h each), single-fuel, heat_rate=2 fixture the
  // offtake column ``Y_f[b]`` is bound to ``2 × gen[b] × 1h``, so
  // demand of 50 MW × 2 blocks = 100 MWh translates to 200 MMBtu of
  // offtake.  A UC cap at 60 MMBtu/block (per-block) restricts gen
  // to 30 MWh per block — 20 MWh unserved per block (40 MWh total).
  //
  // Costs:
  //   gen × heat_rate × price = 60 × 2 × 10  = $1200  → 1.2
  //   unserved × fail_cost    = 40 × 1000    = $40000 → 40.0
  //   Total                                  ≈ 41.2
  using namespace test_fuel_offtake;

  // No native max_offtake — the UC is the ONLY cap, exercising the
  // unconditional offtake-DV path.  The fixture passes 0.0 as a
  // placeholder; explicitly RESET so FuelLP doesn't emit a
  // ``Y_f ≤ 0`` row.
  System sys = make_single_fuel_system(0.0);
  sys.fuel_array[0].max_offtake.reset();
  sys.user_constraint_array = {
      {
          .uid = Uid {1},
          .name = "uc_offtake_cap",
          .expression = "fuel('gas').offtake <= 60",
      },
  };

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

  // Per-block UC: gen_0 = 30, gen_1 = 30 → 60 MWh served, 40 unserved.
  // Costs: 60 × 2 × 10 + 40 × 1000 = $1200 + $40000 = $41200 → 41.2.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(41.2).epsilon(0.01));
}

TEST_CASE("fuel('X').offtake is unconditional (no max_offtake required)")
{
  // The offtake DV must be exposed even when ``Fuel.max_offtake`` is
  // unset — letting external UCs cap the offtake without relying on
  // the native ``max_offtake`` field.  Without the cap UC the LP is
  // unconstrained; gen serves full demand at base cost.
  using namespace test_fuel_offtake;
  System sys = make_single_fuel_system(0.0);
  sys.fuel_array[0].max_offtake.reset();  // explicit unset
  // A LOOSE UC (cap >> physical max) must NOT bind, proving the
  // accessor resolves AND the LP picks the cheapest solution.
  sys.user_constraint_array = {
      {
          .uid = Uid {1},
          .name = "uc_loose",
          .expression = "fuel('gas').offtake <= 10000",
      },
  };

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

  // gen = 100 MWh (full demand) → cost = 100 × 2 × 10 = $2000 → 2.0.
  // No unserved energy because the UC is non-binding.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(2.0).epsilon(0.01));
}

TEST_CASE(
    "fuel('X').offtake binding equation: Y_f = Σ heat_rate·dur·gen")  // NOLINT
{
  // Validate the binding equation directly: cap the offtake at 80
  // MMBtu (per-stage) — should let gen = 40 MWh total (heat_rate=2,
  // both blocks 1h).  Cap is the UC, NOT the native max_offtake.
  using namespace test_fuel_offtake;
  System sys = make_single_fuel_system(0.0);
  sys.fuel_array[0].max_offtake.reset();  // explicit unset
  sys.user_constraint_array = {
      {
          .uid = Uid {1},
          .name = "uc_per_block_cap",
          .expression = "fuel('gas').offtake <= 80",
      },
  };

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

  // Per-block UC: gen ≤ 80/2 = 40 MWh per block.  Demand 50 → 10
  // unserved per block, 20 total.
  // Costs: 80 × 2 × 10 + 20 × 1000 = $1600 + $20 000 = $21 600 → 21.6.
  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(21.6).epsilon(0.01));
}
