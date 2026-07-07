// SPDX-License-Identifier: BSD-3-Clause
//
// Port of the NREL-118 (Pena, Martinez-Anido, Hodge 2017) test system into
// gtopt's emission-framework integration test.  The Python converter
// ``scripts/nrel118_to_gtopt`` does the full 118-bus / 327-generator /
// 168-hour conversion (single-bus aggregation + piecewise heat-rate
// collapse); the C++ test that follows mirrors the analytic pattern of
// the other emission ports (PJM-5, GenX, PyPSA) on a tractable subset of
// the NREL-118 fleet sized so the renewable-penetration delta is
// reproducible without dragging in the full LP.
//
// ## Reference
//
// Pena, I., Martinez-Anido, C., Hodge, B-M., 2018.
//   "An Extended IEEE 118-Bus Test System with High Renewable
//   Penetration."  IEEE Trans. Power Sys. 33(1).
//   DOI: 10.1109/TPWRS.2017.2695963
//
// The published result — a 29-34 % CO₂ reduction at 33 % wind+solar
// penetration vs the no-renewables baseline — is a full-year (8784 h)
// optimal-dispatch + UC outcome.  A 168-hour single-week LP slice is
// inherently noisier than the annual figure, so we assert the delta
// falls in a **CONSERVATIVE 15-50 % window** wider than the annual
// band as the task spec calls out.
//
// ## Subset rationale (small enough to assemble in C++)
//
// We pick one representative generator per IPCC-distinct fuel kind
// shown in the NREL-118 fleet:
//
//   | Kind         | Tag in NREL-118 ``Generators.csv``       | t CO2 / GJ |
//   |--------------|------------------------------------------|------------|
//   | Coal         | "ST Coal 01"                             | 0.0946     |
//   | Natural gas  | "CC NG 01" (combined-cycle)              | 0.0561     |
//   | Natural gas  | "CT NG 01" (combustion turbine peaker)   | 0.0561     |
//   | Oil          | "CT Oil 01" (diesel peaker)              | 0.0741     |
//   | Biomass      | "Biomass 01" (biogenic-zero)             | 0.0        |
//   | Nuclear      | implicit baseload (zero CO₂, no fuel)    | —          |
//
// Heat rates are the pmax-weighted segment-average values the converter
// produces for the NREL-118 Generators.csv rows.  We hard-code those
// derived scalars here so the test does NOT depend on the converter's
// CSV cache being primed at C++ build / CTest time.  The converter's
// own piecewise-collapse logic is exercised by the Python unit tests in
// ``scripts/nrel118_to_gtopt/tests/``.
//
// Capacities and demand are chosen so that:
//   1. The baseline (no renewables) saturates coal + gas before tapping
//      the oil peaker, giving a non-trivial CO₂ output.
//   2. The 33 %-renewables variant displaces the dirtiest marginal unit
//      first (coal → renewable) and bites into the CC NG dispatch.
// The resulting CO₂ delta lands cleanly inside the [15 %, 50 %] window.

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

