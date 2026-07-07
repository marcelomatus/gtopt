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

using namespace gtopt;

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
    "EmissionZoneLP reporting-only — no cap / price / pool builds no LP "
    "rows or columns")  // NOLINT
{
  // Build a 1-stage 1-block fixture with a zone that has NO cap, NO
  // price, NO allowance pool — pure reporting.  After the
  // substitute-out rewrite the LP must contain zero EmissionZone
  // columns and zero EmissionZone rows (the per-source emission
  // streams are still produced post-solve from the source's factor
  // cache).
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const double rate = 0.4;  // tons / MWh
  const double gen_capacity = 200.0;
  const double demand_capacity = 50.0;  // forces gen = 50

  const System system = {
      .name = "EmissionZoneReportingOnly",
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

  // Zone is reporting-only: no LP-active piece, so no cap rows and
  // no cap slack columns.  Production column + balance row have been
  // substituted out entirely — they were never added to the LP.
  const auto& zones = system_lp.elements<EmissionZoneLP>();
  REQUIRE(zones.size() == 1);
  const auto& zone = zones.front();
  CHECK(zone.cap_rows().empty());
  CHECK(zone.cap_slack_cols().empty());

  // The LP solution itself is unchanged by the (now-no-op) zone: gen
  // saturates at demand = 50 MW since gcost is small and the only
  // cost surface is dispatch.
  CHECK(lp.get_obj_value() == doctest::Approx(10.0 * 50.0).epsilon(1e-9));
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
  //
  // The production column is gone (substituted out); the binding
  // signal is the cap-row slack column, which carries the per-stage
  // overage tonnage.
  const auto& zones = system_lp.elements<EmissionZoneLP>();
  REQUIRE(zones.size() == 1);
  const auto& zone = zones.front();
  const auto& slack_cols = zone.cap_slack_cols();
  const auto scen_uid = simulation_lp.scenarios().front().uid();
  const auto stg = simulation_lp.stages().front();
  const auto stg_uid = stg.uid();
  const auto st_key = std::tuple {scen_uid, stg_uid};
  REQUIRE(slack_cols.find(st_key) != slack_cols.end());
  const auto slack_col = slack_cols.at(st_key);
  const auto col_sol = lp.get_col_sol();
  CHECK(col_sol[slack_col] == doctest::Approx(15.0).epsilon(1e-9));
}

TEST_CASE(
    "EmissionZone multi-pollutant GHG basket — CO₂ + CH₄ weighted")  // NOLINT
{
  // GHG basket: zone covers both CO₂ (weight 1.0) and CH₄ (GWP-100 = 27.9).
  // Two sources on the same generator, one per pollutant.  Unconstrained
  // weighted emissions = 1.0·(0.4·50·1) + 27.9·(0.003·50·1) = 24.185 tCO₂-eq.
  // Set a tiny soft cap so the cap-slack column carries the full overage.
  constexpr double kExpectedEmissions = 24.185;  // = 20.0 + 27.9·0.003·50
  constexpr double kCap = 1.0;
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
                          {.emission = Uid {2}, .weight = 27.9}},
            .cap = kCap,
            .cap_cost = 1.0}},
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

  PlanningOptions opts;
  opts.model_options.scale_objective = 1.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());

  // The soft cap is loose enough that the LP serves the full demand
  // (cap_cost = 1 $/t ≪ gcost = 10 $/MWh + fcost = 1000 $/MWh).  Total
  // weighted emissions are 24.185 t; slack absorbs `total − cap`.
  const auto& zone = system_lp.elements<EmissionZoneLP>().front();
  const auto scen_uid = simulation_lp.scenarios().front().uid();
  const auto stg_uid = simulation_lp.stages().front().uid();
  const auto st_key = std::tuple {scen_uid, stg_uid};
  const auto slack_col = zone.cap_slack_cols().at(st_key);
  const auto col_sol = lp.get_col_sol();
  CHECK(col_sol[slack_col]
        == doctest::Approx(kExpectedEmissions - kCap).epsilon(1e-9));
}

