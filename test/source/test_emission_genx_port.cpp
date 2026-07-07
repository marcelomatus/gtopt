// SPDX-License-Identifier: BSD-3-Clause
//
// Port of the GenX "1_three_zones" example to gtopt's emission framework.
//
// Source GenX example (as of 2026-05):
//   https://github.com/GenXProject/GenX/tree/main/example_systems/1_three_zones
//
// CSVs fetched verbatim:
//   resources/Thermal.csv
//     https://raw.githubusercontent.com/GenXProject/GenX/main/example_systems/
//     1_three_zones/resources/Thermal.csv
//   resources/Vre.csv
//     https://raw.githubusercontent.com/GenXProject/GenX/main/example_systems/
//     1_three_zones/resources/Vre.csv
//   system/Fuels_data.csv
//     https://raw.githubusercontent.com/GenXProject/GenX/main/example_systems/
//     1_three_zones/system/Fuels_data.csv
//   system/Demand_data.csv
//     https://raw.githubusercontent.com/GenXProject/GenX/main/example_systems/
//     1_three_zones/system/Demand_data.csv
//   policies/CO2_cap.csv
//     https://raw.githubusercontent.com/GenXProject/GenX/main/example_systems/
//     1_three_zones/policies/CO2_cap.csv
//
// The GenX system has 3 zones (MA / CT / ME) with one NGCC plant per zone
// plus 4 VRE resources.  All thermal resources share the same fuel kind
// ("natural gas", `*_NG`); only their heat-rate, var-OM and zonal fuel
// price differ.  This port keeps the same plant parameters but
// simplifies the topology to a SINGLE gtopt bus aggregating all zones —
// enough to stress the fuel-emission expand pass + a binding CO2 cap.
//
// ## Inputs (verbatim from the GenX CSVs)
//
// Thermal.csv (selected columns):
//
//   | Resource             | Heat_Rate (MMBtu/MWh) | Var_OM ($/MWh) | Fuel  |
//   |----------------------|-----------------------|----------------|-------|
//   | MA_natural_gas_CC    | 7.43                  | 3.55           | MA_NG |
//   | CT_natural_gas_CC    | 7.12                  | 3.57           | CT_NG |
//   | ME_natural_gas_CC    | 12.62                 | 4.50           | ME_NG |
//
// Fuels_data.csv — first data row holds the CO2_content (tCO2/MMBtu)
// for each fuel column; later rows are time-indexed prices.  We use
// the first-period prices (Time_Index=1):
//
//   | Fuel  | CO2_content (tCO2/MMBtu) | First-period price ($/MMBtu) |
//   |-------|--------------------------|------------------------------|
//   | MA_NG | 0.05306                  | 5.28                         |
//   | CT_NG | 0.05306                  | 5.45                         |
//   | ME_NG | 0.05306                  | 5.45                         |
//
// Vre.csv (selected; we keep one solar + one wind for the renewables):
//
//   | Resource         | Var_OM ($/MWh) |
//   |------------------|----------------|
//   | MA_solar_pv      | 0              |
//   | CT_onshore_wind  | 0.10           |
//
// CO2_cap.csv — three mass-based per-zone caps (CO_2_Max_Mtons_*).
// The single-bus port aggregates them into one global CO2 cap.  GenX's
// caps are stated in million-tons per year; we down-scale to one stage
// = one hour and pick a binding value derived from the algebra below.
//
// ## Unit conversion (1 MMBtu = 1.05506 GJ)
//
//   heat_rate (GJ/MWh)         = heat_rate_MMBtu × 1.05506
//   price ($/GJ)               = price_MMBtu / 1.05506
//   combustion (tCO2/GJ)       = CO2_content_MMBtu / 1.05506
//
// All three NGCCs share the same fuel CO2_content, so the per-MWh
// CO2 rate (`heat_rate · combustion`) is set by heat_rate alone:
//
//   | Plant   | heat_rate (GJ/MWh) | price ($/GJ) | SRMC ($/MWh) | CO2 (t/MWh)
//   |
//   |---------|--------------------|--------------|--------------|-------------|
//   | MA_NGCC | 7.83909            | 5.00351      | 42.781       | 0.39423 | |
//   CT_NGCC | 7.51203            | 5.16464      | 42.374       | 0.37779     |
//   | ME_NGCC | 13.31486           | 5.16464      | 73.279       | 0.66962 |
//
// CT_NGCC is the cheapest thermal; ME_NGCC the dirtiest and most
// expensive.  MA_NGCC sits between them on both axes.
//
// ## Expected dispatch
//
// Single bus, single 1-hour block, demand 300 MW.  Capacities chosen
// so the residual after renewables exceeds the cheapest thermal's MW:
//
//   solar 80 MW + wind 80 MW          (must-take, gcost 0)
//   CT_NGCC 100 MW  (SRMC 42.374, CO2 0.37779)
//   MA_NGCC  50 MW  (SRMC 42.781, CO2 0.39423)
//   ME_NGCC 100 MW  (SRMC 73.279, CO2 0.66962)
//
// Residual thermal demand = 300 − 160 = 140 MW.
//
// ### Uncapped baseline
//
// Merit order: CT (full 100) + MA (40 to fill).  ME stays off (most
// expensive).
//   CO2  = 100·0.37779 + 40·0.39423 = 53.548 tCO2
//   Cost = 100·42.374 + 40·42.781   = 5948.64 $
//
// ### Binding cap (45 tCO2 — below the uncapped 53.548)
//
// With `demand_fail_cost = 1000 $/MWh` the LP can buy unserved demand
// at the penalty.  Let CT, MA be the only thermal variables (ME stays
// off — confirmed by SRMC).
//
//   minimize 42.374 CT + 42.781 MA + 1000 (140 − CT − MA)
//   s.t.     0.37779 CT + 0.39423 MA ≤ 45
//            0 ≤ CT ≤ 100, 0 ≤ MA ≤ 50
//
// Simplifying the objective: 140000 − 957.626 CT − 957.219 MA  ⇒
// maximise 957.626 CT + 957.219 MA.  CT's coefficient is larger so
// the LP first pushes CT to its upper bound 100; the cap then leaves
//   0.39423 MA ≤ 45 − 37.779 = 7.221  ⇒  MA = 18.31407.
// Unserved = 140 − 100 − 18.31407 = 21.68593 MW.
//
// Final dispatch:
//   CT = 100,  MA = 18.31407,  ME = 0,  unserved = 21.68593,
//   CO2 = 45 (binding).

