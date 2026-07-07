// SPDX-License-Identifier: BSD-3-Clause
//
// Realistic-fixture integration test for the emissions framework.
//
// Builds a five-unit system (diesel peaker, natural-gas CCGT, coal PC,
// solar PV, wind) wired through:
//   - `Fuel.emission_factors[]` (multi-pollutant per-fuel table,
//     Commit 9)
//   - `Generator.fuel` + `Generator.heat_rate` (parameter-only fuel
//     coupling — combustion + upstream factors are folded into
//     synthesized `EmissionSource` rows by
//     `System::expand_fuel_emission_sources()`)
//   - `EmissionZone.cap` (hard global CO₂ cap)
//
// Plant parameters are picked to be in the literature-realistic range
// (IPCC AR6 / EIA EPM / EPA AP-42), not just toy values:
//
//   |        | price       | heat_rate | gcost  | combustion CO₂ | upstream CO₂
//   | |        | [$/GJ]      | [GJ/MWh]  | [$/MWh]| [tCO₂/GJ]      | [tCO₂/GJ]
//   | | diesel | 22.0        | 10.5      | 4.0    | 0.0741         | 0.0118 |
//   | gas    | 8.5         | 7.5       | 3.0    | 0.0561         | 0.0094 | |
//   coal   | 3.5         | 9.5       | 5.0    | 0.0946         | 0.0042       |
//   | solar  |   —         |   —       | 0      |   —            |   — | | wind
//   |   —         |   —       | 0      |   —            |   —          |
//
// Per-MWh derived values (`heat_rate × factor`) — what
// `expand_fuel_emission_sources()` should write to `EmissionSource.rate`:
//
//   |        | rate    | upstream_rate | fuel $/MWh | gcost $/MWh | SRMC $/MWh
//   | | diesel | 0.7781  | 0.1239        | 231.00     | 4.00        | 235.00 |
//   | gas    | 0.4208  | 0.0705        |  63.75     | 3.00        |  66.75 | |
//   coal   | 0.8987  | 0.0399        |  33.25     | 5.00        |  38.25     |
//
// Capacities: solar 30 MW + wind 40 MW + coal 50 MW + gas 50 MW +
// diesel 30 MW = 200 MW > demand 100 MW.  Renewables are
// must-take (no fuel, gcost 0), so without a CO₂ cap the merit order
// dispatches solar (30) + wind (40) + coal (30) = 100 MW.
//
// With a hard CO₂ cap binding *below* coal's emissions, the LP
// substitutes gas for some coal; this is the test that prove the
// fold + expand + multi-pollutant balance row do the right thing.

#include <algorithm>

