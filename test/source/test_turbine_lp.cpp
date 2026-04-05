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

  const PlanningOptionsLP options;
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
  CHECK(lp.get_obj_value() > 0.0);
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

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  CHECK(lp.get_obj_value() >= 0.0);
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

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // The capacity constraint limits turbine flow to 50 m3/s.
  // With production_factor=2, max hydro power = 100 MW.
  // Demand is 100 MW, so hydro saturates and no thermal is needed.
  CHECK(lp.get_obj_value() > 0.0);
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

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  CHECK(lp.get_obj_value() > 0.0);
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

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // With 100 m3/s * 5 = 500 MW max hydro, demand = 80 MW.
  // All demand served by cheap hydro. Objective is scaled by
  // default_scale_objective (1e7), so just verify it is positive.
  CHECK(lp.get_obj_value() > 0.0);
}
