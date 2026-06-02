// SPDX-License-Identifier: BSD-3-Clause
//
// Port of the RTS-GMLC (GridMod 2019) test system into gtopt's emission-
// framework integration test.  The Python converter
// ``scripts/rts_gmlc_to_gtopt`` does the full 158-generator / 73-bus
// conversion with per-pollutant accounting wired from RTS-GMLC's native
// ``Emissions <X> Lbs/MMBTU`` columns; the C++ test below mirrors that
// path on a tractable named subset chosen to cover the multi-pollutant
// and per-gen-rate fan-out.
//
// ## Reference
//
// Barrows, Bloom, Ehlen, Ikäheimo, Jorgenson, Krishnamurthy, Lau, McBennett,
//   O'Connell, Preston, Staid, Stephen, Watson, 2020. "The IEEE Reliability
//   Test System: A Proposed 2019 Update."  IEEE Trans. Power Sys. 35(1).
//
// RTS-GMLC ships per-generator combustion rates in lbs CO₂ / MMBTU
// directly on each row of ``gen.csv`` — there is no per-fuel IPCC
// overlay needed.  Conversion:
//
//     1 lb = 0.4536 kg     →     × 4.536e-4  for tons
//     1 MMBTU = 1.055 GJ   →     ÷ 1.055     for GJ-basis
//
// So multiplying CSV "Emissions CO2 Lbs/MMBTU" by ``0.4536e-3 / 1.055``
// gives ``tCO2 / GJ``.  Sanity:
//
//     101_STEAM_3 (Coal):   210 lbs/MMBTU → 0.09029 tCO2/GJ
//     101_CT_1   (Oil):     160 lbs/MMBTU → 0.06880 tCO2/GJ
//     118_CC_1   (NG):      117 lbs/MMBTU → 0.05031 tCO2/GJ
//     118_NUC_1  (Nuclear):   0 lbs/MMBTU → 0.00000 tCO2/GJ
//     113_BIO_1  (Biomass):   0 lbs/MMBTU → 0.00000 tCO2/GJ
//
// (113_BIO_1 doesn't actually appear in RTS-GMLC; we synthesize it
// here to round out the biomass slot — the test is about exercising
// the framework's per-gen path, not reproducing every row of the
// upstream CSV verbatim.)
//
// ## What the test asserts
//
// 1. **Per-gen rate sanity** — the synthesized EmissionSource.rate
//    must equal `heat_rate × combustion` (heat_content = 1 default).
//    We verify this for the five named generators above.
// 2. **Aggregate** — a 24-hour single-bus dispatch must produce a
//    finite, physically plausible total CO₂.  RTS-GMLC has a ~10 GW
//    peak; the day-aggregate CO₂ ceiling of 50 kt CO₂/day in the
//    task spec corresponds to ~5 tCO₂/MWh × 10 GW × 24 h, which is
//    only reachable if every generator runs at 100% on pure oil —
//    so the actual dispatch is well below that.

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

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// Unique outer namespace to avoid collisions when CMake batches into a
// unity TU.
namespace test_emission_rts_gmlc_port
{
namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// Conversion factor LBS/MMBTU → tCO2/GJ.
constexpr Real kLbsPerMmbtuToTco2PerGj = 0.4536e-3 / 1.055;  // ≈ 0.0004299

// ── Named RTS-GMLC-style generators ─────────────────────────────────────
struct RtsGen
{
  Uid uid;
  std::string_view name;
  Real pmax;
  Real vom;  // $/MWh
  Real fuel_price_per_mmbtu;
  Real heat_rate_btu_per_kwh;  // = MMBTU/MWh × 1000
  Real co2_lbs_per_mmbtu;
};

// Hard-coded fleet — values from RTS-GMLC ``gen.csv`` (HR_avg_0 column
// for heat rate, Emissions CO2 Lbs/MMBTU column for combustion rate).
// 101_STEAM_3 / 101_CT_1 / 118_NUC_1 come straight from the CSV;
// CC_1 + BIO_1 are representative synthetics in the same shape.
constexpr RtsGen kGenSteam3 = {
    .uid = Uid {1},
    .name = "101_STEAM_3",
    .pmax = 76.0,
    .vom = 2.0,
    .fuel_price_per_mmbtu = 2.11,  // RTS-GMLC coal price
    .heat_rate_btu_per_kwh = 13270.0,
    .co2_lbs_per_mmbtu = 210.0,
};
constexpr RtsGen kGenCT1 = {
    .uid = Uid {2},
    .name = "101_CT_1",
    .pmax = 20.0,
    .vom = 5.0,
    .fuel_price_per_mmbtu = 17.0,  // oil
    .heat_rate_btu_per_kwh = 13114.0,
    .co2_lbs_per_mmbtu = 160.0,
};
constexpr RtsGen kGenCC1 = {
    .uid = Uid {3},
    .name = "118_CC_1",
    .pmax = 355.0,
    .vom = 2.5,
    .fuel_price_per_mmbtu = 4.5,  // NG
    .heat_rate_btu_per_kwh = 7140.0,
    .co2_lbs_per_mmbtu = 117.0,
};
constexpr RtsGen kGenNuclear = {
    .uid = Uid {4},
    .name = "118_NUC_1",
    .pmax = 400.0,
    .vom = 0.5,
    .fuel_price_per_mmbtu = 0.72,  // nuclear is cheap
    .heat_rate_btu_per_kwh = 10446.0,
    .co2_lbs_per_mmbtu = 0.0,
};
constexpr RtsGen kGenBiomass = {
    .uid = Uid {5},
    .name = "113_BIO_1",
    .pmax = 30.0,
    .vom = 4.0,
    .fuel_price_per_mmbtu = 1.0,
    .heat_rate_btu_per_kwh = 13000.0,
    .co2_lbs_per_mmbtu = 0.0,  // biogenic-zero
};

// Convert BTU/kWh → GJ/MWh: factor 1.055e-3.
[[nodiscard]] constexpr Real heat_rate_gj_per_mwh(const RtsGen& g) noexcept
{
  return g.heat_rate_btu_per_kwh * 1.055e-3;
}
[[nodiscard]] constexpr Real combustion_tco2_per_gj(const RtsGen& g) noexcept
{
  return g.co2_lbs_per_mmbtu * kLbsPerMmbtuToTco2PerGj;
}
// SRMC = VOM + heat_rate (MMBtu/MWh) × fuel price ($/MMBtu)
// where heat_rate (MMBtu/MWh) = BTU/kWh × 1e-3.
[[nodiscard]] constexpr Real srmc(const RtsGen& g) noexcept
{
  const Real hr_mmbtu_per_mwh = g.heat_rate_btu_per_kwh * 1.0e-3;
  return g.vom + hr_mmbtu_per_mwh * g.fuel_price_per_mmbtu;
}
// tCO2/MWh = heat_rate (GJ/MWh) × combustion (tCO2/GJ)
[[nodiscard]] constexpr Real co2_rate(const RtsGen& g) noexcept
{
  return heat_rate_gj_per_mwh(g) * combustion_tco2_per_gj(g);
}

constexpr Real kDemandPerHour = 600.0;  // MW — leaves nuclear + CC headroom
constexpr Real kHoursPerDay = 24.0;
constexpr Real kDemandFailCost = 1000.0;

constexpr Uid kBusUid {1};
constexpr Uid kEmissionCO2 {1};
constexpr Uid kZoneCO2 {1};

// One fuel per generator (so each carries its row's per-gen rate).
[[nodiscard]] Uid fuel_uid_of(const RtsGen& g) noexcept
{
  return Uid {g.uid + 100};
}

[[nodiscard]] Fuel make_fuel(const RtsGen& g)
{
  Fuel f;
  f.uid = fuel_uid_of(g);
  f.name = std::string {g.name} + "_fuel";
  f.price = 0.0;  // fuel $ already folded into gcost below
  f.emission_factors = {
      {
          .emission = SingleId {kEmissionCO2},
          .combustion = combustion_tco2_per_gj(g),
      },
  };
  return f;
}

[[nodiscard]] Generator make_generator(const RtsGen& g)
{
  Generator gen;
  gen.uid = g.uid;
  gen.name = std::string {g.name};
  gen.bus = kBusUid;
  gen.gcost = srmc(g);
  gen.capacity = g.pmax;
  // Renewable / nuclear-zero-combustion units still get a fuel + heat
  // rate so the EmissionSource expand pass enumerates them (even at
  // rate=0, the framework still synthesizes the bridge row when *any*
  // factor on the fuel is non-zero; biomass and nuclear get skipped
  // automatically when both combustion and upstream are zero).
  if (g.heat_rate_btu_per_kwh > 0.0) {
    gen.heat_rate = heat_rate_gj_per_mwh(g);
    gen.fuel = SingleId {fuel_uid_of(g)};
  }
  return gen;
}

// Build the 5-generator system with multi-hour dispatch.  ``hours``
// controls the simulation length.
[[nodiscard]] System make_system()
{
  System sys;
  sys.name = "RTSGMLCPort";
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
          .capacity = kDemandPerHour,
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

  sys.fuel_array = {
      make_fuel(kGenSteam3),
      make_fuel(kGenCT1),
      make_fuel(kGenCC1),
      make_fuel(kGenNuclear),
      make_fuel(kGenBiomass),
  };
  sys.generator_array = {
      make_generator(kGenSteam3),
      make_generator(kGenCT1),
      make_generator(kGenCC1),
      make_generator(kGenNuclear),
      make_generator(kGenBiomass),
  };
  return sys;
}

[[nodiscard]] Simulation make_simulation(int hours)
{
  Simulation sim;
  sim.block_array.reserve(static_cast<std::size_t>(hours));
  for (int h = 0; h < hours; ++h) {
    sim.block_array.push_back(Block {
        .uid = Uid {h + 1},
        .duration = 1.0,
    });
  }
  sim.stage_array = {
      {
          .uid = Uid {1},
          .first_block = 0,
          .count_block = static_cast<std::size_t>(hours),
      },
  };
  sim.scenario_array = {
      {
          .uid = Uid {0},
          .probability_factor = 1.0,
      },
  };
  return sim;
}

[[nodiscard]] PlanningOptions make_options()
{
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = kDemandFailCost;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.use_single_bus = true;
  return popts;
}

// Locate a generator's dispatch in block b (0-indexed) for the first
// scenario/stage.
[[nodiscard]] std::optional<Real> generation_at(SystemLP& system_lp,
                                                std::string_view gen_name,
                                                std::size_t block_idx)
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
  if (block_idx >= t.blocks().size()) {
    return std::nullopt;
  }
  const auto buid = t.blocks()[block_idx].uid();
  const auto col_it = gen_cols.find(buid);
  if (col_it == gen_cols.end()) {
    return std::nullopt;
  }
  return system_lp.linear_interface().get_col_sol()[col_it->second];
}

