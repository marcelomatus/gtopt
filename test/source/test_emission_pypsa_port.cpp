// SPDX-License-Identifier: BSD-3-Clause
//
// Integration test — port of PyPSA's CO2 emission-constraint example
// (the coal + gas slice from the "Myopic Pathway Planning" notebook
// reduced to a single dispatch period, no expansion).
//
//   Source: https://github.com/PyPSA/PyPSA/blob/master/docs/examples/
//           myopic-pathway.ipynb
//   (cells defining the `coal`/`gas` Carriers + their generators)
//
// The notebook builds a multi-period capacity-expansion model with
// `n.add("Carrier", ["coal", "gas"], co2_emissions=[0.336, 0.198])`
// and two corresponding `Generator` rows that share the same canonical
// dispatch parameters used across the PyPSA tutorial corpus.  Stripped
// to one period and one load step, the LP is a closed-form 2-generator
// economic-dispatch with a CO₂ cap — exactly the kind of micro-fixture
// gtopt's emissions framework should reproduce bit-for-bit.
//
// ## PyPSA → gtopt mapping
//
// PyPSA stores per-fuel emission intensity on the `Carrier` in
// `[tCO₂/MWh_thermal]` and per-unit efficiency on the `Generator` in
// `[MWh_el/MWh_thermal]`.  The LP coefficient on the emission cap row
// is `co2_emissions / efficiency` `[tCO₂/MWh_el]`.
//
// gtopt's emissions framework expresses the same quantity through
// `Fuel.emission_factors[].combustion · Generator.heat_rate`.  With
// `heat_rate = 1 / efficiency` `[MWh_thermal/MWh_el]` and `combustion`
// in `[tCO₂/MWh_thermal]`, the synthesized `EmissionSource.rate` is
// `combustion / efficiency` `[tCO₂/MWh_el]` — identical to PyPSA's
// LP coefficient.  PyPSA's `marginal_cost` (already in `$/MWh_el`) is
// mapped to `Generator.gcost`; `Fuel.price` stays at zero because
// PyPSA folds the fuel $ into `marginal_cost`.
//
// ## Numerical inputs (verbatim from the PyPSA notebook)
//
// | Carrier | co2_emissions [tCO₂/MWh_th] | efficiency | marginal_cost
// [€/MWh_el] | p_nom [MW] |
// |---------|-----------------------------|------------|--------------------------|------------|
// | coal    | 0.336                       | 0.35       | 30 | 4 000      | |
// gas     | 0.198                       | 0.50       | 60 | 6 000      |
//
// Derived per-MWh_el quantities (what `expand_fuel_emission_sources`
// writes to `EmissionSource.rate`):
//
// | Carrier | heat_rate = 1/η [MWh_th/MWh_el] | rate = co2/η [tCO₂/MWh_el] |
// |---------|---------------------------------|----------------------------|
// | coal    | 2.857142857…                    | 0.960                      |
// | gas     | 2.000                           | 0.396                      |
//
// Demand: 5 000 MW single-block (within coal + gas capacity = 10 GW).
//
// ## Closed-form solutions
//
// ### Uncapped
// Coal is cheaper (€30 < €60), so dispatch the cheapest first:
//   coal = min(4000, 5000) = 4000 MW (binds capacity)
//   gas  = 5000 − 4000     = 1000 MW
// Objective = 4000·30 + 1000·60 = 180 000 €.
// CO₂      = 4000·0.96 + 1000·0.396 = 4236 tCO₂.
//
// ### Hard cap of 3000 tCO₂ (binds — below the uncapped 4236)
// Energy balance: c + g = 5000.
// Cap (binding): 0.96·c + 0.396·g = 3000.
// Substituting g = 5000 − c:
//   0.564·c = 3000 − 5000·0.396 = 1020
//   c = 1020 / 0.564 = 1808.510638297…
//   g = 5000 − c     = 3191.489361702…
// Objective = c·30 + g·60 = 245 744.680851063… €.
//
// Both numbers are checked below.

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