#include <doctest/doctest.h>
#include <gtopt/emission.hpp>
#include <gtopt/emission_source.hpp>
#include <gtopt/emission_source_lp.hpp>
#include <gtopt/emission_zone.hpp>
#include <gtopt/fuel.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace
{

// Plant-parameter constants from the table in the file header.
struct ThermalParams
{
  Real price;
  Real heat_rate;
  Real gcost;
  Real combustion_co2;
  Real upstream_co2;
};

constexpr ThermalParams kDiesel = {
    .price = 22.0,
    .heat_rate = 10.5,
    .gcost = 4.0,
    .combustion_co2 = 0.0741,
    .upstream_co2 = 0.0118,
};
constexpr ThermalParams kGas = {
    .price = 8.5,
    .heat_rate = 7.5,
    .gcost = 3.0,
    .combustion_co2 = 0.0561,
    .upstream_co2 = 0.0094,
};
constexpr ThermalParams kCoal = {
    .price = 3.5,
    .heat_rate = 9.5,
    .gcost = 5.0,
    .combustion_co2 = 0.0946,
    .upstream_co2 = 0.0042,
};

// Capacities (MW).
constexpr Real kCapDiesel = 30.0;
constexpr Real kCapGas = 50.0;
constexpr Real kCapCoal = 50.0;
constexpr Real kCapSolar = 30.0;
constexpr Real kCapWind = 40.0;

// Demand (MW).
constexpr Real kDemand = 100.0;
// Block duration (h) — pin to 1 so MWh = MW × 1.
constexpr Real kDur = 1.0;

constexpr Real kDemandFailCost = 1000.0;

// Helper — SRMC (short-run marginal cost) = fuel $/MWh + gcost.
[[nodiscard]] constexpr Real srmc(const ThermalParams& p) noexcept
{
  return p.price * p.heat_rate + p.gcost;
}
// Helper — per-MWh combustion + upstream CO₂ rate.
[[nodiscard]] constexpr Real co2_rate(const ThermalParams& p) noexcept
{
  return p.heat_rate * (p.combustion_co2 + p.upstream_co2);
}

// Build the five-unit system with multi-pollutant fuel emission
// factors and a single global CO₂ zone.  The caller can override
// `cap` (`std::nullopt` for an uncapped run, or a numeric tons-per-stage
// value for a hard cap).
[[nodiscard]] System make_thermal_mix_system(std::optional<Real> co2_cap)
{
  System sys;
  sys.name = "ThermalMix";
  sys.bus_array = {{.uid = Uid {1}, .name = "bus"}};
  sys.demand_array = {
      {.uid = Uid {1}, .name = "demand", .bus = Uid {1}, .capacity = kDemand}};

  // Pollutant tag.
  sys.emission_array = {{.uid = Uid {1}, .name = "co2"}};

  // Global CO₂ zone — `weight = 1.0` (CO₂ is the basket reference).
  EmissionZone zone {
      .uid = Uid {1},
      .name = "global_co2",
      .emissions = {{.emission = Uid {1}, .weight = 1.0}},
  };
  if (co2_cap.has_value()) {
    zone.cap = OptTRealFieldSched {*co2_cap};
  }
  sys.emission_zone_array = {zone};

  // Three fuels.
  sys.fuel_array = {
      {.uid = Uid {1},
       .name = "diesel",
       .price = kDiesel.price,
       .emission_factors = {{.emission = SingleId {Uid {1}},
                             .combustion = kDiesel.combustion_co2,
                             .upstream = kDiesel.upstream_co2}}},
      {.uid = Uid {2},
       .name = "gas",
       .price = kGas.price,
       .emission_factors = {{.emission = SingleId {Uid {1}},
                             .combustion = kGas.combustion_co2,
                             .upstream = kGas.upstream_co2}}},
      {.uid = Uid {3},
       .name = "coal",
       .price = kCoal.price,
       .emission_factors = {{.emission = SingleId {Uid {1}},
                             .combustion = kCoal.combustion_co2,
                             .upstream = kCoal.upstream_co2}}},
  };

  // Five generators: 3 thermal + 2 renewable.
  sys.generator_array = {
      {.uid = Uid {1},
       .name = "diesel_peaker",
       .bus = Uid {1},
       .gcost = kDiesel.gcost,
       .fuel = SingleId {Uid {1}},
       .heat_rate = kDiesel.heat_rate,
       .capacity = kCapDiesel},
      {.uid = Uid {2},
       .name = "gas_ccgt",
       .bus = Uid {1},
       .gcost = kGas.gcost,
       .fuel = SingleId {Uid {2}},
       .heat_rate = kGas.heat_rate,
       .capacity = kCapGas},
      {.uid = Uid {3},
       .name = "coal_pc",
       .bus = Uid {1},
       .gcost = kCoal.gcost,
       .fuel = SingleId {Uid {3}},
       .heat_rate = kCoal.heat_rate,
       .capacity = kCapCoal},
      {.uid = Uid {4},
       .name = "solar_pv",
       .bus = Uid {1},
       .gcost = 0.0,
       .capacity = kCapSolar},
      {.uid = Uid {5},
       .name = "wind_farm",
       .bus = Uid {1},
       .gcost = 0.0,
       .capacity = kCapWind},
  };
  return sys;
}

[[nodiscard]] Simulation make_one_block_simulation()
{
  return {
      .block_array = {{.uid = Uid {1}, .duration = kDur}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}, .probability_factor = 1.0}},
  };
}