// Unique outer namespace to avoid colliding with the other port tests
// when CMake batches them into a unity TU.
namespace test_emission_nrel118_port
{
namespace
{

// ── IPCC AR6 combustion factors (tCO₂ / GJ) ────────────────────────────
constexpr Real kEfCoal = 0.0946;
constexpr Real kEfGas = 0.0561;
constexpr Real kEfOil = 0.0741;
constexpr Real kEfBiomass = 0.0;  // biogenic-zero

// ── Representative pmax-weighted scalar heat rates (GJ / MWh)
// matching what
// `scripts/nrel118_to_gtopt._converter.collapse_piecewise_heat_rate` produces
// for the named NREL-118 rows.  Verified by piping the cached Generators.csv
// through that helper:
//
//     "ST Coal 01" : HR_Base=108.8 MMBTU/hr, Inc1=10171.43 BTU/kWh,
//                    LP1=19.84, LP5=20 → 10.736 GJ/MWh
//     "CC NG 01"   : HR_Base=316.78 MMBTU/hr, Inc1=6489 BTU/kWh,
//                    LP1=41.5 → 8.045 GJ/MWh
//     "CT NG 01"   : HR_Base=133.1 MMBTU/hr, Inc1=8978.49 BTU/kWh,
//                    LP1=17.78 → 9.479 GJ/MWh
//     "CT Oil 01"  : HR_Base=362.03 MMBTU/hr, Inc1=9973.58 BTU/kWh,
//                    LP1=37.25 → 10.527 GJ/MWh
//     "Biomass 01" : HR_Base=10.91 MMBTU/hr, Inc1=12120 BTU/kWh,
//                    LP1=3 → 12.787 GJ/MWh  (combustion factor = 0)
constexpr Real kHrCoal = 10.736;
constexpr Real kHrCCGas = 8.045;
constexpr Real kHrCTGas = 9.479;
constexpr Real kHrCTOil = 10.527;
constexpr Real kHrBiomass = 12.787;

// ── Per-MWh combustion CO₂ rates ───────────────────────────────────────
constexpr Real kRateCoal = kHrCoal * kEfCoal;  // 1.01563  t / MWh
constexpr Real kRateCCGas = kHrCCGas * kEfGas;  // 0.45132  t / MWh
constexpr Real kRateCTGas = kHrCTGas * kEfGas;  // 0.53177  t / MWh
constexpr Real kRateCTOil = kHrCTOil * kEfOil;  // 0.78005  t / MWh

// ── Capacities (MW) — sized so the dispatch is non-degenerate ──────────
constexpr Real kCapNuclear = 500.0;  // baseload, gcost cheapest
constexpr Real kCapCoal = 200.0;
constexpr Real kCapCCGas = 200.0;
constexpr Real kCapCTGas = 200.0;
constexpr Real kCapCTOil = 200.0;
constexpr Real kCapBiomass = 60.0;

// ── Demand (MW) ─────────────────────────────────────────────────────────
// Total thermal capacity = 1360 MW; demand = 1100 MW leaves headroom so
// that the 33 % renewable injection (≈ 360 MW) doesn't crater dispatch
// below baseload.
constexpr Real kDemand = 1100.0;
constexpr Real kBlockDur = 1.0;
constexpr Real kDemandFailCost = 1000.0;

// ── Per-MWh gen costs ($/MWh) — VO&M from NREL-118 + a small ranking
// offset so the merit order is well-defined.  We do NOT add fuel cost
// to gcost (NREL-118 doesn't ship per-fuel prices on this CSV); the
// LP's only dispatch signal is gcost, which we choose to mirror the
// typical SRMC ranking.
constexpr Real kCostNuclear = 5.0;  // baseload
constexpr Real kCostCoal = 25.0;
constexpr Real kCostCCGas = 35.0;
constexpr Real kCostCTGas = 60.0;
constexpr Real kCostCTOil = 100.0;
constexpr Real kCostBiomass = 50.0;  // renewable but VOM matters
constexpr Real kCostRenewable = 0.5;  // utility-scale wind / solar SRMC

// ── Fuel + emission UIDs ───────────────────────────────────────────────
constexpr Uid kFuelCoal {1};
constexpr Uid kFuelGas {2};
constexpr Uid kFuelOil {3};
constexpr Uid kFuelBiomass {4};
constexpr Uid kEmissionCO2 {1};
constexpr Uid kZoneCO2 {1};
constexpr Uid kBusUid {1};

constexpr Uid kGenNuclear {1};
constexpr Uid kGenCoal {2};
constexpr Uid kGenCCGas {3};
constexpr Uid kGenCTGas {4};
constexpr Uid kGenCTOil {5};
constexpr Uid kGenBiomass {6};
constexpr Uid kGenRenewable {7};  // present only in the 33 % variant

// Build the NREL-118-subset system.  ``renewables_share`` ∈ [0, 1]:
//   0    → baseline (thermals at full capacity, no renewable injection)
//   0.33 → IEEE 33 %-renewables variant: thermal capacities derated by
//          (1 − 0.33), aggregate renewable added at ``share × demand``.
[[nodiscard]] System make_nrel118_subset_system(Real renewables_share)
{
  System sys;
  sys.name = "NREL118Subset";
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

  sys.emission_array = {
      {
          .uid = kEmissionCO2,
          .name = "co2",
      },
  };
  sys.emission_zone_array = {
      {
          .uid = kZoneCO2,
          .name = "global_co2",
          .emissions =
              {
                  {
                      .emission = kEmissionCO2,
                      .weight = 1.0,
                  },
              },
      },
  };

  // Fuels — price=0 (NREL-118 ships no fuel prices on Generators.csv);
  // gcost on the generator carries dispatch order.
  sys.fuel_array = {
      {
          .uid = kFuelCoal,
          .name = "coal",
          .price = 0.0,
          .emission_factors =
              {
                  {
                      .emission = SingleId {kEmissionCO2},
                      .combustion = kEfCoal,
                  },
              },
      },
      {
          .uid = kFuelGas,
          .name = "natural_gas",
          .price = 0.0,
          .emission_factors =
              {
                  {
                      .emission = SingleId {kEmissionCO2},
                      .combustion = kEfGas,
                  },
              },
      },
      {
          .uid = kFuelOil,
          .name = "diesel",
          .price = 0.0,
          .emission_factors =
              {
                  {
                      .emission = SingleId {kEmissionCO2},
                      .combustion = kEfOil,
                  },
              },
      },
      {
          .uid = kFuelBiomass,
          .name = "biomass",
          .price = 0.0,
          .emission_factors =
              {
                  {
                      .emission = SingleId {kEmissionCO2},
                      .combustion = kEfBiomass,
                  },
              },
      },
  };

  const Real derate = 1.0 - renewables_share;
  sys.generator_array = {
      {
          .uid = kGenNuclear,
          .name = "nuclear_baseload",
          .bus = kBusUid,
          .gcost = kCostNuclear,
          .capacity = kCapNuclear * derate,
      },
      {
          .uid = kGenCoal,
          .name = "ST_Coal_01",
          .bus = kBusUid,
          .gcost = kCostCoal,
          .fuel = SingleId {kFuelCoal},
          .heat_rate = kHrCoal,
          .capacity = kCapCoal * derate,
      },
      {
          .uid = kGenCCGas,
          .name = "CC_NG_01",
          .bus = kBusUid,
          .gcost = kCostCCGas,
          .fuel = SingleId {kFuelGas},
          .heat_rate = kHrCCGas,
          .capacity = kCapCCGas * derate,
      },
      {
          .uid = kGenCTGas,
          .name = "CT_NG_01",
          .bus = kBusUid,
          .gcost = kCostCTGas,
          .fuel = SingleId {kFuelGas},
          .heat_rate = kHrCTGas,
          .capacity = kCapCTGas * derate,
      },
      {
          .uid = kGenCTOil,
          .name = "CT_Oil_01",
          .bus = kBusUid,
          .gcost = kCostCTOil,
          .fuel = SingleId {kFuelOil},
          .heat_rate = kHrCTOil,
          .capacity = kCapCTOil * derate,
      },
      {
          .uid = kGenBiomass,
          .name = "Biomass_01",
          .bus = kBusUid,
          .gcost = kCostBiomass,
          .fuel = SingleId {kFuelBiomass},
          .heat_rate = kHrBiomass,
          .capacity = kCapBiomass * derate,
      },
  };
  if (renewables_share > 0.0) {
    sys.generator_array.push_back(Generator {
        .uid = kGenRenewable,
        .name = "aggregate_renewables",
        .bus = kBusUid,
        .gcost = kCostRenewable,
        .capacity = kDemand * renewables_share,
    });
  }
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

// Locate a generator's MW dispatch (one cell).
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

// Compute the total post-solve CO₂ (tCO₂) for the single block by summing
// gen_MW × rate over every EmissionSource the system emitted.
[[nodiscard]] Real total_co2(SystemLP& system_lp)
{
  Real total = 0.0;
  const auto& sources = system_lp.elements<EmissionSourceLP>();
  for (const auto& src_lp : sources) {
    const auto& src = src_lp.emission_source();
    if (!src.generator.has_value() || !src.rate.has_value()) {
      continue;
    }
    const auto gen_uid = std::get<Uid>(*src.generator);
    const auto& gens = system_lp.elements<GeneratorLP>();
    const auto it = std::ranges::find_if(
        gens, [&](const auto& g) { return g.generator().uid == gen_uid; });
    if (it == gens.end()) {
      continue;
    }
    const auto disp = generation_of(system_lp, it->generator().name);
    if (!disp.has_value()) {
      continue;
    }
    const Real rate = std::get<Real>(src.rate.value_or(Real {0.0}));
    total += *disp * rate * kBlockDur;
  }
  return total;
}

}  // namespace

// ── Sanity — fuel-derived EmissionSource fan-out ────────────────────────

TEST_CASE(  // NOLINT
    "NREL-118 subset port — expand_fuel_emission_sources writes one "
    "EmissionSource per fueled (non-renewable) generator")
{
  auto sys = make_nrel118_subset_system(/*renewables_share=*/0.0);
  sys.fold_legacy_fuel_emission_factors();
  sys.expand_fuel_emission_sources();

  // 5 fueled generators (coal, CC, CT NG, CT Oil, biomass) × 1 zone × 1
  // pollutant ⇒ 5 sources.  Nuclear has no fuel and is skipped.  Biomass
  // gets a source even though its combustion factor is 0 — the expand
  // pass only skips when *both* combustion and upstream are zero, which
  // matches the IPCC-AR6 convention (biomass is biogenic-zero but still
  // *enumerated* in the inventory).
  // Actually inspecting `expand_fuel_emission_sources`: the loop body
  // `if (combustion == 0.0 && upstream == 0.0) continue;` SKIPS the
  // biomass source — so we expect 4 sources.
  REQUIRE(sys.emission_source_array.size() == 4);

  // Spot-check: the synthesized rate for ST_Coal_01 must equal
  // heat_rate × combustion (heat_content = 1 default).
  const auto& coal_src = *std::ranges::find_if(
      sys.emission_source_array,
      [](const auto& s)
      { return s.name.find("ST_Coal_01") != std::string::npos; });
  CHECK(std::get<Real>(coal_src.rate.value_or(Real {0.0}))
        == doctest::Approx(kRateCoal).epsilon(1e-6));
}

// ── Baseline vs 33 % renewables — CO₂ delta in the [15 %, 50 %] window ─

TEST_CASE(  // NOLINT
    "NREL-118 subset port — 33% wind+solar penetration drops CO₂ by "
    "15-50% vs the no-renewables baseline")
{
  const auto simulation = make_one_block_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(simulation, options);

  // ── Baseline run ─────────────────────────────────────────────────────
  Real baseline_co2 = 0.0;
  {
    auto sys = make_nrel118_subset_system(/*renewables_share=*/0.0);
    sys.fold_legacy_fuel_emission_factors();
    sys.expand_fuel_emission_sources();
    SystemLP system_lp(sys, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    baseline_co2 = total_co2(system_lp);
    CHECK(baseline_co2 > 0.0);
  }

  // ── 33 %-renewables run ──────────────────────────────────────────────
  Real renewable_co2 = 0.0;
  {
    auto sys = make_nrel118_subset_system(/*renewables_share=*/0.33);
    sys.fold_legacy_fuel_emission_factors();
    sys.expand_fuel_emission_sources();
    SystemLP system_lp(sys, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    renewable_co2 = total_co2(system_lp);
    CHECK(renewable_co2 > 0.0);
  }

  // NOTE: window tightened from [15, 50] to [20, 45] per
  // ``integration_test/cases/nrel118_reference/headline_results.json``,
  // which captures the paper's 29-34 % annual CO2-reduction range at
  // 33 % wind+solar penetration with a ~10 pp slack each side to absorb
  // single-block-vs-8784-h variance.  See
  // ``integration_test/cases/nrel118_reference/PROVENANCE.md`` for the
  // citation chain and why we cannot tighten further without a full
  // annual UC fixture (issue #510).
  const Real reduction_pct =
      100.0 * (baseline_co2 - renewable_co2) / baseline_co2;
  CHECK(reduction_pct >= 20.0);
  CHECK(reduction_pct <= 45.0);
}

}  // namespace test_emission_nrel118_port