// Wrap the file body in a uniquely-named namespace so the inner
// anonymous namespace doesn't collide with the other emission test
// files when CMake batches them into a unity TU.
namespace test_emission_pypsa_port
{
namespace
{

// PyPSA carrier-side constants (verbatim from `myopic-pathway.ipynb`).
struct PypsaCarrier
{
  Real co2_emissions_per_thermal;  // [tCO₂ / MWh_th]
  Real efficiency;  // [MWh_el / MWh_th]
  Real marginal_cost;  // [€ / MWh_el]
  Real p_nom;  // [MW]
};

constexpr PypsaCarrier kCoal = {
    .co2_emissions_per_thermal = 0.336,
    .efficiency = 0.35,
    .marginal_cost = 30.0,
    .p_nom = 4000.0,
};
constexpr PypsaCarrier kGas = {
    .co2_emissions_per_thermal = 0.198,
    .efficiency = 0.50,
    .marginal_cost = 60.0,
    .p_nom = 6000.0,
};

// PyPSA Load.p_set [MW] (single time step).
constexpr Real kLoadMW = 5000.0;

// Block duration (h) — pin to 1 so MWh = MW × 1, matching PyPSA's
// per-snapshot dispatch convention.
constexpr Real kDur = 1.0;

constexpr Real kDemandFailCost = 1e6;  // far above any plant's SRMC

// gtopt heat_rate = 1/efficiency, in [MWh_th / MWh_el].  Units cancel
// with `Fuel.emission_factors[].combustion` `[tCO₂ / MWh_th]` to give
// `EmissionSource.rate` `[tCO₂ / MWh_el]` — i.e. the same coefficient
// PyPSA puts directly on the emission-cap row.
[[nodiscard]] constexpr Real heat_rate_of(const PypsaCarrier& c) noexcept
{
  return 1.0 / c.efficiency;
}
[[nodiscard]] constexpr Real co2_per_el(const PypsaCarrier& c) noexcept
{
  return c.co2_emissions_per_thermal / c.efficiency;
}

// Build the two-generator system with the PyPSA carrier semantics.
// When `co2_cap` is set, a hard cap is wired on the single global
// `EmissionZone`; otherwise the zone is reporting-only.
[[nodiscard]] System make_pypsa_port_system(std::optional<Real> co2_cap)
{
  System sys;
  sys.name = "PyPSAPortCoalGas";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "electricity",
      },
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "load",
          .bus = Uid {1},
          .capacity = kLoadMW,
      },
  };

  // Pollutant registry (PyPSA's `carrier_attribute = "co2_emissions"`).
  sys.emission_array = {
      {
          .uid = Uid {1},
          .name = "co2",
      },
  };

  // Single global CO₂ zone (PyPSA `GlobalConstraint`,
  // `type = "primary_energy"`, `carrier_attribute = "co2_emissions"`).
  EmissionZone zone {
      .uid = Uid {1},
      .name = "global_co2",
      .emissions =
          {
              {
                  .emission = Uid {1},
                  .weight = 1.0,
              },
          },
  };
  if (co2_cap.has_value()) {
    zone.cap = OptTRealFieldSched {*co2_cap};
  }
  sys.emission_zone_array = {zone};

  // Two fuels — `price = 0` because PyPSA folds the fuel cost into
  // `marginal_cost`.  Per-fuel combustion factors are the PyPSA
  // `Carrier.co2_emissions` values verbatim.
  sys.fuel_array = {
      {
          .uid = Uid {1},
          .name = "coal",
          .price = 0.0,
          .emission_factors =
              {
                  {
                      .emission = SingleId {Uid {1}},
                      .combustion = kCoal.co2_emissions_per_thermal,
                  },
              },
      },
      {
          .uid = Uid {2},
          .name = "gas",
          .price = 0.0,
          .emission_factors =
              {
                  {
                      .emission = SingleId {Uid {1}},
                      .combustion = kGas.co2_emissions_per_thermal,
                  },
              },
      },
  };

  // Two thermal generators.  `gcost = marginal_cost` (PyPSA already
  // folds fuel $ into it); `heat_rate = 1/η` so the per-MWh_el CO₂
  // coefficient matches PyPSA's `co2_emissions / efficiency`.
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "coal",
          .bus = Uid {1},
          .gcost = kCoal.marginal_cost,
          .fuel = SingleId {Uid {1}},
          .heat_rate = heat_rate_of(kCoal),
          .capacity = kCoal.p_nom,
      },
      {
          .uid = Uid {2},
          .name = "gas",
          .bus = Uid {1},
          .gcost = kGas.marginal_cost,
          .fuel = SingleId {Uid {2}},
          .heat_rate = heat_rate_of(kGas),
          .capacity = kGas.p_nom,
      },
  };
  return sys;
}

[[nodiscard]] Simulation make_one_block_simulation()
{
  return {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = kDur,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
                  .probability_factor = 1.0,
              },
          },
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

// ── Uncapped baseline — merit order dispatches coal then gas ─────────