[[nodiscard]] PlanningOptions make_options()
{
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = kDemandFailCost;
  popts.model_options.scale_objective = 1.0;
  return popts;
}

// Find a generator's dispatch column for the (only) (s, t, b) cell.
// Returns `std::nullopt` when the generator can't be located, so the
// caller can REQUIRE on it.
[[nodiscard]] std::optional<Real> generation_of(SystemLP& system_lp,
                                                std::string_view gen_name)
{
  const auto& gens = system_lp.elements<GeneratorLP>();
  const auto it = std::ranges::find_if(
      gens, [&](const auto& g) { return g.generator().name == gen_name; });
  if (it == gens.end()) {
    return std::nullopt;
  }
  const auto& s = system_lp.scene().scenarios()[0];
  const auto& t = system_lp.phase().stages()[0];
  const auto& gen_cols = it->generation_cols_at(s, t);
  const auto buid = t.blocks()[0].uid();
  const auto col_it = gen_cols.find(buid);
  if (col_it == gen_cols.end()) {
    return std::nullopt;
  }
  return system_lp.linear_interface().get_col_sol()[col_it->second];
}

}  // namespace

// ── Fixture sanity — the expand pass writes the per-MWh rates ────────

TEST_CASE(
    "Thermal-mix fixture — expand_fuel_emission_sources synthesizes one "
    "EmissionSource per fueled generator with literature-realistic rates")  // NOLINT
{
  auto sys = make_thermal_mix_system(/*co2_cap=*/std::nullopt);
  sys.expand_fuel_emission_sources();

  // 3 fueled generators × 1 covering zone = 3 sources.  Renewables
  // have no `fuel` so they're skipped.
  REQUIRE(sys.emission_source_array.size() == 3);

  const auto find_by_gen = [&](Uid gen_uid) -> const EmissionSource&
  {
    const auto it = std::ranges::find_if(
        sys.emission_source_array,
        [&](const auto& es)
        {
          return es.generator.has_value()
              && std::get<Uid>(es.generator.value_or(SingleId {Uid {0}}))
              == gen_uid;
        });
    REQUIRE(it != sys.emission_source_array.end());
    return *it;
  };

  // Diesel — heat_rate × combustion = 10.5 × 0.0741 = 0.77805
  const auto& es_diesel = find_by_gen(Uid {1});
  REQUIRE(es_diesel.rate.has_value());
  CHECK(std::get<Real>(*es_diesel.rate)
        == doctest::Approx(kDiesel.heat_rate * kDiesel.combustion_co2));
  REQUIRE(es_diesel.upstream_rate.has_value());
  CHECK(std::get<Real>(*es_diesel.upstream_rate)
        == doctest::Approx(kDiesel.heat_rate * kDiesel.upstream_co2));

  // Gas
  const auto& es_gas = find_by_gen(Uid {2});
  REQUIRE(es_gas.rate.has_value());
  CHECK(std::get<Real>(*es_gas.rate)
        == doctest::Approx(kGas.heat_rate * kGas.combustion_co2));

  // Coal — heat_rate × combustion = 9.5 × 0.0946 = 0.8987
  const auto& es_coal = find_by_gen(Uid {3});
  REQUIRE(es_coal.rate.has_value());
  CHECK(std::get<Real>(*es_coal.rate)
        == doctest::Approx(kCoal.heat_rate * kCoal.combustion_co2));
  REQUIRE(es_coal.upstream_rate.has_value());
  CHECK(std::get<Real>(*es_coal.upstream_rate)
        == doctest::Approx(kCoal.heat_rate * kCoal.upstream_co2));
}

// ── Uncapped — merit order is solar + wind + coal ───────────────────

