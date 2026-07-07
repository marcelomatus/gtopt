// SPDX-License-Identifier: BSD-3-Clause
//
// Phase 3 LP integration tests for the AllowancePool ↔ EmissionZone
// coupling.  When an `EmissionZone` carries an `allowance_pool` FK, its
// per-block `production` columns are injected as a drawdown into the
// pool's energy-balance rows (the banked SoC becomes the binding
// multi-stage cap) and the zone's own standalone per-stage `cap` row is
// skipped.
//
// What these tests exercise:
//   * the JSON `allowance_pool` field round-trips on EmissionZone,
//   * a binding bank limits cumulative emissions across the horizon
//     (dirty cheap generator is throttled, clean expensive one fills
//     the gap),
//   * a zone `cap` set ALONGSIDE the pool FK is ignored (pool wins),
//   * an ample bank does not over-constrain dispatch.

#include <doctest/doctest.h>
#include <gtopt/allowance_pool_lp.hpp>
#include <gtopt/emission_zone_lp.hpp>
#include <gtopt/json/json_emission_zone.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace test_allowance_pool_coupling
{

/// Two-stage chronological simulation (each stage = 24 hourly blocks).
Simulation make_2stage_24h_simulation()
{
  Simulation sim;
  sim.block_array.reserve(48);
  for (int i = 0; i < 48; ++i) {
    sim.block_array.push_back(Block {
        .uid = Uid {i},
        .duration = 1.0,
    });
  }
  sim.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 24,
          .chronological = true,
      },
      {
          .uid = Uid {1},
          .first_block = 24,
          .count_block = 24,
          .chronological = true,
      },
  };
  sim.scenario_array = {
      {
          .uid = Uid {0},
      },
  };
  return sim;
}

/// Copperplate system with a dirty-cheap generator (g1, emits CO₂) and
/// a clean-expensive one (g2, no emissions), a CO₂ EmissionZone wired
/// to an AllowancePool, and a single EmissionSource binding g1 → zone.
/// `pool_eini` sizes the bank; `zone_cap` (when > 0) adds a standalone
/// cap row that the coupling is expected to ignore.
System make_coupled_system(double pool_eini, double zone_cap = -1.0)
{
  System sys;
  sys.name = "co2_phase3_coupling";
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

  EmissionZone zone {
      .uid = Uid {1},
      .name = "global_co2",
      .emissions = {{.emission = Uid {1}, .weight = 1.0}},
      .allowance_pool = OptSingleId {Uid {1}},
  };
  if (zone_cap >= 0.0) {
    zone.cap = zone_cap;
  }
  sys.emission_zone_array = {std::move(zone)};

  sys.emission_source_array = {{.uid = Uid {1},
                                .name = "g1_co2",
                                .generator = OptSingleId {Uid {1}},
                                .zone = Uid {1},
                                .emission = Uid {1},
                                .rate = 0.5}};

  sys.allowance_pool_array = {
      {
          .uid = Uid {1},
          .name = "co2_pool",
          .emin = 0.0,
          .emax = 1.0e6,
          .eini = pool_eini,
          .capacity = 1.0e6,
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };
  return sys;
}

/// Build the LP for `sys` on the standard 2-stage / 24h simulation,
/// solve, and return the raw (scaled) objective.  Fails the test on a
/// solve error.
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

}  // namespace test_allowance_pool_coupling

TEST_CASE("EmissionZone JSON — allowance_pool FK round-trips")  // NOLINT
{
  constexpr std::string_view js = R"({
    "uid": 7,
    "name": "ets_zone",
    "emission": "co2",
    "allowance_pool": 3
  })";

  const auto z = daw::json::from_json<EmissionZone>(js);
  CHECK(z.uid == Uid {7});
  REQUIRE(z.allowance_pool.has_value());
  // Singular `emission` shortcut folds into a 1-element emissions list.
  REQUIRE(z.emissions.size() == 1);

  // Round-trip back out and parse again — the FK survives.
  const auto js2 = daw::json::to_json(z);
  const auto z2 = daw::json::from_json<EmissionZone>(js2);
  CHECK(z2.allowance_pool.has_value());
}