TEST_CASE(
    "PyPSA-port LP — uncapped: coal binds capacity, gas covers the rest")  // NOLINT
{
  // Closed-form expected dispatch (see file header):
  //   coal = 4 000 MW (= p_nom)
  //   gas  = 1 000 MW
  //   obj  = 4000·30 + 1000·60 = 180 000 €
  constexpr Real kCoalExpected = 4000.0;
  constexpr Real kGasExpected = kLoadMW - kCoalExpected;
  constexpr Real kObjExpected =
      kCoalExpected * kCoal.marginal_cost + kGasExpected * kGas.marginal_cost;

  auto sys = make_pypsa_port_system(/*co2_cap=*/std::nullopt);
  // `PlanningLP::create_systems` calls fold-then-expand; replicate
  // that order in unit tests.
  sys.fold_legacy_fuel_emission_factors();
  sys.expand_fuel_emission_sources();

  const auto simulation = make_one_block_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  CHECK(generation_of(system_lp, "coal").value_or(-1.0)
        == doctest::Approx(kCoalExpected).epsilon(1e-5));
  CHECK(generation_of(system_lp, "gas").value_or(-1.0)
        == doctest::Approx(kGasExpected).epsilon(1e-5));
  CHECK(lp.get_obj_value() == doctest::Approx(kObjExpected).epsilon(1e-5));
}

// ── Binding CO₂ cap — coal/gas swap per closed-form algebra ──────────

TEST_CASE(
    "PyPSA-port LP — hard CO₂ cap at 3 000 tCO₂ forces coal/gas swap")  // NOLINT
{
  // Closed-form (file header derivation):
  //   c = 1020 / 0.564 = 1808.510638…  MW coal
  //   g = 5000 − c     = 3191.489361…  MW gas
  //   obj = c·30 + g·60 = 245 744.680851…  €
  //   CO₂ = c·0.96 + g·0.396 = 3000  (cap exactly binds)
  constexpr Real kCap = 3000.0;

  const Real coal_co2 = co2_per_el(kCoal);  // 0.96
  const Real gas_co2 = co2_per_el(kGas);  // 0.396
  const Real coal_expected = (kCap - gas_co2 * kLoadMW) / (coal_co2 - gas_co2);
  const Real gas_expected = kLoadMW - coal_expected;
  const Real obj_expected =
      coal_expected * kCoal.marginal_cost + gas_expected * kGas.marginal_cost;

  auto sys = make_pypsa_port_system(kCap);
  sys.fold_legacy_fuel_emission_factors();
  sys.expand_fuel_emission_sources();

  const auto simulation = make_one_block_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  const auto coal_dispatch = generation_of(system_lp, "coal").value_or(-1.0);
  const auto gas_dispatch = generation_of(system_lp, "gas").value_or(-1.0);
  CHECK(coal_dispatch == doctest::Approx(coal_expected).epsilon(1e-5));
  CHECK(gas_dispatch == doctest::Approx(gas_expected).epsilon(1e-5));
  CHECK(lp.get_obj_value() == doctest::Approx(obj_expected).epsilon(1e-5));

  // CO₂ produced must equal the cap (it binds by construction).
  const Real co2_total = coal_dispatch * coal_co2 + gas_dispatch * gas_co2;
  CHECK(co2_total == doctest::Approx(kCap).epsilon(1e-5));
}

// ── Carbon tax (no cap) — PyPSA equivalent: GlobalConstraint with
//    `type = "primary_energy"` replaced by a per-ton carbon price ─────

TEST_CASE(
    "PyPSA-port LP — €100/tCO₂ carbon tax flips merit order to gas")  // NOLINT
{
  // With a CO₂ price p, the tax-inclusive SRMC is:
  //   SRMC_coal = 30 + p · 0.96
  //   SRMC_gas  = 60 + p · 0.396
  // They cross when p · (0.96 − 0.396) = 30, i.e. p ≈ 53.19 €/tCO₂.
  // At €100/tCO₂ gas is cheaper, so the LP dispatches gas first.
  //   gas  = 5000 MW (well under p_nom = 6000 MW)
  //   coal = 0 MW
  //   obj  = 5000·(60 + 100·0.396) = 5000·99.60 = 498 000 €
  constexpr Real kPrice = 100.0;
  constexpr Real kGasExpected = kLoadMW;
  constexpr Real kCoalExpected = 0.0;

  auto sys = make_pypsa_port_system(/*co2_cap=*/std::nullopt);
  sys.emission_zone_array.front().price = OptTRealFieldSched {kPrice};
  sys.fold_legacy_fuel_emission_factors();
  sys.expand_fuel_emission_sources();

  const auto simulation = make_one_block_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  CHECK(generation_of(system_lp, "coal").value_or(-1.0)
        == doctest::Approx(kCoalExpected).epsilon(1e-5));
  CHECK(generation_of(system_lp, "gas").value_or(-1.0)
        == doctest::Approx(kGasExpected).epsilon(1e-5));

  // Objective = dispatch cost + tax revenue (the carbon price enters
  // the objective as `price · production_b` per block, so the LP's
  // reported objective includes it).
  const Real obj_expected = kGasExpected * kGas.marginal_cost
      + kGasExpected * co2_per_el(kGas) * kPrice;
  CHECK(lp.get_obj_value() == doctest::Approx(obj_expected).epsilon(1e-5));
}

}  // namespace test_emission_pypsa_port
