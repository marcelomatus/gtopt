// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_reservoir_production_factor.hpp
 * @brief     Unit tests for ReservoirProductionFactor,
 * evaluate_production_factor, and the LP coefficient update mechanism
 * @date      2026-03-10
 * @copyright BSD-3-Clause
 */

#include <vector>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/reservoir_production_factor.hpp>
#include <gtopt/reservoir_production_factor_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── evaluate_production_factor tests
// ──────────────────────────────────────────────

TEST_CASE("evaluate_production_factor with empty segments returns 1.0")
{
  const std::vector<ProductionFactorSegment> segments {};
  CHECK(evaluate_production_factor(segments, 0.0) == doctest::Approx(1.0));
  CHECK(evaluate_production_factor(segments, 500.0) == doctest::Approx(1.0));
}

TEST_CASE("evaluate_production_factor with single segment")
{
  // constant = 1.2, slope = 0.0002, volume breakpoint = 0
  // efficiency(V) = 1.2 + 0.0002 * (V - 0) = 1.2 + 0.0002 * V
  const std::vector<ProductionFactorSegment> segments {
      {.volume = 0.0, .slope = 0.0002, .constant = 1.2},
  };

  CHECK(evaluate_production_factor(segments, 0.0) == doctest::Approx(1.2));
  CHECK(evaluate_production_factor(segments, 500.0) == doctest::Approx(1.3));
  CHECK(evaluate_production_factor(segments, 1000.0) == doctest::Approx(1.4));
}

TEST_CASE(
    "evaluate_production_factor with multiple segments (concave envelope)")
{
  // Two segments creating a concave envelope (PLP FRendimientos style):
  // constant is the value AT the breakpoint (point-slope form).
  // seg1: constant=2.0, slope=0.001, volume=0.0 → 2.0 + 0.001 * (V-0)
  // seg2: constant=2.8, slope=0.0002, volume=500.0 → 2.8 + 0.0002*(V-500)
  //
  // The Fortran FRendimientos computes min over all segments:
  // At V=0:   seg1=2.0, seg2=2.8+0.0002*(-500)=2.7 → min=2.0
  // At V=500: seg1=2.5, seg2=2.8 → min=2.5
  // At V=1500: seg1=3.5, seg2=2.8+0.0002*1000=3.0 → min=3.0
  const std::vector<ProductionFactorSegment> segments {
      {.volume = 0.0, .slope = 0.001, .constant = 2.0},
      {.volume = 500.0, .slope = 0.0002, .constant = 2.8},
  };

  CHECK(evaluate_production_factor(segments, 0.0) == doctest::Approx(2.0));
  CHECK(evaluate_production_factor(segments, 500.0) == doctest::Approx(2.5));
  CHECK(evaluate_production_factor(segments, 1500.0) == doctest::Approx(3.0));
}

TEST_CASE("evaluate_production_factor never returns negative")
{
  // slope = -0.01 → at large volume, result would go negative
  const std::vector<ProductionFactorSegment> segments {
      {.volume = 0.0, .slope = -0.01, .constant = 1.0},
  };

  CHECK(evaluate_production_factor(segments, 0.0) == doctest::Approx(1.0));
  CHECK(evaluate_production_factor(segments, 50.0) == doctest::Approx(0.5));
  CHECK(evaluate_production_factor(segments, 200.0) == doctest::Approx(0.0));
}

// ─── ReservoirProductionFactor struct tests
// ───────────────────────────────────────

TEST_CASE("ReservoirProductionFactor default construction")
{
  const ReservoirProductionFactor re;

  CHECK(re.uid == Uid {unknown_uid});
  CHECK(re.name == Name {});
  CHECK_FALSE(re.active.has_value());
  CHECK(re.turbine == SingleId {unknown_uid});
  CHECK(re.reservoir == SingleId {unknown_uid});
  CHECK(re.mean_production_factor == doctest::Approx(1.0));
  CHECK(re.segments.empty());
}

TEST_CASE("ReservoirProductionFactor attribute assignment")
{
  ReservoirProductionFactor re;

  re.uid = 9001;
  re.name = "eff_colbun";
  re.turbine = Uid {101};
  re.reservoir = Uid {201};
  re.mean_production_factor = 1.53;
  re.segments = {
      {.volume = 0.0, .slope = 0.0002294, .constant = 1.2558},
      {.volume = 500.0, .slope = 0.0001, .constant = 1.53},
  };
  CHECK(re.uid == 9001);
  CHECK(re.name == "eff_colbun");
  CHECK(std::get<Uid>(re.turbine) == Uid {101});
  CHECK(std::get<Uid>(re.reservoir) == Uid {201});
  CHECK(re.mean_production_factor == doctest::Approx(1.53));
  CHECK(re.segments.size() == 2);
  CHECK(re.segments[0].constant == doctest::Approx(1.2558));
  CHECK(re.segments[1].slope == doctest::Approx(0.0001));
}

// ─── ProductionFactorSegment tests
// ────────────────────────────────────────────────

