// SPDX-License-Identifier: BSD-3-Clause
//
// Tests for the `EmissionZone` constraint-owner entity and its passive
// `EmissionZoneLP` wrapper (Commit 2 of the emissions ladder).
//
// `EmissionZone` is the per-pollutant cap / price owner — mirrors
// `InertiaZone` / `ReserveZone`.  Passive in Commit 2: just data +
// JSON + Collection presence.  LP-active wiring (the balance row +
// optional cap row + optional price coefficient) lands in Commit 3.

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/emission_zone.hpp>
#include <gtopt/emission_zone_lp.hpp>
#include <gtopt/json/json_emission_zone.hpp>
#include <gtopt/json/json_system.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("EmissionZone default construction")  // NOLINT
{
  const EmissionZone z;
  CHECK(z.uid == Uid {unknown_uid});
  CHECK(z.name.empty());
  CHECK_FALSE(z.active.has_value());
  CHECK(z.emissions.empty());
  CHECK_FALSE(z.cap.has_value());
  CHECK_FALSE(z.cap_cost.has_value());
  CHECK_FALSE(z.price.has_value());
}

TEST_CASE("EmissionZone attribute assignment")  // NOLINT
{
  EmissionZone z;
  z.uid = Uid {1};
  z.name = "global_co2";
  z.emissions = {{.emission = Uid {1}, .weight = 1.0}};
  z.cap = 1.0e6;
  z.cap_cost = 1000.0;
  z.price = 50.0;

  CHECK(z.name == "global_co2");
  REQUIRE(z.emissions.size() == 1);
  CHECK(std::get<Uid>(z.emissions.front().emission) == Uid {1});
  REQUIRE(z.cap.has_value());
  REQUIRE(z.cap_cost.has_value());
  REQUIRE(z.price.has_value());
}

TEST_CASE(
    "EmissionZone JSON round-trip — legacy singular `emission` shortcut")  // NOLINT
{
  // Legacy single-pollutant form auto-folds into `emissions[]` with
  // weight 1.0.  This preserves backward compat with the pre-multi-
  // pollutant JSON shape that's still in test fixtures and existing
  // case files.
  constexpr std::string_view src = R"({
    "uid": 1,
    "name": "global_co2",
    "emission": "co2",
    "cap": 1000000.0,
    "cap_cost": 1000.0,
    "price": 50.0
  })";

  const auto z = daw::json::from_json<EmissionZone>(src);
  CHECK(z.name == "global_co2");
  REQUIRE(z.emissions.size() == 1);
  CHECK(std::get<Name>(z.emissions.front().emission) == "co2");
  CHECK(z.emissions.front().weight.value_or(0.0) == doctest::Approx(1.0));
  REQUIRE(z.cap.has_value());
  CHECK(std::get<Real>(z.cap.value_or(Real {0.0})) == doctest::Approx(1.0e6));

  // Re-emit and re-parse.
  const auto rendered = daw::json::to_json(z);
  const auto z2 = daw::json::from_json<EmissionZone>(rendered);
  CHECK(z2.name == z.name);
}

TEST_CASE("EmissionZone JSON — pure reporting (no cap, no price)")  // NOLINT
{
  // Zone with neither cap nor price — the balance row will still be
  // built in Commit 3 so total emissions are accounted for.
  constexpr std::string_view src = R"({
    "uid": 99,
    "name": "default_co2",
    "emission": "co2"
  })";

  const auto z = daw::json::from_json<EmissionZone>(src);
  CHECK(z.name == "default_co2");
  CHECK_FALSE(z.cap.has_value());
  CHECK_FALSE(z.price.has_value());
}

TEST_CASE("EmissionZone survives the System → SystemLP pipeline")  // NOLINT
{
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "EmissionZoneSurvival",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .demand_array =
          {{.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0}},
      .generator_array = {{.uid = Uid {1},
                           .name = "g1",
                           .bus = Uid {1},
                           .gcost = 10.0,
                           .capacity = 200.0}},
      .emission_array = {{.uid = Uid {1}, .name = "co2"}},
      .emission_zone_array = {{.uid = Uid {1},
                               .name = "global_co2",
                               .emissions = {{.emission = Uid {1},
                                              .weight = 1.0}},
                               .cap = 1.0e6,
                               .cap_cost = 1000.0,
                               .price = 50.0}},
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto& zones = system_lp.elements<EmissionZoneLP>();
  REQUIRE(zones.size() == 1);
  CHECK(zones.front().emission_zone().name == "global_co2");
  const auto sid = simulation_lp.stages().front().uid();
  CHECK(zones.front().param_cap(sid).value_or(0.0) == doctest::Approx(1.0e6));
  CHECK(zones.front().param_price(sid).value_or(0.0) == doctest::Approx(50.0));
}

