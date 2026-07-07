// SPDX-License-Identifier: BSD-3-Clause
//
// Port of the MATPOWER `case5` (PJM 5-bus) benchmark into a gtopt
// emission-framework integration test.
//
// ## Source
//
// MATPOWER `case5.m` is a canonical 5-bus OPF benchmark first
// published by Frank Li (Cornell) and shipped with MATPOWER since
// v5.0.  Network + cost data fetched from:
//
//   https://raw.githubusercontent.com/MATPOWER/matpower/master/data/case5.m
//
// The original `case5` is a pure OPF benchmark and carries NO emission
// data.  We layer an emission accounting on top by:
//
//   1. Keeping the case's economic-dispatch cost coefficients exactly
//      (`gencost.c1` → `Generator.gcost`, `fuel.price = 0`), so the
//      merit order matches the published case.
//   2. Assigning each generator a fuel type based on its cost rank,
//      following the PJM-fleet convention (cheapest = nuclear /
//      baseload, mid-tier = coal, then gas CCGT, peakers = diesel).
//   3. Pairing each fuel with IPCC AR6 combustion emission factors
//      [kg CO₂ / GJ] and EIA-923 average heat rates [GJ / MWh].
//
// ## case5 cost-rank → fuel-type assignment
//
// | Gen | bus | Pmax | c1   | Assignment        | Heat-rate | tCO₂/GJ |
// |-----|-----|------|------|-------------------|-----------|---------|
// | G1  | 1   | 40   | 14   | coal (subcrit)    | 10.5      | 0.0946  |
// | G2  | 1   | 170  | 15   | coal (subcrit)    | 10.5      | 0.0946  |
// | G3  | 3   | 520  | 30   | gas CCGT          |  7.5      | 0.0561  |
// | G4  | 4   | 200  | 40   | diesel peaker     | 10.0      | 0.0741  |
// | G5  | 5   | 600  | 10   | nuclear (no fuel) |   —       |   —     |
//
// Rationale: in the PJM dispatch stack the cheapest baseload is
// nuclear (low SRMC, must-run), and the highest SRMC is the oil/
// diesel peaker.  Coal sits in the middle on cost but tops the
// emission stack, which makes for a clean cap-binding test.
//
// ## IPCC AR6 default emission factors (combustion, kg CO₂ / GJ)
//
// | Fuel    | Factor | Source                                |
// |---------|--------|---------------------------------------|
// | Coal    | 94.6   | IPCC AR6 WG3 Table A.III.2 (bituminous) |
// | Gas     | 56.1   | IPCC AR6 WG3 Table A.III.2 (natural gas) |
// | Diesel  | 74.1   | IPCC AR6 WG3 Table A.III.2 (gas/diesel oil) |
//
// (Values stored as tons / GJ — i.e. kg / GJ × 1e-3.)
//
// ## Heat rates (EIA-923 fleet averages, GJ / MWh)
//
//   Coal subcritical : 10.5 GJ/MWh  (≈ 10 000 Btu/kWh)
//   Gas CCGT         :  7.5 GJ/MWh  (≈  7 100 Btu/kWh)
//   Diesel peaker    : 10.0 GJ/MWh  (≈  9 500 Btu/kWh)
//
// ## Per-MWh CO₂ rates (heat_rate × combustion)
//
//   Coal   : 10.5 × 0.0946 = 0.9933  tCO₂/MWh
//   Gas    :  7.5 × 0.0561 = 0.42075 tCO₂/MWh
//   Diesel : 10.0 × 0.0741 = 0.7410  tCO₂/MWh
//
// ## Network simplification
//
// case5 has 6 branches forming a small meshed network with two
// binding line ratings.  Modelling them requires Kirchhoff DC OPF
// plus per-branch reactances; that is overkill for an emission-
// framework test (which exercises the fuel × heat_rate × combustion
// pipeline, not OPF).  We therefore use `use_single_bus = true` and
// aggregate all demand (300 + 300 + 400 = 1000 MW) onto one
// bus / one demand row, leaving the cost-rank merit order as the
// only dispatch driver.
//
// ## Expected dispatch
//
// Total demand 1000 MW, total capacity 1530 MW.  Merit order
// (cheapest first): G5 nuclear (600 MW, $10) → G1 coal (40 MW, $14)
// → G2 coal (170 MW, $15) → G3 gas (190 of 520 MW, $30).  G4 diesel
// stays off (peaker, $40).  Objective:
//
//   600·10 + 40·14 + 170·15 + 190·30 = 14 810 $/h.
//
// Uncapped CO₂:
//   (40 + 170)·0.9933 + 190·0.42075 = 208.59 + 79.94 = 288.53 tCO₂.
//
// With a binding cap = 200 tCO₂, swap the more-expensive coal (G2,
// $15) for gas (G3, $30) — saves 0.57255 tCO₂ per MWh swapped.
// Required reduction Δ = 88.53 tCO₂ ⇒ swap x = Δ / 0.57255 = 154.63
// MW.  Final dispatch under the cap:
//
//   G5 = 600, G1 = 40, G2 = 15.37, G3 = 344.63, G4 = 0.

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
namespace test_emission_pjm5_port
{
namespace
{

// ── case5 cost coefficients ($/MWh) — verbatim from mpc.gencost ────────
constexpr Real kCostG1 = 14.0;  // Alta    — coal
constexpr Real kCostG2 = 15.0;  // Park C. — coal
constexpr Real kCostG3 = 30.0;  // Solitude— gas
constexpr Real kCostG4 = 40.0;  // Sundance— diesel
constexpr Real kCostG5 = 10.0;  // Brighton— nuclear

// ── case5 capacities (MW) — verbatim from mpc.gen Pmax ─────────────────
constexpr Real kCapG1 = 40.0;
constexpr Real kCapG2 = 170.0;
constexpr Real kCapG3 = 520.0;
constexpr Real kCapG4 = 200.0;
constexpr Real kCapG5 = 600.0;

// ── case5 demand (MW) — Pd summed across buses 2, 3, 4 ─────────────────
constexpr Real kDemand = 300.0 + 300.0 + 400.0;  // 1000 MW

// ── IPCC AR6 combustion factors (tCO₂ / GJ) ────────────────────────────
constexpr Real kEfCoal = 0.0946;
constexpr Real kEfGas = 0.0561;
constexpr Real kEfDiesel = 0.0741;

// ── EIA-923 average heat rates (GJ / MWh) ──────────────────────────────
constexpr Real kHrCoal = 10.5;
constexpr Real kHrGas = 7.5;
constexpr Real kHrDiesel = 10.0;

// ── Per-MWh combustion CO₂ rates ───────────────────────────────────────
constexpr Real kRateCoal = kHrCoal * kEfCoal;  // 0.9933  tCO₂/MWh
constexpr Real kRateGas = kHrGas * kEfGas;  //  0.42075 tCO₂/MWh
constexpr Real kRateDiesel = kHrDiesel * kEfDiesel;  // 0.7410  tCO₂/MWh

// ── Single-block stage ─────────────────────────────────────────────────
constexpr Real kBlockDur = 1.0;
constexpr Real kDemandFailCost = 1000.0;

// Fuel UIDs.
constexpr Uid kFuelCoalUid {1};
constexpr Uid kFuelGasUid {2};
constexpr Uid kFuelDieselUid {3};
constexpr Uid kEmissionCO2Uid {1};
constexpr Uid kZoneCO2Uid {1};
constexpr Uid kBusUid {1};

// Generator UIDs match case5 mpc.gen row order.
constexpr Uid kGenG1Uid {1};
constexpr Uid kGenG2Uid {2};
constexpr Uid kGenG3Uid {3};
constexpr Uid kGenG4Uid {4};
constexpr Uid kGenG5Uid {5};

// Build the PJM-5-port system in single-bus mode with an optional
// global CO₂ zone cap.
[[nodiscard]] System make_pjm5_system(std::optional<Real> co2_cap)
{
  System sys;
  sys.name = "PJM5Port";
  sys.bus_array = {
      {
          .uid = kBusUid,
          .name = "bus",
      },
  };
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "agg_demand",
          .bus = kBusUid,
          .capacity = kDemand,
      },
  };

