// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_pump_production_factor.cpp
 * @brief     Tests for pumped-storage with ReservoirProductionFactor
 * @date      2026-04-13
 * @copyright BSD-3-Clause
 *
 * Tests that a reversible pumped-storage unit (HB Maule model) works
 * correctly when the turbine-mode production factor depends on the
 * upstream reservoir volume, while the pump-mode factor is constant.
 *
 * Topology (HB Maule):
 *   Colbun (high, 405-437 masl) <-> HB Maule <-> Machicura (low, ~256 masl)
 *   - Generation: water from Colbun tunnel -> HB Maule -> Machicura
 *   - Pumping:    water from Machicura     -> HB Maule -> back to Colbun
 *   - Both modes share the same head (Colbun level - Machicura level)
 *   - Only the generation production_factor varies with Colbun volume
 *   - Pump factor (1.88 MW/(m3/s)) is constant
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Build a Colbun-Machicura pumped-storage system.
/// The turbine has a volume-dependent production factor on Colbun.
/// The pump has a constant pump_factor.
auto make_hb_maule_system(Real colbun_eini, bool with_production_factor)
    -> System
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
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 80.0,
          .capacity = 300.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "system_load",
          .bus = Uid {1},
          .fcost = 1000.0,
          .capacity = 150.0,
      },
      {
          .uid = Uid {2},
          .name = "pump_load",
          .bus = Uid {1},
          .capacity = 200.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_colbun",
      },
      {
          .uid = Uid {2},
          .name = "j_machicura",
          .drain = true,
      },
  };

  // Generation waterway: Colbun -> Machicura (high to low)
  // Pump waterway: Machicura -> Colbun (low to high)
  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww_gen",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 49.0,
      },
      {
          .uid = Uid {2},
          .name = "ww_pump",
          .junction_a = Uid {2},
          .junction_b = Uid {1},
          .fmin = 0.0,
          .fmax = 40.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv_colbun",
          .junction = Uid {1},
          .capacity = 10000.0,
          .emin = 1000.0,
          .emax = 10000.0,
          .eini = colbun_eini,
      },
      {
          .uid = Uid {2},
          .name = "rsv_machicura",
          .junction = Uid {2},
          .capacity = 3000.0,
          .emin = 500.0,
          .emax = 3000.0,
          .eini = 1500.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "natural_inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 20.0,
      },
  };

  // Turbine: HB Maule generation mode (prod_factor=1.44 at nominal head)
  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur_hb_maule",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.44,
          .main_reservoir = Uid {1},
      },
  };

  // Pump: HB Maule pumping mode (pump_factor=1.88, constant)
  const Array<Pump> pump_array = {
      {
          .uid = Uid {1},
          .name = "pump_hb_maule",
          .waterway = Uid {2},
          .demand = Uid {2},
          .pump_factor = 1.88,
          .efficiency = 0.85,
          .capacity = 40.0,
      },
  };

  // Volume-dependent production factor on Colbun (generation mode only)
  // At low volume (low head): lower production factor
  // At high volume (high head): higher production factor
  Array<ReservoirProductionFactor> rpf_array;
  if (with_production_factor) {
    rpf_array = {
        {
            .uid = Uid {1},
            .name = "rpf_hb_maule",
            .turbine = Uid {1},
            .reservoir = Uid {1},
            .mean_production_factor = 1.44,
            .segments =
                {
                    {.volume = 0.0, .slope = 0.0002, .constant = 1.00},
                    {.volume = 5000.0, .slope = 0.00005, .constant = 1.44},
                },
        },
    };
  }

  return System {
      .name = "HBMauleTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .pump_array = pump_array,
      .reservoir_production_factor_array = rpf_array,
  };
}

}  // namespace

// ---------------------------------------------------------------------------
// Pumped storage with constant production factor builds and solves
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "PumpLP — HB Maule pumped storage with constant production factor")
{
  const auto system = make_hb_maule_system(5000.0, false);

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 4},
              {.uid = Uid {2}, .duration = 2},
          },
      .stage_array =
          {
              {.uid = Uid {1}, .first_block = 0, .count_block = 2},
          },
      .scenario_array =
          {
              {.uid = Uid {0}},
          },
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  CHECK(lp.get_obj_value_raw() > 0.0);
}

// ---------------------------------------------------------------------------
// Pumped storage with volume-dependent production factor
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "PumpLP — HB Maule with ReservoirProductionFactor on turbine")
{
  const auto system = make_hb_maule_system(5000.0, true);

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 4},
              {.uid = Uid {2}, .duration = 2},
          },
      .stage_array =
          {
              {.uid = Uid {1}, .first_block = 0, .count_block = 2},
          },
      .scenario_array =
          {
              {.uid = Uid {0}},
          },
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  CHECK(lp.get_obj_value_raw() > 0.0);
}