TEST_CASE(  // NOLINT
    "AllowancePool ↔ EmissionZone: binding bank throttles dirty generator")
{
  using namespace test_allowance_pool_coupling;
  // Bank = 100 tCO₂, g1 rate = 0.5 t/MWh ⇒ g1 capped at 200 MWh of
  // generation over the whole 48 h horizon.  Demand needs 10 MW × 48 h
  // = 480 MWh, so the clean-expensive g2 must cover 280 MWh.
  //   cost = 200·$10 + 280·$50 = $16 000 → 16.0 scaled.
  const System sys = make_coupled_system(/*pool_eini=*/100.0);

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
  CHECK(result.value() == 0);

  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(16.0).epsilon(0.01));
}

TEST_CASE(  // NOLINT
    "AllowancePool coupling: zone standalone cap is skipped when pool set")
{
  using namespace test_allowance_pool_coupling;
  // Same bank (100 tCO₂) but ALSO a tiny zone cap = 10 tCO₂.  If the
  // standalone cap row were active it would throttle g1 to 20 MWh and
  // push the objective far above 16.0.  Because the pool mediates the
  // cap, the cap row is skipped and the objective stays at 16.0 — the
  // bank (100 t) binds, not the cap (10 t).
  const System sys = make_coupled_system(/*pool_eini=*/100.0,
                                         /*zone_cap=*/10.0);

  const auto simulation = make_2stage_24h_simulation();
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  // The cap row must NOT be present for a pool-coupled zone.
  const auto& zones = system_lp.elements<EmissionZoneLP>();
  REQUIRE(zones.size() == 1);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(16.0).epsilon(0.01));
}

TEST_CASE(  // NOLINT
    "AllowancePool coupling: ample bank does not bind dispatch")
{
  using namespace test_allowance_pool_coupling;
  // Bank = 1e6 tCO₂ ≫ the 240 tCO₂ the dirty generator would emit
  // serving all 480 MWh.  The coupling must not over-constrain: g1
  // serves everything at $10/MWh ⇒ cost = 480·$10 = $4 800 → 4.8.
  const System sys = make_coupled_system(/*pool_eini=*/1.0e6);

  const auto simulation = make_2stage_24h_simulation();
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto obj = li.get_obj_value_raw();
  CHECK(obj == doctest::Approx(4.8).epsilon(0.01));
}

TEST_CASE("AllowancePool coupling: zone references pool by NAME")  // NOLINT
{
  using namespace test_allowance_pool_coupling;
  // The `allowance_pool` FK must resolve by element name, not only uid.
  // First confirm the JSON parses the string form into a Name variant,
  // then confirm the LP path resolves it (same binding bank ⇒ 16.0).
  constexpr std::string_view js = R"({
    "uid": 1, "name": "z", "emission": "co2", "allowance_pool": "co2_pool"
  })";
  const auto z = daw::json::from_json<EmissionZone>(js);
  REQUIRE(z.allowance_pool.has_value());
  CHECK(std::holds_alternative<Name>(*z.allowance_pool));
  CHECK(std::get<Name>(*z.allowance_pool) == "co2_pool");

  auto sys = make_coupled_system(/*pool_eini=*/100.0);
  sys.emission_zone_array[0].allowance_pool = OptSingleId {Name {"co2_pool"}};
  CHECK(solve_obj(sys) == doctest::Approx(16.0).epsilon(0.01));
}