TEST_CASE(
    "Thermal-mix LP — no CO₂ cap: merit order picks solar + wind + coal")  // NOLINT
{
  auto sys = make_thermal_mix_system(/*co2_cap=*/std::nullopt);
  // Pre-run the fold + expand so the LP build sees synthesized sources.
  sys.fold_legacy_fuel_emission_factors();
  sys.expand_fuel_emission_sources();

  const auto simulation = make_one_block_simulation();
  PlanningOptions popts = make_options();
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Coal is the cheapest fueled unit (SRMC = $38.25/MWh) — the LP
  // should pick it after renewables are exhausted.
  CHECK(generation_of(system_lp, "solar_pv").value_or(-1.0)
        == doctest::Approx(kCapSolar));
  CHECK(generation_of(system_lp, "wind_farm").value_or(-1.0)
        == doctest::Approx(kCapWind));
  CHECK(generation_of(system_lp, "coal_pc").value_or(-1.0)
        == doctest::Approx(kDemand - kCapSolar - kCapWind));  // 30 MW
  CHECK(generation_of(system_lp, "gas_ccgt").value_or(-1.0)
        == doctest::Approx(0.0));
  CHECK(generation_of(system_lp, "diesel_peaker").value_or(-1.0)
        == doctest::Approx(0.0));

  // Objective sanity — only coal contributes (renewables free).
  const auto coal_gen = kDemand - kCapSolar - kCapWind;
  CHECK(lp.get_obj_value() == doctest::Approx(coal_gen * srmc(kCoal)));
}

// ── Capped — coal gives way to gas under a binding CO₂ cap ──────────

TEST_CASE(
    "Thermal-mix LP — binding CO₂ cap displaces coal in favour of gas")  // NOLINT
{
  // Without a cap the dispatch is renewables + 30 MW coal, producing
  //   30 × (0.8987 + 0.03990) = 30 × 0.9386 = 28.158 tCO₂.
  // Set the cap below this — say 20 tCO₂ — to force a coal/gas swap.
  //
  // Let c = coal MW, g = gas MW.  Energy balance: c + g = 30.
  // Emission cap:
  //   0.9386 · c + (0.4208 + 0.0705) · g  ≤  20
  //   0.9386 · c + 0.4913 · (30 − c)      ≤  20
  //   0.4473 · c                          ≤  5.2390
  //   c                                   ≤  11.7124  (binding)
  //   g                                   =  18.2876
  //
  // The LP minimises cost so the cap binds; coal is the dirty plant
  // and gas the cleaner substitute.
  constexpr Real kCap = 20.0;
  auto sys = make_thermal_mix_system(kCap);
  sys.fold_legacy_fuel_emission_factors();
  sys.expand_fuel_emission_sources();

  const auto simulation = make_one_block_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Renewables still take their full output.
  CHECK(generation_of(system_lp, "solar_pv").value_or(-1.0)
        == doctest::Approx(kCapSolar));
  CHECK(generation_of(system_lp, "wind_farm").value_or(-1.0)
        == doctest::Approx(kCapWind));

  // Coal and gas split the remaining 30 MW per the cap-binding algebra.
  // Use a wider epsilon — the algebra above is an exact rational but
  // the LP solver returns up to ~1e-9 precision.
  const Real co2_coal = co2_rate(kCoal);  // 0.93860
  const Real co2_gas = co2_rate(kGas);  // 0.49133 (0.42075 + 0.07050)
  const Real coal_expected = (kCap - co2_gas * (kDemand - kCapSolar - kCapWind))
      / (co2_coal - co2_gas);
  const Real gas_expected = (kDemand - kCapSolar - kCapWind) - coal_expected;

  CHECK(generation_of(system_lp, "coal_pc").value_or(-1.0)
        == doctest::Approx(coal_expected).epsilon(1e-5));
  CHECK(generation_of(system_lp, "gas_ccgt").value_or(-1.0)
        == doctest::Approx(gas_expected).epsilon(1e-5));
  CHECK(generation_of(system_lp, "diesel_peaker").value_or(-1.0)
        == doctest::Approx(0.0).epsilon(1e-5));

  // Spot-check: total CO₂ produced equals the cap (binding).
  const Real co2_total = coal_expected * co2_coal + gas_expected * co2_gas;
  CHECK(co2_total == doctest::Approx(kCap).epsilon(1e-5));
}