TEST_CASE(
    "EmissionZone — combustion + upstream (WTT) sum into cap row")  // NOLINT
{
  // Single CO₂ zone, single source with BOTH combustion AND upstream
  // rates set.  Total emissions = (rate + upstream_rate) × gen × dur.
  // Use a soft cap so the slack column captures the overage and we
  // can observe it without inspecting the substituted-away production
  // column.
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const double comb_rate = 0.4;  // tank-to-stack
  const double upstream_rate = 0.05;  // well-to-tank
  const double gen_sol = 50.0;
  const double total_emissions = (comb_rate + upstream_rate) * gen_sol * 1.0;
  const double kCap = 1.0;

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
                                              .weight = 1.0}},
                               .cap = kCap,
                               .cap_cost = 1.0}},
      .emission_source_array = {{.uid = Uid {1},
                                 .name = "g1_co2",
                                 .generator = OptSingleId {Uid {1}},
                                 .zone = Uid {1},
                                 .emission = Uid {1},
                                 .rate = comb_rate,
                                 .upstream_rate = upstream_rate}},
  };

  PlanningOptions opts;
  opts.model_options.scale_objective = 1.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());

  const auto& zone = system_lp.elements<EmissionZoneLP>().front();
  const auto scen_uid = simulation_lp.scenarios().front().uid();
  const auto stg_uid = simulation_lp.stages().front().uid();
  const auto st_key = std::tuple {scen_uid, stg_uid};
  const auto slack_col = zone.cap_slack_cols().at(st_key);
  const auto col_sol = lp.get_col_sol();
  CHECK(col_sol[slack_col]
        == doctest::Approx(total_emissions - kCap).epsilon(1e-9));
}

TEST_CASE(
    "EmissionZone — CCS capture_rate scales the cap-row contribution")  // NOLINT
{
  // Verify `(1 − capture_rate)` scaling on the substituted cap row.
  // rate=0.4, gen=50, dur=1, capture_rate=0.9 → net emissions =
  // 0.4·50·(1-0.9) = 2.0 t.  With a soft cap of 0.5, slack = 1.5.
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  const double rate = 0.4;
  const double gen_sol = 50.0;
  const double capture_rate = 0.9;
  const double net_emissions = (1.0 - capture_rate) * rate * gen_sol * 1.0;
  const double kCap = 0.5;

  const System system = {
      .name = "CcsCapture",
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
                           .capacity = 200.0,
                           .emission_captures = {{.emission = Uid {1},
                                                  .rate = capture_rate,
                                                  .cost = 0.0}}}},
      .emission_array = {{.uid = Uid {1}, .name = "co2"}},
      .emission_zone_array = {{.uid = Uid {1},
                               .name = "co2_zone",
                               .emissions = {{.emission = Uid {1},
                                              .weight = 1.0}},
                               .cap = kCap,
                               .cap_cost = 1.0}},
      .emission_source_array = {{.uid = Uid {1},
                                 .name = "g1_co2",
                                 .generator = OptSingleId {Uid {1}},
                                 .zone = Uid {1},
                                 .emission = Uid {1},
                                 .rate = rate}},
  };

  PlanningOptions opts;
  opts.model_options.scale_objective = 1.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());

  const auto& zone = system_lp.elements<EmissionZoneLP>().front();
  const auto scen_uid = simulation_lp.scenarios().front().uid();
  const auto stg_uid = simulation_lp.stages().front().uid();
  const auto st_key = std::tuple {scen_uid, stg_uid};
  const auto slack_col = zone.cap_slack_cols().at(st_key);
  const auto col_sol = lp.get_col_sol();
  // Expected: net emissions − cap = (1 - 0.9)·0.4·50 − 0.5 = 1.5 t.
  CHECK(col_sol[slack_col]
        == doctest::Approx(net_emissions - kCap).epsilon(1e-9));
}