// Find a synthesized EmissionSource by the generator's name.  Returns
// nullopt if no source is enumerated for that generator (which is
// expected for zero-combustion fuels like nuclear / biomass).
[[nodiscard]] std::optional<Real> source_rate_for_generator(
    const System& sys, std::string_view gen_name)
{
  for (const auto& src : sys.emission_source_array) {
    if (src.name.find(gen_name) != std::string::npos && src.rate.has_value()) {
      return std::get<Real>(src.rate.value_or(Real {0.0}));
    }
  }
  return std::nullopt;
}

}  // namespace

// ── Per-gen rate sanity ────────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "RTS-GMLC port — per-gen EmissionSource.rate matches "
    "heat_rate × combustion within 5%")
{
  auto sys = make_system();
  sys.fold_legacy_fuel_emission_factors();
  sys.expand_fuel_emission_sources();

  // Coal / oil / gas units produce a source.  Nuclear + biomass are
  // SKIPPED by the expand pass because combustion = upstream = 0.
  REQUIRE(sys.emission_source_array.size() == 3);

  // Spot-check three named sources.
  const auto coal_rate = source_rate_for_generator(sys, "101_STEAM_3");
  REQUIRE(coal_rate.has_value());
  CHECK(*coal_rate == doctest::Approx(co2_rate(kGenSteam3)).epsilon(0.05));

  const auto oil_rate = source_rate_for_generator(sys, "101_CT_1");
  REQUIRE(oil_rate.has_value());
  CHECK(*oil_rate == doctest::Approx(co2_rate(kGenCT1)).epsilon(0.05));

  const auto gas_rate = source_rate_for_generator(sys, "118_CC_1");
  REQUIRE(gas_rate.has_value());
  CHECK(*gas_rate == doctest::Approx(co2_rate(kGenCC1)).epsilon(0.05));

  // Nuclear / biomass were skipped — confirm.
  CHECK_FALSE(source_rate_for_generator(sys, "118_NUC_1").has_value());
  CHECK_FALSE(source_rate_for_generator(sys, "113_BIO_1").has_value());

  // Sanity vs the spec's reference value (0.0903 tCO2/GJ for the coal):
  CHECK(combustion_tco2_per_gj(kGenSteam3)
        == doctest::Approx(0.0903).epsilon(0.005));
}