// ── Multi-pollutant — adds a NOx pollutant and a regional NOx zone ─

TEST_CASE(
    "Thermal-mix LP — multi-pollutant fuels (CO₂ + NOₓ) synthesize one "
    "EmissionSource per (fuel, zone) covering each pollutant")  // NOLINT
{
  // Three thermal plants emit NOₓ at typical per-GJ rates (IPCC AP-42
  // — high for coal, lower for gas, mid for diesel) in addition to
  // their CO₂.  A regional NOₓ zone is created alongside the global
  // CO₂ zone — the expand pass should emit one EmissionSource per
  // (fueled generator, covering zone) where the zone lists the
  // pollutant.  No NOₓ source is created for renewables (no fuel).
  constexpr Real nox_diesel = 0.0004;  // tNOx/GJ
  constexpr Real nox_gas = 0.00015;
  constexpr Real nox_coal = 0.00080;

  auto sys = make_thermal_mix_system(/*co2_cap=*/std::nullopt);
  // Append NOₓ to the emission registry + a regional zone.
  sys.emission_array.push_back({.uid = Uid {2}, .name = "nox"});
  sys.emission_zone_array.push_back(
      {.uid = Uid {2},
       .name = "regional_nox",
       .emissions = {{.emission = Uid {2}, .weight = 1.0}}});
  // Add the NOₓ row to each fuel's emission_factors[].
  sys.fuel_array[0].emission_factors.push_back(
      {.emission = SingleId {Uid {2}}, .combustion = nox_diesel});
  sys.fuel_array[1].emission_factors.push_back(
      {.emission = SingleId {Uid {2}}, .combustion = nox_gas});
  sys.fuel_array[2].emission_factors.push_back(
      {.emission = SingleId {Uid {2}}, .combustion = nox_coal});

  sys.expand_fuel_emission_sources();

  // Expect 3 CO₂ sources + 3 NOₓ sources = 6 total.
  REQUIRE(sys.emission_source_array.size() == 6);

  // The LP must still build + solve.
  const auto simulation = make_one_block_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Unchanged uncapped dispatch — multi-pollutant balance rows are
  // tracking-only (no cap, no price), so coal still wins on cost.
  CHECK(generation_of(system_lp, "coal_pc").value_or(-1.0)
        == doctest::Approx(kDemand - kCapSolar - kCapWind));
}

// ── Carbon price (no cap, just a tax) — expected merit-order shuffle

TEST_CASE(
    "Thermal-mix LP — CO₂ price (no cap) gas-vs-coal break-even check")  // NOLINT
{
  // A CO₂ tax adds `price · production_b` to the objective per block.
  // Under a tax `p`, coal's tax-inclusive SRMC is
  //   SRMC_coal_tax = 38.25 + p · 0.93860
  // and gas's is
  //   SRMC_gas_tax  = 66.75 + p · 0.49133.
  // They're equal when p = 28.50 / 0.44727 ≈ 63.72 $/tCO₂.
  // Above the break-even, gas is cheaper and the LP swaps to gas.
  //
  // Pick a price comfortably above the break-even — 100 $/tCO₂ ≈
  // the high end of EU ETS.
  constexpr Real kPrice = 100.0;

  auto sys = make_thermal_mix_system(/*co2_cap=*/std::nullopt);
  sys.emission_zone_array.front().price = OptTRealFieldSched {kPrice};
  sys.expand_fuel_emission_sources();

  const auto simulation = make_one_block_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Gas is now cheaper per MWh once the carbon tax is included; coal
  // is dispatched only if gas is exhausted (it isn't — demand only
  // needs 30 MW after renewables and gas has 50 MW of capacity).
  CHECK(generation_of(system_lp, "gas_ccgt").value_or(-1.0)
        == doctest::Approx(kDemand - kCapSolar - kCapWind));
  CHECK(generation_of(system_lp, "coal_pc").value_or(-1.0)
        == doctest::Approx(0.0));
}

// ── Legacy fields fold path — system written in old style still works