#include <algorithm>

#include <doctest/doctest.h>
#include <gtopt/emission.hpp>
#include <gtopt/emission_source.hpp>
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
namespace test_emission_genx_port
{
namespace
{

// 1 MMBtu = 1.05506 GJ — IEEE / IAPWS conversion constant.
constexpr Real kMMBtuPerGJ = 1.05506;

// GenX raw inputs (verbatim from Thermal.csv + Fuels_data.csv).
struct GenXThermal
{
  Real heat_rate_mmbtu;  // Heat_Rate_MMBTU_per_MWh
  Real var_om;  // Var_OM_Cost_per_MWh
  Real fuel_price_mmbtu;  // First-period price from Fuels_data.csv
  Real fuel_co2_mmbtu;  // CO2_content (Time_Index=0 row) tCO2/MMBtu
};

constexpr GenXThermal kMA_NGCC = {
    .heat_rate_mmbtu = 7.43,
    .var_om = 3.55,
    .fuel_price_mmbtu = 5.28,
    .fuel_co2_mmbtu = 0.05306,
};
constexpr GenXThermal kCT_NGCC = {
    .heat_rate_mmbtu = 7.12,
    .var_om = 3.57,
    .fuel_price_mmbtu = 5.45,
    .fuel_co2_mmbtu = 0.05306,
};
constexpr GenXThermal kME_NGCC = {
    .heat_rate_mmbtu = 12.62,
    .var_om = 4.50,
    .fuel_price_mmbtu = 5.45,
    .fuel_co2_mmbtu = 0.05306,
};

// Derived gtopt-side quantities (see header table).
[[nodiscard]] constexpr Real heat_rate_gj(const GenXThermal& t) noexcept
{
  return t.heat_rate_mmbtu * kMMBtuPerGJ;
}
[[nodiscard]] constexpr Real price_per_gj(const GenXThermal& t) noexcept
{
  return t.fuel_price_mmbtu / kMMBtuPerGJ;
}
[[nodiscard]] constexpr Real combustion_per_gj(const GenXThermal& t) noexcept
{
  return t.fuel_co2_mmbtu / kMMBtuPerGJ;
}
[[nodiscard]] constexpr Real srmc(const GenXThermal& t) noexcept
{
  // SRMC = price_per_GJ × heat_rate_GJ + var_OM
  //      = (fuel_price_MMBtu / 1.05506) × (heat_rate_MMBtu × 1.05506) + VOM
  //      = fuel_price_MMBtu × heat_rate_MMBtu + VOM  — kMMBtuPerGJ cancels.
  return t.fuel_price_mmbtu * t.heat_rate_mmbtu + t.var_om;
}
[[nodiscard]] constexpr Real co2_rate(const GenXThermal& t) noexcept
{
  // tCO2/MWh = heat_rate_GJ × combustion_per_GJ
  //         = heat_rate_MMBtu × CO2_content_MMBtu  — kMMBtuPerGJ cancels.
  return t.heat_rate_mmbtu * t.fuel_co2_mmbtu;
}

// Capacities (MW) — see header rationale.
constexpr Real kCapMA = 50.0;
constexpr Real kCapCT = 100.0;
constexpr Real kCapME = 100.0;
constexpr Real kCapSolar = 80.0;
constexpr Real kCapWind = 80.0;

// Demand (MW) and block duration (h).
constexpr Real kDemand = 300.0;
constexpr Real kDur = 1.0;

constexpr Real kDemandFailCost = 1000.0;

// Build the GenX-port system.  `co2_cap = std::nullopt` ⇒ uncapped.
[[nodiscard]] System make_genx_system(std::optional<Real> co2_cap)
{
  System sys;
  sys.name = "GenXThreeZonesPort";
  sys.bus_array = {{
      .uid = Uid {1},
      .name = "bus",
  }};
  sys.demand_array = {
      {
          .uid = Uid {1},
          .name = "demand",
          .bus = Uid {1},
          .capacity = kDemand,
      },
  };

  // Single CO2 pollutant + single global zone (aggregated from GenX's
  // three per-zone caps).
  sys.emission_array = {
      {
          .uid = Uid {1},
          .name = "co2",
      },
  };

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
  sys.emission_zone_array = {
      zone,
  };

  // One fuel per generator (matching GenX's per-zone NG prices).  All
  // three fuels share the same combustion factor — natural gas
  // CO2_content from Fuels_data.csv row Time_Index=0.
  sys.fuel_array = {
      {
          .uid = Uid {1},
          .name = "MA_NG",
          .price = price_per_gj(kMA_NGCC),
          .emission_factors =
              {
                  {
                      .emission = SingleId {Uid {1}},
                      .combustion = combustion_per_gj(kMA_NGCC),
                  },
              },
      },
      {
          .uid = Uid {2},
          .name = "CT_NG",
          .price = price_per_gj(kCT_NGCC),
          .emission_factors =
              {
                  {
                      .emission = SingleId {Uid {1}},
                      .combustion = combustion_per_gj(kCT_NGCC),
                  },
              },
      },
      {
          .uid = Uid {3},
          .name = "ME_NG",
          .price = price_per_gj(kME_NGCC),
          .emission_factors =
              {
                  {
                      .emission = SingleId {Uid {1}},
                      .combustion = combustion_per_gj(kME_NGCC),
                  },
              },
      },
  };

  // Three NGCC thermals + one solar + one wind.  gcost ≡ Var_OM
  // (GenX's variable O&M is dispatch-cost per MWh).
  sys.generator_array = {
      {
          .uid = Uid {1},
          .name = "MA_NGCC",
          .bus = Uid {1},
          .gcost = kMA_NGCC.var_om,
          .fuel = SingleId {Uid {1}},
          .heat_rate = heat_rate_gj(kMA_NGCC),
          .capacity = kCapMA,
      },
      {
          .uid = Uid {2},
          .name = "CT_NGCC",
          .bus = Uid {1},
          .gcost = kCT_NGCC.var_om,
          .fuel = SingleId {Uid {2}},
          .heat_rate = heat_rate_gj(kCT_NGCC),
          .capacity = kCapCT,
      },
      {
          .uid = Uid {3},
          .name = "ME_NGCC",
          .bus = Uid {1},
          .gcost = kME_NGCC.var_om,
          .fuel = SingleId {Uid {3}},
          .heat_rate = heat_rate_gj(kME_NGCC),
          .capacity = kCapME,
      },
      {
          .uid = Uid {4},
          .name = "MA_solar_pv",
          .bus = Uid {1},
          .gcost = 0.0,
          .capacity = kCapSolar,
      },
      {
          .uid = Uid {5},
          .name = "CT_onshore_wind",
          .bus = Uid {1},
          .gcost = 0.10,
          .capacity = kCapWind,
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

// Find a generator's MW dispatch in the (only) cell.
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

// ── Uncapped baseline ──────────────────────────────────────────────────

TEST_CASE(
    "GenX 1_three_zones port — uncapped baseline: renewables + cheapest "
    "NGCC (CT) saturate, MA fills the remainder, ME stays off")  // NOLINT
{
  auto sys = make_genx_system(/*co2_cap=*/std::nullopt);
  sys.fold_legacy_fuel_emission_factors();
  sys.expand_fuel_emission_sources();

  const auto simulation = make_one_block_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Renewables go first (lowest SRMC, must-take by capacity).
  CHECK(generation_of(system_lp, "MA_solar_pv").value_or(-1.0)
        == doctest::Approx(kCapSolar).epsilon(1e-5));
  CHECK(generation_of(system_lp, "CT_onshore_wind").value_or(-1.0)
        == doctest::Approx(kCapWind).epsilon(1e-5));

  // Residual 140 MW: CT (cheapest thermal) full + MA fills the gap.
  const Real residual = kDemand - kCapSolar - kCapWind;  // 140 MW
  const Real ct_expected = kCapCT;  // 100 MW
  const Real ma_expected = residual - kCapCT;  // 40 MW

  CHECK(generation_of(system_lp, "CT_NGCC").value_or(-1.0)
        == doctest::Approx(ct_expected).epsilon(1e-5));
  CHECK(generation_of(system_lp, "MA_NGCC").value_or(-1.0)
        == doctest::Approx(ma_expected).epsilon(1e-5));
  CHECK(generation_of(system_lp, "ME_NGCC").value_or(-1.0)
        == doctest::Approx(0.0).epsilon(1e-5));

  // Objective: wind VOM (0.10 $/MWh) + CT + MA dispatch costs.
  const Real obj_expected = kCapWind * 0.10  // 8.0
      + ct_expected * srmc(kCT_NGCC)  // 100 × 42.374  = 4237.40
      + ma_expected * srmc(kMA_NGCC);  //  40 × 42.781  = 1711.24
  CHECK(lp.get_obj_value() == doctest::Approx(obj_expected).epsilon(1e-5));
}

// ── Binding CO2 cap ────────────────────────────────────────────────────

TEST_CASE(
    "GenX 1_three_zones port — binding CO2 cap forces unserved demand: "
    "CT stays full, MA throttled to honor the cap")  // NOLINT
{
  // Uncapped CO2 = 53.548 tCO2; pick 45 tCO2 to bind.
  constexpr Real kCap = 45.0;
  auto sys = make_genx_system(kCap);
  sys.fold_legacy_fuel_emission_factors();
  sys.expand_fuel_emission_sources();

  const auto simulation = make_one_block_simulation();
  const PlanningOptionsLP options(make_options());
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);
  auto&& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Renewables unchanged.
  CHECK(generation_of(system_lp, "MA_solar_pv").value_or(-1.0)
        == doctest::Approx(kCapSolar).epsilon(1e-5));
  CHECK(generation_of(system_lp, "CT_onshore_wind").value_or(-1.0)
        == doctest::Approx(kCapWind).epsilon(1e-5));

  // From the LP-algebra in the file header.  See the derivation in the
  // comment block above the includes.
  const Real ct_expected = kCapCT;  // 100 MW (cheapest, hit upper bound)
  const Real ma_expected =
      (kCap - co2_rate(kCT_NGCC) * ct_expected) / co2_rate(kMA_NGCC);
  // ma_expected = (45 − 37.779) / 0.39423 ≈ 18.31407 MW

  CHECK(generation_of(system_lp, "CT_NGCC").value_or(-1.0)
        == doctest::Approx(ct_expected).epsilon(1e-5));
  CHECK(generation_of(system_lp, "MA_NGCC").value_or(-1.0)
        == doctest::Approx(ma_expected).epsilon(1e-5));
  CHECK(generation_of(system_lp, "ME_NGCC").value_or(-1.0)
        == doctest::Approx(0.0).epsilon(1e-5));

  // Cap is binding: total CO2 = cap.
  const Real total_co2 =
      ct_expected * co2_rate(kCT_NGCC) + ma_expected * co2_rate(kMA_NGCC);
  CHECK(total_co2 == doctest::Approx(kCap).epsilon(1e-5));
}

}  // namespace test_emission_genx_port