// ── 24-hour aggregate dispatch ─────────────────────────────────────────

TEST_CASE(  // NOLINT
    "RTS-GMLC port — 24-hour single-bus dispatch produces a finite, "
    "physically plausible total CO₂")
{
  auto sys = make_system();
  sys.fold_legacy_fuel_emission_factors();
  sys.expand_fuel_emission_sources();

  const auto simulation = make_simulation(static_cast<int>(kHoursPerDay));
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Aggregate CO₂: sum over (block, generator) of gen × rate × dur.
  Real total = 0.0;
  const auto& sources = system_lp.elements<EmissionSourceLP>();
  for (const auto& src_lp : sources) {
    const auto& src = src_lp.emission_source();
    if (!src.generator.has_value() || !src.rate.has_value()) {
      continue;
    }
    const auto gen_uid = std::get<Uid>(*src.generator);
    const auto& gens = system_lp.elements<GeneratorLP>();
    const auto gen_it = std::ranges::find_if(
        gens, [&](const auto& g) { return g.generator().uid == gen_uid; });
    if (gen_it == gens.end()) {
      continue;
    }
    const Real rate = std::get<Real>(src.rate.value_or(Real {0.0}));
    for (std::size_t b = 0; b < static_cast<std::size_t>(kHoursPerDay); ++b) {
      const auto disp = generation_at(system_lp, gen_it->generator().name, b);
      if (disp.has_value()) {
        total += *disp * rate * 1.0;  // 1-hour blocks
      }
    }
  }

  // Physical bounds:
  //   * Strictly positive — the LP cannot meet 600 MW load without
  //     burning coal or gas (nuclear capacity = 400 MW < demand).
  //   * Well below 50 ktCO₂/day for a 600 MW × 24 h system.  At the
  //     dirtiest unit (coal at ~1.02 tCO2/MWh) the upper bound on
  //     CO₂ if EVERY MWh came from coal would be ~14.7 kt; the LP
  //     will pick CC NG + nuclear first, so we expect well under that.
  CHECK(total > 0.0);
  CHECK(total < 50000.0);  // 50 ktCO₂/day ceiling per task spec.
}

}  // namespace test_emission_rts_gmlc_port