  // Pollutant registry (CO₂ only).
  sys.emission_array = {
      {
          .uid = kEmissionCO2Uid,
          .name = "co2",
      },
  };

  // One global zone for CO₂.
  EmissionZone zone {
      .uid = kZoneCO2Uid,
      .name = "global_co2",
      .emissions =
          {
              {
                  .emission = kEmissionCO2Uid,
                  .weight = 1.0,
              },
          },
  };
  if (co2_cap.has_value()) {
    zone.cap = OptTRealFieldSched {*co2_cap};
  }
  sys.emission_zone_array = {
      zone,
  };

  // Three fuels.  `price = 0` because the per-MWh dispatch cost
  // already lives on `Generator.gcost` (preserved from `mpc.gencost`).
  sys.fuel_array = {
      {
          .uid = kFuelCoalUid,
          .name = "coal",
          .price = 0.0,
          .emission_factors =
              {
                  {
                      .emission = SingleId {kEmissionCO2Uid},
                      .combustion = kEfCoal,
                  },
              },
      },
      {
          .uid = kFuelGasUid,
          .name = "gas",
          .price = 0.0,
          .emission_factors =
              {
                  {
                      .emission = SingleId {kEmissionCO2Uid},
                      .combustion = kEfGas,
                  },
              },
      },
      {
          .uid = kFuelDieselUid,
          .name = "diesel",
          .price = 0.0,
          .emission_factors =
              {
                  {
                      .emission = SingleId {kEmissionCO2Uid},
                      .combustion = kEfDiesel,
                  },
              },
      },
  };