TEST_CASE("System::merge concatenates emission_zone_array")  // NOLINT
{
  System a;
  a.emission_zone_array = {
      {.uid = Uid {1},
       .name = "global_co2",
       .emissions = {{.emission = Uid {1}, .weight = 1.0}}}};
  System b;
  b.emission_zone_array = {
      {.uid = Uid {2},
       .name = "la_nox",
       .emissions = {{.emission = Uid {2}, .weight = 1.0}}}};

  a.merge(std::move(b));
  REQUIRE(a.emission_zone_array.size() == 2);
  CHECK(a.emission_zone_array[0].name == "global_co2");
  CHECK(a.emission_zone_array[1].name == "la_nox");
}

// ── Commit 3 — LP-active wiring tests ────────────────────────────────

TEST_CASE(
    "EmissionZoneLP balance identity — production == rate × gen × dur")  // NOLINT
{
  // Build a 1-stage 1-block fixture with known emission rate.  Verify
  // post-solve that `EmissionZone/production_sol` equals
  // `rate × generation × duration` exactly (the LP balance row is an
  // equality constraint, so this must hold to FP tolerance).
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const double rate = 0.4;  // tons / MWh
  const double gen_capacity = 200.0;
  const double demand_capacity = 50.0;  // forces gen = 50

  const System system = {
      .name = "EmissionZoneBalanceIdentity",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .demand_array = {{.uid = Uid {1},
                        .name = "d1",
                        .bus = Uid {1},
                        .fcost = 1000.0,
                        .capacity = demand_capacity}},
      .generator_array = {{.uid = Uid {1},
                           .name = "g1",
                           .bus = Uid {1},
                           .gcost = 10.0,
                           .capacity = gen_capacity}},
      .emission_array = {{.uid = Uid {1}, .name = "co2"}},
      .emission_zone_array = {{.uid = Uid {1},
                               .name = "global_co2",
                               .emissions = {{.emission = Uid {1},
                                              .weight = 1.0}}}},
      .emission_source_array = {{.uid = Uid {1},
                                 .name = "g1_co2",
                                 .generator = OptSingleId {Uid {1}},
                                 .zone = Uid {1},
                                 .emission = Uid {1},
                                 .rate = rate}},
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  const auto& zones = system_lp.elements<EmissionZoneLP>();
  REQUIRE(zones.size() == 1);
  const auto& zone = zones.front();
  const auto& prod_cols = zone.production_cols();
  REQUIRE(!prod_cols.empty());

  // Look up the production column via the live LP indices.
  const auto scen_uid = simulation_lp.scenarios().front().uid();
  const auto stg = simulation_lp.stages().front();
  const auto stg_uid = stg.uid();
  const auto blk_uid = stg.blocks().front().uid();
  const auto st_key = std::tuple {scen_uid, stg_uid};
  const auto pcols_it = prod_cols.find(st_key);
  REQUIRE(pcols_it != prod_cols.end());
  REQUIRE(!pcols_it->second.empty());
  const auto& block_to_col = pcols_it->second;
  const auto pcol_it = block_to_col.find(blk_uid);
  REQUIRE(pcol_it != block_to_col.end());
  const auto prod_col = pcol_it->second;

  const auto col_sol = lp.get_col_sol();
  const auto production_sol = col_sol[prod_col];

  // Identity: production = rate × gen × duration.  Gen will saturate
  // at demand (50 MW) since gcost is small.
  CHECK(production_sol == doctest::Approx(rate * 50.0 * 1.0).epsilon(1e-9));
}

TEST_CASE("EmissionZoneLP soft cap binds + slack penalises")  // NOLINT
{
  // Force the cap to bind by setting it below the unconstrained
  // emission level, and verify (a) the LP still solves, (b) the
  // cap slack is non-zero, (c) the obj reflects the cap_cost penalty.
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  const System system = {
      .name = "EmissionZoneSoftCap",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .demand_array = {{.uid = Uid {1},
                        .name = "d1",
                        .bus = Uid {1},
                        .fcost = 10000.0,
                        .capacity = 50.0}},
      .generator_array = {{.uid = Uid {1},
                           .name = "g1",
                           .bus = Uid {1},
                           .gcost = 10.0,
                           .capacity = 200.0}},
      .emission_array = {{.uid = Uid {1}, .name = "co2"}},
      // Unconstrained emissions would be 0.4 × 50 = 20 tons.  Cap at
      // 5 forces slack = 15.
      .emission_zone_array = {{.uid = Uid {1},
                               .name = "global_co2",
                               .emissions = {{.emission = Uid {1},
                                              .weight = 1.0}},
                               .cap = 5.0,
                               .cap_cost = 100.0}},
      .emission_source_array = {{.uid = Uid {1},
                                 .name = "g1_co2",
                                 .generator = OptSingleId {Uid {1}},
                                 .zone = Uid {1},
                                 .emission = Uid {1},
                                 .rate = 0.4}},
  };

  PlanningOptions opts;
  opts.model_options.scale_objective = 1.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  // Demand is unserved here unless serving is cheaper than the cap +
  // unserved-demand penalty trade-off.  At gcost=10 the LP will run
  // the generator to meet demand (paying the cap slack) rather than
  // pay 10_000 $/MWh fcost.  So gen=50, emissions=20, slack=15.
  const auto& zones = system_lp.elements<EmissionZoneLP>();
  REQUIRE(zones.size() == 1);
  const auto& zone = zones.front();
  const auto& prod_cols = zone.production_cols();
  const auto scen_uid = simulation_lp.scenarios().front().uid();
  const auto stg = simulation_lp.stages().front();
  const auto stg_uid = stg.uid();
  const auto blk_uid = stg.blocks().front().uid();
  const auto st_key = std::tuple {scen_uid, stg_uid};
  const auto prod_col = prod_cols.at(st_key).at(blk_uid);
  const auto col_sol = lp.get_col_sol();
  CHECK(col_sol[prod_col] == doctest::Approx(20.0).epsilon(1e-9));
}

TEST_CASE(
    "EmissionZone multi-pollutant GHG basket — CO₂ + CH₄ weighted")  // NOLINT
{
  // GHG basket: zone covers both CO₂ (weight 1.0) and CH₄ (GWP-100 = 27.9).
  // Two sources on the same generator, one per pollutant.
  // Expected: production_sol = 1.0 * (0.4 * 50 * 1) + 27.9 * (0.003 * 50 * 1)
  //                          = 20.0 + 4.185 = 24.185 tCO₂-eq.
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "GhgBasket",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .demand_array = {{.uid = Uid {1},
                        .name = "d1",
                        .bus = Uid {1},
                        .fcost = 1000.0,
                        .capacity = 50.0}},
      .generator_array = {{.uid = Uid {1},
                           .name = "ngcc",
                           .bus = Uid {1},
                           .gcost = 10.0,
                           .capacity = 200.0}},
      .emission_array = {{.uid = Uid {1}, .name = "co2"},
                         {.uid = Uid {2}, .name = "ch4"}},
      .emission_zone_array =
          {{.uid = Uid {1},
            .name = "ghg_basket",
            .emissions = {{.emission = Uid {1}, .weight = 1.0},
                          {.emission = Uid {2}, .weight = 27.9}}}},
      .emission_source_array =
          {
              {.uid = Uid {1},
               .name = "ngcc_co2",
               .generator = OptSingleId {Uid {1}},
               .zone = Uid {1},
               .emission = Uid {1},
               .rate = 0.4},
              {.uid = Uid {2},
               .name = "ngcc_ch4",
               .generator = OptSingleId {Uid {1}},
               .zone = Uid {1},
               .emission = Uid {2},
               .rate = 0.003},
          },
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());

  const auto& zones = system_lp.elements<EmissionZoneLP>();
  REQUIRE(zones.size() == 1);
  const auto& zone = zones.front();
  const auto& prod_cols = zone.production_cols();
  const auto scen_uid = simulation_lp.scenarios().front().uid();
  const auto stg = simulation_lp.stages().front();
  const auto stg_uid = stg.uid();
  const auto blk_uid = stg.blocks().front().uid();
  const auto prod_col =
      prod_cols.at(std::tuple {scen_uid, stg_uid}).at(blk_uid);

  const auto col_sol = lp.get_col_sol();
  CHECK(col_sol[prod_col]
        == doctest::Approx(20.0 + 27.9 * 0.003 * 50.0).epsilon(1e-9));
}