TEST_CASE(
    "Thermal-mix LP — legacy combustion_emission_factor / "
    "upstream_emission_factor still work after the auto-fold")  // NOLINT
{
  // Same system, but each fuel carries the *legacy* scalar
  // `combustion_emission_factor` + `upstream_emission_factor` fields
  // (CO₂-only) instead of the new `emission_factors[]` table.  After
  // `fold_legacy_fuel_emission_factors()` they should produce the
  // same dispatch as the new-style fixture.
  System sys;
  sys.name = "ThermalMixLegacy";
  sys.bus_array = {{.uid = Uid {1}, .name = "bus"}};
  sys.demand_array = {
      {.uid = Uid {1}, .name = "demand", .bus = Uid {1}, .capacity = kDemand}};
  sys.emission_array = {{.uid = Uid {1}, .name = "co2"}};
  sys.emission_zone_array = {
      {.uid = Uid {1},
       .name = "global_co2",
       .emissions = {{.emission = Uid {1}, .weight = 1.0}}}};
  sys.fuel_array = {
      {.uid = Uid {1},
       .name = "diesel",
       .price = kDiesel.price,
       .combustion_emission_factor = kDiesel.combustion_co2,
       .upstream_emission_factor = kDiesel.upstream_co2},
      {.uid = Uid {2},
       .name = "gas",
       .price = kGas.price,
       .combustion_emission_factor = kGas.combustion_co2,
       .upstream_emission_factor = kGas.upstream_co2},
      {.uid = Uid {3},
       .name = "coal",
       .price = kCoal.price,
       .combustion_emission_factor = kCoal.combustion_co2,
       .upstream_emission_factor = kCoal.upstream_co2},
  };
  sys.generator_array = {
      {.uid = Uid {1},
       .name = "diesel_peaker",
       .bus = Uid {1},
       .gcost = kDiesel.gcost,
       .fuel = SingleId {Uid {1}},
       .heat_rate = kDiesel.heat_rate,
       .capacity = kCapDiesel},
      {.uid = Uid {2},
       .name = "gas_ccgt",
       .bus = Uid {1},
       .gcost = kGas.gcost,
       .fuel = SingleId {Uid {2}},
       .heat_rate = kGas.heat_rate,
       .capacity = kCapGas},
      {.uid = Uid {3},
       .name = "coal_pc",
       .bus = Uid {1},
       .gcost = kCoal.gcost,
       .fuel = SingleId {Uid {3}},
       .heat_rate = kCoal.heat_rate,
       .capacity = kCapCoal},
      {.uid = Uid {4},
       .name = "solar_pv",
       .bus = Uid {1},
       .capacity = kCapSolar},
      {.uid = Uid {5},
       .name = "wind_farm",
       .bus = Uid {1},
       .capacity = kCapWind},
  };

  // The fold path is supposed to migrate legacy → emission_factors[]
  // BEFORE expand runs.  Production callers (`PlanningLP::
  // create_systems`) do that automatically; we replicate the order
  // here.
  sys.fold_legacy_fuel_emission_factors();
  sys.expand_fuel_emission_sources();

  // After the fold, every fuel should now carry exactly one CO₂ row
  // in its emission_factors[] table.
  for (const auto& f : sys.fuel_array) {
    REQUIRE(f.emission_factors.size() == 1);
    CHECK_FALSE(f.combustion_emission_factor.has_value());
    CHECK_FALSE(f.upstream_emission_factor.has_value());
  }
  // 3 fueled generators ⇒ 3 synthesized sources.
  CHECK(sys.emission_source_array.size() == 3);

  const auto simulation = make_one_block_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Same dispatch as the new-style uncapped fixture.
  CHECK(generation_of(system_lp, "coal_pc").value_or(-1.0)
        == doctest::Approx(kDemand - kCapSolar - kCapWind));
}

// ── Multi-block ramp — 4 blocks with varying demand profile ─────────

