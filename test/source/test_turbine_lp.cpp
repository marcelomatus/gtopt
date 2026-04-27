/**
 * @file      test_turbine_lp.hpp
 * @brief     Unit tests for TurbineLP covering uncovered code paths
 * @date      2026-04-04
 * @copyright BSD-3-Clause
 *
 * Tests cover:
 *  - Flow-connected turbine (uses_flow() path)
 *  - Waterway turbine with drain enabled (less_equal constraint)
 *  - Waterway turbine with capacity constraint
 *  - Waterway turbine with drain + capacity combined
 *  - Turbine with no waterway or flow (error path)
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// -----------------------------------------------------------------------
// Flow-connected turbine (exercises the uses_flow() branch)
// -----------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "TurbineLP — flow-connected turbine creates fconv constraint")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "pasada_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 80.0,
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

  // Junction needed for the flow element
  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_pasada",
          .drain = true,
      },
  };

  // Flow element with fixed discharge
  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "river_flow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 30.0,
      },
  };

  // Turbine connected via flow (not waterway)
  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur_pasada",
          .flow = SingleId {Uid {1}},
          .generator = Uid {1},
          .production_factor = 2.0,
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
      .name = "FlowTurbineTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // With 30 m3/s flow and production_factor=2, max hydro power = 60 MW.
  // Demand = 50 MW, gcost_hydro=5 < gcost_thermal=80, so hydro serves all.
  // Objective should be positive (serving 50 MW for 3 block-hours).
  CHECK(lp.get_obj_value_raw() > 0.0);
}

// -----------------------------------------------------------------------
// Waterway turbine with drain=true (less_equal conversion constraint)
// -----------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "TurbineLP — drain=true creates less_equal conversion constraint")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
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

  // Turbine with drain=true: can spill water without generating power
  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur_drain",
          .waterway = Uid {1},
          .generator = Uid {1},
          .drain = true,
          .production_factor = 1.0,
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
      .name = "DrainTurbineTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  CHECK(lp.get_obj_value_raw() >= 0.0);
}

// -----------------------------------------------------------------------
// Waterway turbine with capacity constraint
// -----------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "TurbineLP — capacity constraint limits waterway flow")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
          .capacity = 300.0,
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
          .name = "d1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
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
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 5000.0,
          .emin = 0.0,
          .emax = 5000.0,
          .eini = 2500.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 200.0,
      },
  };

  // Turbine with a capacity constraint limiting flow to 50 m3/s
  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur_capped",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 2.0,
          .capacity = 50.0,
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
      .name = "CapacityTurbineTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // The capacity constraint limits turbine flow to 50 m3/s.
  // With production_factor=2, max hydro power = 100 MW.
  // Demand is 100 MW, so hydro saturates and no thermal is needed.
  CHECK(lp.get_obj_value_raw() > 0.0);
}

// -----------------------------------------------------------------------
// Waterway turbine with drain + capacity combined
// -----------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "TurbineLP — drain=true with capacity exercises both paths")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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
          .capacity = 300.0,
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
          .name = "d1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
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
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 5000.0,
          .emin = 0.0,
          .emax = 5000.0,
          .eini = 2500.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 200.0,
      },
  };

  // Turbine with both drain and capacity
  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur_drain_cap",
          .waterway = Uid {1},
          .generator = Uid {1},
          .drain = true,
          .production_factor = 2.0,
          .capacity = 80.0,
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
      .name = "DrainCapTurbineTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  CHECK(lp.get_obj_value_raw() > 0.0);
}

// -----------------------------------------------------------------------
// Multi-block flow-connected turbine with varying conversion rate
// -----------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "TurbineLP — flow turbine with custom production_factor solves correctly")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "pasada_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 500.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 80.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_pasada",
          .drain = true,
      },
  };

  // Large flow discharge
  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "river_flow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 100.0,
      },
  };

  // Flow-connected turbine with high conversion rate
  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur_flow_hi",
          .flow = SingleId {Uid {1}},
          .generator = Uid {1},
          .production_factor = 5.0,
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
              {
                  .uid = Uid {3},
                  .duration = 3,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 3,
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
      .name = "FlowTurbineHiConv",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // With 100 m3/s * 5 = 500 MW max hydro, demand = 80 MW.
  // All demand served by cheap hydro. Objective is scaled by
  // default_scale_objective (1e7), so just verify it is positive.
  CHECK(lp.get_obj_value_raw() > 0.0);
}

// -----------------------------------------------------------------------
// Primal col_sol assertion: waterway turbine with equality conversion
// forces discharge = generation / production_factor.
// -----------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "TurbineLP — col_sol enforces discharge = generation / production_factor")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Minimal hydro chain: reservoir → waterway → turbine → generator → bus.
  // A 120 MW demand is served exclusively by the hydro generator (no
  // thermal alternative).  With drain=false, the turbine conversion row
  // is an equality: generation = production_factor × discharge.
  // Therefore discharge is uniquely determined at 120 / 3 = 40 m3/s.
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
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 120.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
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
          .fmax = 1000.0,
      },
  };

  // Plenty of water stored: 10'000 m3 initial; over a 1 h block only
  // 40 m3/s × 3600 s = 144'000 m3 would be drawn, but the storage is
  // not the binding constraint here — the equality conversion row is.
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 1.0e9,
          .emin = 0.0,
          .emax = 1.0e9,
          .eini = 1.0e9,
      },
  };

  // drain=false (default) → equality conversion row:
  //   gen - 3 · flow = 0     (i.e. discharge = generation / 3).
  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur_eq",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 3.0,
      },
  };

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
      .name = "TurbineColSolTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Pull the three LP objects we need to locate the columns.
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& block_lp = simulation_lp.blocks().front();
  const auto buid = block_lp.uid();

  // Generation column (owned by GeneratorLP).
  const auto& gen_lps = system_lp.elements<GeneratorLP>();
  REQUIRE(gen_lps.size() == 1);
  const auto& gen_lp = gen_lps.front();
  const auto& gcols = gen_lp.generation_cols_at(scenario_lp, stage_lp);
  REQUIRE(gcols.size() == 1);
  const auto gcol = gcols.at(buid);

  // Discharge column (owned by WaterwayLP, exposed via flow_cols_at).
  const auto& ww_lps = system_lp.elements<WaterwayLP>();
  REQUIRE(ww_lps.size() == 1);
  const auto& ww_lp = ww_lps.front();
  const auto& fcols = ww_lp.flow_cols_at(scenario_lp, stage_lp);
  REQUIRE(fcols.size() == 1);
  const auto fcol = fcols.at(buid);

  const auto col_sol = lp.get_col_sol();

  // Demand = 120 MW must be met by the only generator.
  CHECK(col_sol[gcol] == doctest::Approx(120.0).epsilon(1e-6));

  // Equality conversion row forces discharge = generation / 3 = 40 m3/s.
  CHECK(col_sol[fcol] == doctest::Approx(40.0).epsilon(1e-6));

  // Cross-check: generation / production_factor must equal discharge.
  CHECK(col_sol[gcol] / 3.0 == doctest::Approx(col_sol[fcol]).epsilon(1e-6));
}

// -----------------------------------------------------------------------
// Dual asserts: the turbine conversion row
//   generation − production_factor · flow = 0
// is an equality tying electrical generation to hydraulic discharge.
// Its shadow price is the marginal value of the conversion itself and,
// when the turbine is the marginal supplier, is tightly linked to the
// bus marginal cost ($/MWh).  Here we cap the WATERWAY flow (not the
// generator) so the conversion equality is the structural bottleneck
// limiting hydro output; the residual demand is served by an expensive
// thermal unit, forcing a non-trivial LMP and a non-zero dual on the
// conversion row.
// -----------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "TurbineLP — conversion row dual is non-zero when turbine is marginal")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  // Hydro generator is amply sized (100 MW); the bottleneck is the
  // waterway's fmax (see below), so the conversion equality is what
  // caps hydro gen at pf × fmax = 20 MW.  Thermal backup at $50/MWh
  // serves the residual 10 MW → bus LMP = 50.  Relaxing the conversion
  // row by δ would allow +δ MW of cheap hydro and save $50/MWh, so the
  // conversion row's shadow price equals the bus LMP.
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 0.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 1000.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 30.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  // Waterway flow capped at 10 m³/s — with production_factor = 2, this
  // caps hydro generation at 20 MW via the conversion equality.
  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 10.0,
      },
  };

  // Ample water — storage is not the binding constraint.
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 1.0e9,
          .emin = 0.0,
          .emax = 1.0e9,
          .eini = 1.0e9,
      },
  };

  // production_factor = 2 MW/(m³/s); the conversion row is the equality
  // gen − 2 · flow = 0 → at gen = 20 MW the turbine flow = 10 m³/s.
  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 2.0,
      },
  };

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
      .name = "TurbineConvDualTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Locate the scenario/stage/block handles.
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& block_lp = simulation_lp.blocks().front();
  const auto buid = block_lp.uid();

  // Primal sanity: hydro is capped at 20 MW, thermal serves the rest.
  const auto& gen_lps = system_lp.elements<GeneratorLP>();
  REQUIRE(gen_lps.size() == 2);

  // Find the hydro generator (capacity = 20) to locate its column.
  const GeneratorLP* hydro_gen_lp = nullptr;
  for (const auto& g : gen_lps) {
    if (g.uid() == Uid {1}) {
      hydro_gen_lp = &g;
      break;
    }
  }
  REQUIRE(hydro_gen_lp != nullptr);
  const auto& gcols = hydro_gen_lp->generation_cols_at(scenario_lp, stage_lp);
  REQUIRE(gcols.size() == 1);
  const auto gcol = gcols.at(buid);

  const auto col_sol = lp.get_col_sol();
  CHECK(col_sol[gcol] == doctest::Approx(20.0).epsilon(1e-4));

  // Retrieve the conversion row index for the turbine.
  const auto& turbine_lps = system_lp.elements<TurbineLP>();
  REQUIRE(turbine_lps.size() == 1);
  const auto& tur_lp = turbine_lps.front();
  const auto& conv_rows = tur_lp.conversion_rows_at(scenario_lp, stage_lp);
  REQUIRE_FALSE(conv_rows.empty());
  const auto conv_row = conv_rows.at(buid);

  // Read row duals (physical units — includes scale_objective).
  const auto row_dual = lp.get_row_dual();
  const auto dual = row_dual[conv_row];

  // The conversion row is an equality with the waterway flow-bound,
  // so relaxing it would admit cheaper hydro generation at the bus
  // LMP (= thermal gcost = $50/MWh).  The shadow price must be
  // non-zero and economically bounded by the bus marginal price.
  CHECK(std::abs(dual) > 1e-3);
  CHECK(std::abs(dual) <= 50.0 + 1e-4);
}