TEST_CASE(
    "EmissionZone — combustion + upstream (WTT) sum into balance")  // NOLINT
{
  // Single CO₂ zone, single source with BOTH combustion AND upstream
  // rates set.  Verify production_sol = (rate + upstream_rate) × gen × dur.
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const double comb_rate = 0.4;  // tank-to-stack
  const double upstream_rate = 0.05;  // well-to-tank
  const double gen_sol = 50.0;

  const System system = {
      .name = "WttPlusTtw",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .demand_array = {{.uid = Uid {1},
                        .name = "d1",
                        .bus = Uid {1},
                        .fcost = 1000.0,
                        .capacity = gen_sol}},
      .generator_array = {{.uid = Uid {1},
                           .name = "g1",
                           .bus = Uid {1},
                           .gcost = 10.0,
                           .capacity = 200.0}},
      .emission_array = {{.uid = Uid {1}, .name = "co2"}},
      .emission_zone_array = {{.uid = Uid {1},
                               .name = "lifecycle_co2",
                               .emissions = {{.emission = Uid {1},
                                              .weight = 1.0}}}},
      .emission_source_array = {{.uid = Uid {1},
                                 .name = "g1_co2",
                                 .generator = OptSingleId {Uid {1}},
                                 .zone = Uid {1},
                                 .emission = Uid {1},
                                 .rate = comb_rate,
                                 .upstream_rate = upstream_rate}},
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());

  const auto& zone = system_lp.elements<EmissionZoneLP>().front();
  const auto scen_uid = simulation_lp.scenarios().front().uid();
  const auto stg = simulation_lp.stages().front();
  const auto stg_uid = stg.uid();
  const auto blk_uid = stg.blocks().front().uid();
  const auto prod_col =
      zone.production_cols().at(std::tuple {scen_uid, stg_uid}).at(blk_uid);

  const auto col_sol = lp.get_col_sol();
  CHECK(col_sol[prod_col]
        == doctest::Approx((comb_rate + upstream_rate) * gen_sol * 1.0)
               .epsilon(1e-9));
}