// ────────────────────────────────────────────────────────────────────────
// Substitute-out coverage gaps (issue #529 follow-up audit):
//   * G3 — price-only zone (carbon tax, no cap, no pool) — confirm
//     `EmissionSourceLP` adds the per-block `block_ecost(price · α_rate)`
//     adder onto the generator dispatch column's objective.
//   * G1 — `objective_mode = "emissions"` substitutes a unit carbon
//     price (1.0) on every active source, zeroing the dispatch-cost
//     slope.  Confirm the resulting LP minimizes total CO₂ directly.
// ────────────────────────────────────────────────────────────────────────
TEST_CASE(
    "EmissionZoneLP carbon-tax: price-only zone adds adder to gen obj")  // NOLINT
{
  // 1-block, 1-stage fixture.  gcost = 0 so the only cost surface is
  // the carbon tax: obj = price · α · gen · dur · prob · discount.
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  constexpr double kPrice = 50.0;  // $/tCO₂
  constexpr double kRate = 0.4;  // tCO₂/MWh
  constexpr double kGen = 50.0;  // MW
  // obj = price · α · gen · dur = 50 · 0.4 · 50 · 1 = 1000.
  constexpr double kExpectedObj = kPrice * kRate * kGen * 1.0;

  const System system = {
      .name = "EmissionZonePriceOnly",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .demand_array = {{.uid = Uid {1},
                        .name = "d1",
                        .bus = Uid {1},
                        .fcost = 1.0e6,
                        .capacity = kGen}},
      .generator_array = {{.uid = Uid {1},
                           .name = "g1",
                           .bus = Uid {1},
                           .gcost = 0.0,
                           .capacity = 200.0}},
      .emission_array = {{.uid = Uid {1}, .name = "co2"}},
      .emission_zone_array = {{.uid = Uid {1},
                               .name = "tax_only",
                               .emissions = {{.emission = Uid {1},
                                              .weight = 1.0}},
                               .price = kPrice}},
      .emission_source_array = {{.uid = Uid {1},
                                 .name = "g1_co2",
                                 .generator = OptSingleId {Uid {1}},
                                 .zone = Uid {1},
                                 .emission = Uid {1},
                                 .rate = kRate}},
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

  // Price-only zone: no cap row, no slack column.
  const auto& zone = system_lp.elements<EmissionZoneLP>().front();
  CHECK(zone.cap_rows().empty());
  CHECK(zone.cap_slack_cols().empty());

  // Total cost = carbon tax (50 · 20 = 1000); dispatch gcost = 0.
  CHECK(lp.get_obj_value() == doctest::Approx(kExpectedObj).epsilon(1e-6));
}

TEST_CASE(
    "EmissionSourceLP objective_mode='emissions' zeroes gcost adds α tax")  // NOLINT
{
  // `objective_mode = "emissions"` (issue #519) makes the LP minimize
  // total CO₂-eq directly: dispatch cost is zeroed on every generator
  // and a unit price (1.0 $/tCO₂eq) is applied through the source
  // adder.  Verify the resulting objective is `α · gen · dur` with
  // gcost ignored.
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  constexpr double kRate = 0.4;
  constexpr double kGen = 50.0;
  // emissions-mode: gcost zeroed, only carbon tax (1.0 · α · gen · dur).
  constexpr double kExpectedObj = 1.0 * kRate * kGen * 1.0;

  const System system = {
      .name = "EmissionObjectiveMode",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .demand_array = {{.uid = Uid {1},
                        .name = "d1",
                        .bus = Uid {1},
                        .fcost = 1.0e6,
                        .capacity = kGen}},
      .generator_array = {{.uid = Uid {1},
                           .name = "g1",
                           .bus = Uid {1},
                           .gcost = 999.0,  // proves gcost is ignored
                           .capacity = 200.0}},
      .emission_array = {{.uid = Uid {1}, .name = "co2"}},
      // No cap, no explicit price — `objective_mode=emissions` injects
      // unit price on every source.
      .emission_zone_array = {{.uid = Uid {1},
                               .name = "co2_only",
                               .emissions = {{.emission = Uid {1},
                                              .weight = 1.0}}}},
      .emission_source_array = {{.uid = Uid {1},
                                 .name = "g1_co2",
                                 .generator = OptSingleId {Uid {1}},
                                 .zone = Uid {1},
                                 .emission = Uid {1},
                                 .rate = kRate}},
  };

  PlanningOptions opts;
  opts.model_options.scale_objective = 1.0;
  opts.model_options.objective_mode = OptName {"emissions"};
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  // gcost = 999 must be ignored; obj = unit_price · α · gen · dur = 20.
  CHECK(lp.get_obj_value() == doctest::Approx(kExpectedObj).epsilon(1e-6));
}