  // Five generators: order + uids match case5 mpc.gen.
  sys.generator_array = {
      {
          .uid = kGenG1Uid,
          .name = "Alta",
          .bus = kBusUid,
          .gcost = kCostG1,
          .fuel = SingleId {kFuelCoalUid},
          .heat_rate = kHrCoal,
          .capacity = kCapG1,
      },
      {
          .uid = kGenG2Uid,
          .name = "ParkCity",
          .bus = kBusUid,
          .gcost = kCostG2,
          .fuel = SingleId {kFuelCoalUid},
          .heat_rate = kHrCoal,
          .capacity = kCapG2,
      },
      {
          .uid = kGenG3Uid,
          .name = "Solitude",
          .bus = kBusUid,
          .gcost = kCostG3,
          .fuel = SingleId {kFuelGasUid},
          .heat_rate = kHrGas,
          .capacity = kCapG3,
      },
      {
          .uid = kGenG4Uid,
          .name = "Sundance",
          .bus = kBusUid,
          .gcost = kCostG4,
          .fuel = SingleId {kFuelDieselUid},
          .heat_rate = kHrDiesel,
          .capacity = kCapG4,
      },
      {
          .uid = kGenG5Uid,
          .name = "Brighton",
          .bus = kBusUid,
          .gcost = kCostG5,
          .capacity = kCapG5,
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
                  .duration = kBlockDur,
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
  popts.model_options.use_single_bus = true;
  return popts;
}

// Helper: locate a generator's dispatch column for the only (s, t, b)
// cell and return its primal value.  Returns `std::nullopt` when the
// generator can't be found so the caller can `value_or(-1.0)` against
// a sentinel inside an `Approx` check.
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

// ── Uncapped — pure economic dispatch from mpc.gencost ─────────────────

TEST_CASE(
    "PJM-5 port — uncapped dispatch matches the MATPOWER case5 cost-rank "
    "merit order")  // NOLINT
{
  auto sys = make_pjm5_system(/*co2_cap=*/std::nullopt);
  sys.fold_legacy_fuel_emission_factors();
  sys.expand_fuel_emission_sources();

  // Sanity — 4 fueled generators (G5 nuclear has no fuel) ⇒ 4 sources.
  REQUIRE(sys.emission_source_array.size() == 4);

  const auto simulation = make_one_block_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Merit order: G5 (600, $10) → G1 (40, $14) → G2 (170, $15) →
  // G3 (190 of 520, $30).  G4 diesel stays off.
  constexpr Real kG3Disp = kDemand - kCapG5 - kCapG1 - kCapG2;  // 190 MW
  CHECK(generation_of(system_lp, "Brighton").value_or(-1.0)
        == doctest::Approx(kCapG5).epsilon(1e-5));
  CHECK(generation_of(system_lp, "Alta").value_or(-1.0)
        == doctest::Approx(kCapG1).epsilon(1e-5));
  CHECK(generation_of(system_lp, "ParkCity").value_or(-1.0)
        == doctest::Approx(kCapG2).epsilon(1e-5));
  CHECK(generation_of(system_lp, "Solitude").value_or(-1.0)
        == doctest::Approx(kG3Disp).epsilon(1e-5));
  CHECK(generation_of(system_lp, "Sundance").value_or(-1.0)
        == doctest::Approx(0.0).epsilon(1e-5));

  // Objective = Σ gen_i · gcost_i.
  constexpr Real kObjective = kCapG5 * kCostG5  //  6 000
      + kCapG1 * kCostG1  //    560
      + kCapG2 * kCostG2  //  2 550
      + kG3Disp * kCostG3;  //  5 700  → 14 810
  CHECK(lp.get_obj_value() == doctest::Approx(kObjective).epsilon(1e-5));
}

// ── Capped — binding CO₂ cap forces coal (G2) → gas (G3) swap ──────────

TEST_CASE(
    "PJM-5 port — binding CO₂ cap displaces the marginal coal unit "
    "in favour of gas")  // NOLINT
{
  // Uncapped CO₂ = (40 + 170)·0.9933 + 190·0.42075 = 288.53 tCO₂.
  // Pick a cap below this to force a swap.  At cap = 200 tCO₂ the
  // marginal coal unit (G2, $15) is swapped for gas (G3, $30) — the
  // cheaper coal (G1, $14) stays on, as do the renewables.  Each
  // MWh of coal→gas saves (0.9933 − 0.42075) = 0.57255 tCO₂ and
  // costs ($30 − $15) = $15 extra.
  constexpr Real kCap = 200.0;
  auto sys = make_pjm5_system(kCap);
  sys.fold_legacy_fuel_emission_factors();
  sys.expand_fuel_emission_sources();

  const auto simulation = make_one_block_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Closed-form expected swap:
  //   Δ_CO₂   = 288.53 − 200 = 88.53 tCO₂.
  //   x_swap  = Δ_CO₂ / (kRateCoal − kRateGas) ≈ 154.63 MW.
  //   G2_new  = kCapG2 − x_swap.
  //   G3_new  = (kDemand − kCapG5 − kCapG1) − G2_new.
  constexpr Real kUncappedCoal = kCapG1 + kCapG2;  // 210 MW
  constexpr Real kUncappedGas = kDemand - kCapG5 - kUncappedCoal;  // 190 MW
  constexpr Real kUncappedCo2 =
      kUncappedCoal * kRateCoal + kUncappedGas * kRateGas;
  constexpr Real kDeltaCo2 = kUncappedCo2 - kCap;
  constexpr Real kSwapMw = kDeltaCo2 / (kRateCoal - kRateGas);
  constexpr Real kG2New = kCapG2 - kSwapMw;
  constexpr Real kG3New = (kDemand - kCapG5 - kCapG1) - kG2New;

  CHECK(generation_of(system_lp, "Brighton").value_or(-1.0)
        == doctest::Approx(kCapG5).epsilon(1e-5));
  CHECK(generation_of(system_lp, "Alta").value_or(-1.0)
        == doctest::Approx(kCapG1).epsilon(1e-5));
  CHECK(generation_of(system_lp, "ParkCity").value_or(-1.0)
        == doctest::Approx(kG2New).epsilon(1e-5));
  CHECK(generation_of(system_lp, "Solitude").value_or(-1.0)
        == doctest::Approx(kG3New).epsilon(1e-5));
  CHECK(generation_of(system_lp, "Sundance").value_or(-1.0)
        == doctest::Approx(0.0).epsilon(1e-5));

  // Cap-binding spot check — total CO₂ equals the cap.
  const Real total_co2 = (kCapG1 + kG2New) * kRateCoal + kG3New * kRateGas;
  CHECK(total_co2 == doctest::Approx(kCap).epsilon(1e-5));

  // Objective = base merit-order cost + swap penalty
  //           = 14 810 + kSwapMw · ($30 − $15).
  constexpr Real kBaseObjective = kCapG5 * kCostG5 + kCapG1 * kCostG1
      + kCapG2 * kCostG2
      + (kDemand - kCapG5 - kCapG1 - kCapG2) * kCostG3;  // 14 810
  constexpr Real kSwapPenalty = kSwapMw * (kCostG3 - kCostG2);
  CHECK(lp.get_obj_value()
        == doctest::Approx(kBaseObjective + kSwapPenalty).epsilon(1e-5));
}

}  // namespace test_emission_pjm5_port