TEST_CASE(
    "Thermal-mix LP — multi-block demand profile with CO₂ cap "
    "per stage")  // NOLINT
{
  // Four blocks (off-peak / shoulder / peak / shoulder) with hourly
  // demand factors [0.6, 1.0, 1.5, 0.9] on a 100 MW base — total
  // 100 + 100 + 150 + 90 = 440 MWh over 4 blocks.  Renewables = 70 MW
  // available every block (must-take, but solar/wind capacities here
  // are constant — we treat them as round-the-clock available for
  // the test, which is the LP's behaviour without a profile).
  //
  // Without a cap the LP picks whichever thermal is cheapest after
  // renewables.  Coal again wins on cost, so for each block the
  // residual demand-after-renewables falls on coal.
  //
  // With a per-stage CO₂ cap of 50 tCO₂, total coal output is bounded
  // — coal CO₂ per MWh is 0.9386 ⇒ at most ~53.3 MWh coal across the
  // 4 blocks before binding.  Without renewables, 440 MWh of coal
  // would produce 413 tCO₂; renewables shave 4 × 70 = 280 MWh off
  // that, leaving 160 MWh of thermal.  All-coal would produce
  // 160 × 0.9386 = 150 tCO₂ — well above 50.  So the LP must
  // substitute gas.  Gas costs more per MWh ($66.75 vs $38.25), but
  // less per tCO₂.
  //
  // We don't pin per-block primal values here — the LP has multiple
  // optima for *how* it spreads coal/gas across blocks.  Pin the
  // aggregate emissions to the cap and the aggregate generation
  // structure.
  Simulation simulation;
  simulation.block_array = {
      {.uid = Uid {1}, .duration = 1.0},
      {.uid = Uid {2}, .duration = 1.0},
      {.uid = Uid {3}, .duration = 1.0},
      {.uid = Uid {4}, .duration = 1.0},
  };
  simulation.stage_array = {
      {.uid = Uid {1}, .first_block = 0, .count_block = 4}};
  simulation.scenario_array = {{.uid = Uid {0}, .probability_factor = 1.0}};

  constexpr Real kCap = 50.0;
  auto sys = make_thermal_mix_system(kCap);
  // Use Demand.lmax (per-stage × per-block) for the block-level load
  // profile.  Demand.capacity is per-stage 1D (installed capacity), so
  // the block shape goes on lmax.  One stage, four blocks.
  sys.demand_array.front().capacity = OptTRealFieldSched {150.0};
  sys.demand_array.front().lmax = OptTBRealFieldSched {
      std::vector<std::vector<Real>> {{60.0, 100.0, 150.0, 90.0}}};

  sys.fold_legacy_fuel_emission_factors();
  sys.expand_fuel_emission_sources();

  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Aggregate per-pollutant emissions over the 4 blocks (= 1 stage)
  // by summing coal + gas generation × their CO₂ rates.  We pin total
  // CO₂ to the cap rather than the per-block split (multiple optima).
  const auto sum_gen = [&](std::string_view gen_name) -> Real
  {
    const auto& gens = system_lp.elements<GeneratorLP>();
    const auto it = std::ranges::find_if(
        gens, [&](const auto& g) { return g.generator().name == gen_name; });
    REQUIRE(it != gens.end());
    Real total = 0.0;
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];
    const auto& gen_cols = it->generation_cols_at(s, t);
    const auto& col_sol = system_lp.linear_interface().get_col_sol();
    for (const auto& block : t.blocks()) {
      const auto col_it = gen_cols.find(block.uid());
      REQUIRE(col_it != gen_cols.end());
      total += col_sol[col_it->second] * block.duration();
    }
    return total;
  };

  const auto coal_mwh = sum_gen("coal_pc");
  const auto gas_mwh = sum_gen("gas_ccgt");
  const Real total_co2 = coal_mwh * co2_rate(kCoal) + gas_mwh * co2_rate(kGas);
  CHECK(total_co2 == doctest::Approx(kCap).epsilon(1e-4));
  // Diesel is too expensive — should stay off.
  CHECK(sum_gen("diesel_peaker") == doctest::Approx(0.0).epsilon(1e-5));
}