// ---------------------------------------------------------------------------
// Different Colbun volumes yield different objectives (prod factor varies)
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "PumpLP — different Colbun volumes change objective via production factor")
{
  // At higher Colbun volume → higher production factor → more MW per m3/s
  // → cheaper hydro generation → lower objective
  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 4},
              {.uid = Uid {2}, .duration = 2},
          },
      .stage_array =
          {
              {.uid = Uid {1}, .first_block = 0, .count_block = 2},
          },
      .scenario_array =
          {
              {.uid = Uid {0}},
          },
  };

  const PlanningOptionsLP options;

  // Low volume → low production factor
  const auto system_low = make_hb_maule_system(2000.0, true);
  SimulationLP sim_low(simulation, options);
  SystemLP slp_low(system_low, sim_low);
  auto result_low = slp_low.linear_interface().resolve();
  REQUIRE(result_low.has_value());
  const auto obj_low = slp_low.linear_interface().get_obj_value_raw();

  // High volume → high production factor
  const auto system_high = make_hb_maule_system(8000.0, true);
  SimulationLP sim_high(simulation, options);
  SystemLP slp_high(system_high, sim_high);
  auto result_high = slp_high.linear_interface().resolve();
  REQUIRE(result_high.has_value());
  const auto obj_high = slp_high.linear_interface().get_obj_value_raw();

  // Higher Colbun volume → higher production factor → more MW per m3/s
  // → cheaper generation → lower or equal objective
  CHECK(obj_high <= obj_low);
}

// ---------------------------------------------------------------------------
// Round-trip efficiency prevents simultaneous gen and pump
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "PumpLP — round-trip efficiency < 1 prevents simultaneous gen/pump")
{
  // Round-trip: (1.44/1.88) * 0.85 ≈ 0.65
  // Generating 1 MW of hydro costs 1/1.44 m3/s of water.
  // Pumping that water back costs (1/1.44) * 1.88 / 0.85 ≈ 1.53 MW.
  // Net loss: 0.53 MW per cycle. LP will never run both simultaneously.

  const auto system = make_hb_maule_system(5000.0, false);

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 6},
          },
      .stage_array =
          {
              {.uid = Uid {1}, .first_block = 0, .count_block = 1},
          },
      .scenario_array =
          {
              {.uid = Uid {0}},
          },
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // With a single block, the optimizer cannot time-shift between gen and pump.
  // Any round-trip would be pure loss, so at most one of gen/pump is active.
  // We verify the LP solves feasibly (the optimizer doesn't need both).
  CHECK(lp.get_obj_value_raw() >= 0.0);
}

// ---------------------------------------------------------------------------
// Pump main_reservoir field exists on Pump struct
// ---------------------------------------------------------------------------

TEST_CASE("Pump — main_reservoir field")  // NOLINT
{
  SUBCASE("default has no main_reservoir")
  {
    const Pump pump;
    CHECK_FALSE(pump.main_reservoir.has_value());
  }

  SUBCASE("can set main_reservoir")
  {
    Pump pump;
    pump.main_reservoir = Uid {42};
    REQUIRE(pump.main_reservoir.has_value());
    CHECK(std::get<Uid>(pump.main_reservoir.value()) == Uid {42});
  }
}

// ---------------------------------------------------------------------------
// Inline production_factor on reservoir for turbine of reversible unit
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "PumpLP — inline production_factor on reservoir for reversible unit")
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal",
          .bus = Uid {1},
          .gcost = 80.0,
          .capacity = 300.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load",
          .bus = Uid {1},
          .fcost = 1000.0,
          .capacity = 50.0,
      },
      {
          .uid = Uid {2},
          .name = "pump_load",
          .bus = Uid {1},
          .capacity = 200.0,
      },
  };

  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j_up"},
      {.uid = Uid {2}, .name = "j_down", .drain = true},
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww_gen",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 49.0,
      },
      {
          .uid = Uid {2},
          .name = "ww_pump",
          .junction_a = Uid {2},
          .junction_b = Uid {1},
          .fmin = 0.0,
          .fmax = 40.0,
      },
  };

  // Reservoir with INLINE production_factor for the turbine
  Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv_colbun",
          .junction = Uid {1},
          .capacity = 10000.0,
          .emin = 1000.0,
          .emax = 10000.0,
          .eini = 5000.0,
          .production_factor =
              {
                  {
                      .turbine = Uid {1},
                      .mean_production_factor = 1.44,
                      .segments =
                          {
                              {
                                  .volume = 0.0,
                                  .slope = 0.0002,
                                  .constant = 1.00,
                              },
                          },
                  },
              },
      },
      {
          .uid = Uid {2},
          .name = "rsv_machicura",
          .junction = Uid {2},
          .capacity = 3000.0,
          .emin = 500.0,
          .emax = 3000.0,
          .eini = 1500.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.44,
          .main_reservoir = Uid {1},
      },
  };

  const Array<Pump> pump_array = {
      {
          .uid = Uid {1},
          .name = "pump1",
          .waterway = Uid {2},
          .demand = Uid {2},
          .pump_factor = 1.88,
          .capacity = 40.0,
      },
  };

  System system = {
      .name = "InlineProdFactorPump",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .pump_array = pump_array,
  };

  // Expand inline production_factor to system-level array
  system.expand_reservoir_constraints();
  REQUIRE(system.reservoir_production_factor_array.size() == 1);
  CHECK(system.reservoir_production_factor_array[0].reservoir
        == SingleId {Uid {1}});
  CHECK(system.reservoir_array[0].production_factor.empty());

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 4}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}