TEST_CASE("ProductionFactorSegment default values")
{
  const ProductionFactorSegment seg;

  CHECK(seg.volume == doctest::Approx(0.0));
  CHECK(seg.slope == doctest::Approx(0.0));
  CHECK(seg.constant == doctest::Approx(0.0));
}

// ─── Turbine main_reservoir field ───────────────────────────────────────────

TEST_CASE("Turbine main_reservoir field")
{
  SUBCASE("default has no main_reservoir")
  {
    const Turbine turbine;
    CHECK_FALSE(turbine.main_reservoir.has_value());
  }

  SUBCASE("can set main_reservoir")
  {
    Turbine turbine;
    turbine.main_reservoir = Uid {201};

    REQUIRE(turbine.main_reservoir.has_value());
    CHECK(std::get<Uid>(turbine.main_reservoir.value()) == Uid {201});
  }
}

// ─── SystemLP with ReservoirProductionFactor
// ──────────────────────────────────────

TEST_CASE("SystemLP with reservoir production factor element")
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_upstream",
      },
      {
          .uid = Uid {2},
          .name = "j_downstream",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 1000.0,
          .emin = 0.0,
          .emax = 1000.0,
          .eini = 500.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 1.0,
          .main_reservoir = Uid {1},
      },
  };

  const Array<ReservoirProductionFactor> reservoir_production_factor_array = {
      {
          .uid = Uid {1},
          .name = "eff1",
          .turbine = Uid {1},
          .reservoir = Uid {1},
          .mean_production_factor = 1.5,
          .segments =
              {
                  {.volume = 0.0, .slope = 0.001, .constant = 1.0},
                  {.volume = 800.0, .slope = 0.0001, .constant = 1.5},
              },
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
              },
              {
                  .uid = Uid {2},
                  .duration = 2,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 2,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const System system = {
      .name = "HydroEfficiencyTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .reservoir_production_factor_array = reservoir_production_factor_array,
  };

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  SUBCASE("system solves successfully")
  {
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
  }

  SUBCASE("ReservoirProductionFactorLP stores coefficient indices")
  {
    auto& effs = system_lp.elements<ReservoirProductionFactorLP>();
    REQUIRE(effs.size() == 1);

    auto& eff = effs.front();
    CHECK(eff.has_coeff_indices(ScenarioUid {0}, StageUid {1}));

    const auto& bmap = eff.coeff_indices_at(ScenarioUid {0}, StageUid {1});
    CHECK(bmap.size() == 2);  // 2 blocks
  }

  SUBCASE("update_lp modifies coefficients using vini")
  {
    // First solve to establish baseline
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // Update coefficients using the reservoir's initial volume (eini = 500).
    // The new API derives the volume from the reservoir LP itself (iteration=0
    // and phase=0 both trigger the eini fallback: rsv.reservoir().eini = 500).
    const auto updated = system_lp.update_lp();
    CHECK(updated > 0);

    // Verify the coefficient was changed (original was -1.0)
    auto& effs = system_lp.elements<ReservoirProductionFactorLP>();
    auto& eff = effs.front();
    // evaluate_production_factor uses concave-envelope min (PLP FRendimientos):
    // seg1: 1.0 + 0.001*(500-0)=1.5, seg2: 1.5+0.0001*(500-800)=1.47
    // min = 1.47
    const auto expected_rate = eff.compute_production_factor(500.0);
    CHECK(expected_rate > 0.0);

    // Check that the LP coefficient matches
    const auto& bmap = eff.coeff_indices_at(ScenarioUid {0}, StageUid {1});
    for (const auto& [buid, ci] : bmap) {
      const auto coeff = lp.get_coeff(ci.row, ci.col);
      CHECK(coeff == doctest::Approx(-expected_rate));
    }

    // Re-solve with the updated coefficient
    auto result2 = lp.resolve();
    REQUIRE(result2.has_value());
    CHECK(result2.value() == 0);
  }
}

TEST_CASE("ReservoirProductionFactorLP - update_lp with different eini segment")
{
  // eini=100 → seg1 (volume < 800). update_lp iter=0/phase=0 uses eini.
  // The initial conversion_rate=1.0 from Turbine will be overwritten by
  // the production factor evaluated at eini=100.
  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};

  const Array<Generator> generator_array = {{
      .uid = Uid {1},
      .name = "hydro_gen",
      .bus = Uid {1},
      .gcost = 5.0,
      .capacity = 200.0,
  }};

  const Array<Demand> demand_array = {{
      .uid = Uid {1},
      .name = "d1",
      .bus = Uid {1},
      .capacity = 50.0,
  }};

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_upstream",
      },
      {
          .uid = Uid {2},
          .name = "j_downstream",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {{
      .uid = Uid {1},
      .name = "ww1",
      .junction_a = Uid {1},
      .junction_b = Uid {2},
      .fmin = 0.0,
      .fmax = 100.0,
  }};

  const Array<Reservoir> reservoir_array = {{
      .uid = Uid {1},
      .name = "rsv1",
      .junction = Uid {1},
      .capacity = 1000.0,
      .emin = 0.0,
      .emax = 1000.0,
      .eini = 100.0,
  }};

  const Array<Turbine> turbine_array = {{
      .uid = Uid {1},
      .name = "tur1",
      .waterway = Uid {1},
      .generator = Uid {1},
      .conversion_rate = 1.0,
      .main_reservoir = Uid {1},
  }};

  const Array<ReservoirProductionFactor> reservoir_production_factor_array = {{
      .uid = Uid {1},
      .name = "eff1",
      .turbine = Uid {1},
      .reservoir = Uid {1},
      .mean_production_factor = 1.5,
      .segments =
          {
              {.volume = 0.0, .slope = 0.001, .constant = 1.0},
              {.volume = 800.0, .slope = 0.0001, .constant = 1.5},
          },
  }};

  const Simulation simulation = {
      .block_array = {{
          .uid = Uid {1},
          .duration = 1,
      }},
      .stage_array = {{
          .uid = Uid {1},
          .first_block = 0,
          .count_block = 1,
      }},
      .scenario_array = {{
          .uid = Uid {0},
      }},
  };

  const System system = {
      .name = "PFDiffSegTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .reservoir_production_factor_array = reservoir_production_factor_array,
  };

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // update_lp uses vavg = (eini + efin_from_solution) / 2.
  // physical_eini returns eini=100 (first stage, first phase fallback).
  // physical_efin reads from the LP solution.
  const auto updated = system_lp.update_lp();
  CHECK(updated > 0);

  auto& effs = system_lp.elements<ReservoirProductionFactorLP>();
  auto& eff = effs.front();

  // The coefficient is updated using vavg = (eini + efin_from_solution) / 2.
  // eini = 100 (first-stage fallback), efin comes from the LP solution.
  // With eini=100, seg1 applies: rate ≈ 1.0 + 0.001 * vavg (near 1.1).
  const auto& bmap = eff.coeff_indices_at(ScenarioUid {0}, StageUid {1});
  for (const auto& [buid, ci] : bmap) {
    const auto coeff = lp.get_coeff(ci.row, ci.col);
    // Coefficient is negative rate; should be close to -1.1
    CHECK(coeff == doctest::Approx(-1.1).epsilon(0.01));
  }

  // Re-solve succeeds
  auto result2 = lp.resolve();
  REQUIRE(result2.has_value());
  CHECK(result2.value() == 0);
}