TEST_CASE(  // NOLINT
    "AllowancePool coupling: two zones drain one shared pool additively")
{
  using namespace test_allowance_pool_coupling;
  // g1 and g2 are identical dirty-cheap units (0.5 t/MWh, $10), each in
  // its own EmissionZone, both pointing at ONE pool (eini = 100 t).  g3
  // is a clean backstop ($50).  The shared bank caps COMBINED emissions
  // at 100 t ⇒ g1+g2 ≤ 200 MWh; g3 serves the remaining 280 MWh.
  //   obj = (200·$10 + 280·$50)/1000 = 16.0
  // If the two zones' drawdowns OVERWROTE (instead of accumulating) on
  // the shared energy row, only one generator would be capped and the
  // other would serve all 480 MWh at $10 ⇒ obj 4.8.  The 16.0 golden
  // proves the production columns add into the same row.
  System sys;
  sys.name = "co2_two_zone_pool";
  sys.bus_array = {{.uid = Uid {1}, .name = "b1"}};
  sys.demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 10.0},
  };
  sys.generator_array = {
      {.uid = Uid {1},
       .name = "g1",
       .bus = Uid {1},
       .pmin = 0.0,
       .pmax = 100.0,
       .gcost = 10.0,
       .capacity = 100.0},
      {.uid = Uid {2},
       .name = "g2",
       .bus = Uid {1},
       .pmin = 0.0,
       .pmax = 100.0,
       .gcost = 10.0,
       .capacity = 100.0},
      {.uid = Uid {3},
       .name = "g3_clean",
       .bus = Uid {1},
       .pmin = 0.0,
       .pmax = 100.0,
       .gcost = 50.0,
       .capacity = 100.0},
  };
  sys.emission_array = {{.uid = Uid {1}, .name = "co2"}};
  sys.emission_zone_array = {
      {.uid = Uid {1},
       .name = "z1",
       .emissions = {{.emission = Uid {1}, .weight = 1.0}},
       .allowance_pool = OptSingleId {Uid {1}}},
      {.uid = Uid {2},
       .name = "z2",
       .emissions = {{.emission = Uid {1}, .weight = 1.0}},
       .allowance_pool = OptSingleId {Uid {1}}},
  };
  sys.emission_source_array = {
      {.uid = Uid {1},
       .name = "g1_co2",
       .generator = OptSingleId {Uid {1}},
       .zone = Uid {1},
       .emission = Uid {1},
       .rate = 0.5},
      {.uid = Uid {2},
       .name = "g2_co2",
       .generator = OptSingleId {Uid {2}},
       .zone = Uid {2},
       .emission = Uid {1},
       .rate = 0.5},
  };
  sys.allowance_pool_array = {
      {.uid = Uid {1},
       .name = "co2_pool",
       .emin = 0.0,
       .emax = 1.0e6,
       .eini = 100.0,
       .capacity = 1.0e6,
       .use_state_variable = true,
       .daily_cycle = false},
  };
  CHECK(solve_obj(sys) == doctest::Approx(16.0).epsilon(0.01));
}

TEST_CASE(  // NOLINT
    "AllowancePool coupling: per-ton price coexists with pool drawdown")
{
  using namespace test_allowance_pool_coupling;
  // `price` (carbon tax on the production column) and `allowance_pool`
  // (bank drawdown) wire independently — both must apply together.
  SUBCASE("ample bank — tax applies to the full emissions")
  {
    // Bank ≫ demand ⇒ g1 serves all 480 MWh (240 tCO₂).  All-in g1 cost
    // = $10 + $20·0.5 = $20/MWh, still below clean g2 ($50).
    //   obj = (480·$10 + 240·$20)/1000 = (4800 + 4800)/1000 = 9.6
    auto sys = make_coupled_system(/*pool_eini=*/1.0e6);
    sys.emission_zone_array[0].price = 20.0;
    CHECK(solve_obj(sys) == doctest::Approx(9.6).epsilon(0.01));
  }
  SUBCASE("binding bank — tax applies only to the abated 100 tCO₂")
  {
    // Bank caps g1 at 200 MWh (100 tCO₂); g2 covers 280 MWh.
    //   obj = 16.0 (dispatch) + 100·$20/1000 = 16.0 + 2.0 = 18.0
    auto sys = make_coupled_system(/*pool_eini=*/100.0);
    sys.emission_zone_array[0].price = 20.0;
    CHECK(solve_obj(sys) == doctest::Approx(18.0).epsilon(0.01));
  }
}