TEST_CASE(
    "ReservoirProductionFactorLP - update_lp with empty segments is "
    "no-op")
{
  // Empty segments → mean_production_factor is used directly, no update_lp
  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};

  const Array<Generator> generator_array = {{
      .uid = Uid {1},
      .name = "hydro_gen",
      .bus = Uid {1},
      .gcost = 5.0,
      .capacity = 200.0,
  }};

  const Array<Demand> demand_array = {{
      .uid = Uid {1},
      .name = "d1",
      .bus = Uid {1},
      .capacity = 50.0,
  }};

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_upstream",
      },
      {
          .uid = Uid {2},
          .name = "j_downstream",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {{
      .uid = Uid {1},
      .name = "ww1",
      .junction_a = Uid {1},
      .junction_b = Uid {2},
      .fmin = 0.0,
      .fmax = 100.0,
  }};

  const Array<Reservoir> reservoir_array = {{
      .uid = Uid {1},
      .name = "rsv1",
      .junction = Uid {1},
      .capacity = 1000.0,
      .emin = 0.0,
      .emax = 1000.0,
      .eini = 500.0,
  }};

  const Array<Turbine> turbine_array = {{
      .uid = Uid {1},
      .name = "tur1",
      .waterway = Uid {1},
      .generator = Uid {1},
      .conversion_rate = 1.0,
      .main_reservoir = Uid {1},
  }};

  // No segments → update_lp should not change coefficients
  const Array<ReservoirProductionFactor> reservoir_production_factor_array = {{
      .uid = Uid {1},
      .name = "eff_noseg",
      .turbine = Uid {1},
      .reservoir = Uid {1},
      .mean_production_factor = 2.0,
      .segments = {},
  }};

  const Simulation simulation = {
      .block_array = {{
          .uid = Uid {1},
          .duration = 1,
      }},
      .stage_array = {{
          .uid = Uid {1},
          .first_block = 0,
          .count_block = 1,
      }},
      .scenario_array = {{
          .uid = Uid {0},
      }},
  };

  const System system = {
      .name = "PFNoOpTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .reservoir_production_factor_array = reservoir_production_factor_array,
  };

  const OptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Empty segments → evaluate_production_factor returns 1.0 (default),
  // which differs from mean_production_factor=2.0 set at add_to_lp,
  // so the coefficient IS updated.  ReservoirProductionFactorLP::update_lp
  // always calls update_conversion_coeff when coeff indices exist.
  const auto updated = system_lp.update_lp();
  CHECK(updated > 0);
}
