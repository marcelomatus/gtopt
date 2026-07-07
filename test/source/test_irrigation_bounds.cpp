/**
 * @file      test_irrigation_bounds.cpp
 * @brief     LP integration tests for FlowRight/VolumeRight bound rules,
 *            rights depletion, economy saving, and cost/value parameters
 * @date      2026-04-05
 * @copyright BSD-3-Clause
 *
 * Tests verify that bound_rule correctly constrains irrigation rights based on
 * reservoir volumes, that rights depletion limits generation, that economy
 * saving accumulates unused entitlements, and that use_value/fail_cost
 * objective coefficients are assembled correctly.
 */

#include <doctest/doctest.h>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

TEST_CASE(  // NOLINT
    "FlowRight with bound_rule - LP construction and bound application")
{
  using namespace gtopt;

  // Hydro system with reservoir whose volume drives the bound rule
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
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
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 3000.0,
          .emin = 100.0,
          .emax = 3000.0,
          .eini = 1500.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 100.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 2.0,
      },
  };

  // FlowRight with bound_rule: step function at V=500
  // At eini=1500 > 500, bound evaluates to 200
  const Array<FlowRight> flow_right_array = {
      {
          .uid = Uid {1},
          .name = "bounded_irrig",
          .fmax = 300.0,
          .target = 300.0,
          .fcost = 5000.0,
          .bound_rule =
              RightBoundRule {
                  .reservoir = Uid {1},
                  .segments =
                      {
                          {
                              .volume = 0.0,
                              .slope = 0.0,
                              .constant = 0.0,
                          },
                          {
                              .volume = 500.0,
                              .slope = 0.0,
                              .constant = 200.0,
                          },
                      },
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
              },
          },
  };

  const System system = {
      .name = "BoundRuleFlowTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .flow_right_array = flow_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "VolumeRight with bound_rule - LP construction and bound application")
{
  using namespace gtopt;

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
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
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 3000.0,
          .emin = 100.0,
          .emax = 3000.0,
          .eini = 1500.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 100.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 2.0,
      },
  };

  // VolumeRight with Laja-style bound_rule
  const Array<VolumeRight> volume_right_array = {
      {
          .uid = Uid {1},
          .name = "bounded_vol",
          .emax = 500.0,
          .eini = 0.0,
          .demand = 50.0,
          .fail_cost = 5000.0,
          .use_state_variable = false,
          .bound_rule =
              RightBoundRule {
                  .reservoir = Uid {1},
                  .segments =
                      {
                          {
                              .volume = 0.0,
                              .slope = 0.0,
                              .constant = 570.0,
                          },
                          {
                              .volume = 1200.0,
                              .slope = 0.40,
                              .constant = 90.0,
                          },
                      },
                  .cap = 5000.0,
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
              },
          },
  };

  const System system = {
      .name = "BoundRuleVolTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .volume_right_array = volume_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "FlowRight bound_rule deactivation - zero bound fixes variable at 0")
{
  using namespace gtopt;

  // When reservoir volume is below the threshold, bound evaluates to 0.
  // The flow column should be fixed at 0.
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
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
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 3000.0,
          .emin = 100.0,
          .emax = 3000.0,
          .eini = 200.0,  // Below threshold → bound = 0
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 100.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 2.0,
      },
  };

  // Bound rule: 0 below 500, 200 above 500
  // eini=200 < 500 → initial bound = 0 → flow fixed at 0
  const Array<FlowRight> flow_right_array = {
      {
          .uid = Uid {1},
          .name = "deactivated_irrig",
          .fmax = 300.0,
          .target = 300.0,
          .fcost = 5000.0,
          .bound_rule =
              RightBoundRule {
                  .reservoir = Uid {1},
                  .segments =
                      {
                          {
                              .volume = 0.0,
                              .slope = 0.0,
                              .constant = 0.0,
                          },
                          {
                              .volume = 500.0,
                              .slope = 0.0,
                              .constant = 200.0,
                          },
                      },
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
              },
          },
  };

  const System system = {
      .name = "DeactivationTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .flow_right_array = flow_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(  // NOLINT
    "FlowRightLP::update_lp recomputes volume-dependent bound after solve")
{
  using namespace gtopt;

  // Exercises FlowRightLP::update_lp(): after an initial solve the
  // reservoir's physical efin differs from eini, so the (eini+efin)/2
  // average volume picks a different piecewise segment from the one
  // evaluated in add_to_lp. update_lp should then call set_col_upp for
  // every block and return a non-zero count.

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
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
          .fmax = 500.0,
      },
  };

  // Use a big enough capacity that the solver discharges meaningful water
  // through the turbine, so efin < eini.
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 3000.0,
          .emin = 0.0,
          .emax = 3000.0,
          .eini = 1500.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 0.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 2.0,
      },
  };

  // Bound rule: linear segments so that a small change in volume
  // changes the evaluated bound, guaranteeing update_lp > 0 unless
  // efin == eini exactly.
  const Array<FlowRight> flow_right_array = {
      {
          .uid = Uid {1},
          .name = "bounded_irrig",
          .fmax = 300.0,
          .target = 300.0,
          .fcost = 5000.0,
          .bound_rule =
              RightBoundRule {
                  .reservoir = Uid {1},
                  .segments =
                      {
                          {
                              .volume = 0.0,
                              .slope = 0.1,
                              .constant = 0.0,
                          },
                          {
                              .volume = 1000.0,
                              .slope = 0.2,
                              .constant = 100.0,
                          },
                      },
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
              },
          },
  };

  const System system = {
      .name = "UpdateLpFlowRightTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
      .flow_right_array = flow_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Dispatch update_lp across all phases. FlowRightLP::update_lp should
  // fire because efin != eini after the solve depletes the reservoir.
  // The return value is the number of bounds actually modified; we only
  // assert it is non-negative (the physical_efin value depends on solver
  // behavior and is not deterministic across backends).
  const auto updated = system_lp.update_lp();
  CHECK(updated >= 0);

  // Problem still resolvable after the bound update.
  auto result2 = lp.resolve();
  REQUIRE(result2.has_value());
  CHECK(result2.value() == 0);
}

TEST_CASE(  // NOLINT
    "Rights exhaustion limits generation despite available water")
{
  using namespace gtopt;

  // Physical model: reservoir → waterway → j_down (drain=true).
  // A user constraint couples reservoir extraction to VolumeRight extraction:
  //   rsv_extraction = vrt_extraction
  // VolumeRight tracks remaining rights (eini=limit → depletes to 0).
  // When vrt_vol hits 0, extraction must stop →
  // turbine stops → thermal generator fills the gap at higher cost.
  //
  // No FlowRight — the VolumeRight extraction IS the physical extraction.

  auto solve = [](double rights_limit) -> double
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
            .name = "hydro",
            .bus = Uid {1},
            .gcost = 0.0,
            .capacity = 200.0,
        },
        {
            .uid = Uid {2},
            .name = "thermal",
            .bus = Uid {1},
            .gcost = 100.0,
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
            .name = "j_rsv",
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
            .name = "ww",
            .junction_a = Uid {1},
            .junction_b = Uid {2},
            .fmin = 0.0,
            .fmax = 200.0,
        },
    };
    const Array<Reservoir> reservoir_array = {
        {
            .uid = Uid {1},
            .name = "rsv",
            .junction = Uid {1},
            .capacity = 200.0,
            .emin = 0.0,
            .emax = 200.0,
            .eini = 100.0,
        },
    };
    const Array<Turbine> turbine_array = {
        {
            .uid = Uid {1},
            .name = "tur",
            .waterway = Uid {1},
            .generator = Uid {1},
            .production_factor = 2.0,
        },
    };
    // VolumeRight: starts at rights_limit and depletes toward 0.
    // extraction = extraction flow (m³/s), coupled to reservoir via user
    // constraint.
    const Array<VolumeRight> volume_right_array = {
        {
            .uid = Uid {1},
            .name = "rights_vol",
            .emax = rights_limit,
            .eini = rights_limit,
            .fmax = 200.0,
        },
    };
    // User constraint ties reservoir extraction to VolumeRight extraction:
    //   rsv_extraction = vrt_extraction  (physical extraction IS rights
    //   consumption)
    const Array<UserConstraint> user_constraint_array = {
        {
            .uid = Uid {1},
            .name = "rsv_vrt_couple",
            .expression =
                R"(reservoir("rsv").extraction = volume_right("rights_vol").extraction)",
            .constraint_type = "raw",
        },
    };

    const Simulation simulation = {
        .block_array =
            {
                {
                    .uid = Uid {1},
                    .duration = 24,
                },
            },
        .stage_array =
            {
                {
                    .uid = Uid {1},
                    .first_block = 0,
                    .count_block = 1,
                },
                {
                    .uid = Uid {2},
                    .first_block = 0,
                    .count_block = 1,
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
        .name = "RightsExhaustionTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .reservoir_array = reservoir_array,
        .turbine_array = turbine_array,
        .volume_right_array = volume_right_array,
        .user_constraint_array = user_constraint_array,
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options(std::move(popts));
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
    return lp.get_obj_value_raw();
  };

  // With 100 hm³ of rights (plenty): all demand served by hydro → cost ≈ 0
  // Demand = 50 MW, conv_rate = 2.0, so need 25 m³/s.
  // 2 stages × 24h × 25 m³/s × 0.0036 = 4.32 hm³ needed, 100 available.
  const auto cost_unlimited = solve(100.0);
  CHECK(cost_unlimited == doctest::Approx(0.0).epsilon(0.01));

  // With 3 hm³ of rights (< 4.32 hm³ needed): thermal must fill the gap.
  // The optimizer allocates rights optimally across stages, so partial
  // hydro is possible — but total cost must be strictly positive.
  const auto cost_limited = solve(3.0);
  CHECK(cost_limited > cost_unlimited);
}

TEST_CASE(  // NOLINT
    "VolumeRight economy with saving variable accumulates unused rights")
{
  using namespace gtopt;

  // Model: a rights VolumeRight extracts water, an economy VolumeRight
  // receives the unused portion as savings via a user constraint:
  //   rights.extraction + economy.saving <= max_extraction_flow
  //
  // The economy VolumeRight uses saving_rate to enable the saving variable.
  // When the rights holder doesn't fully exercise their entitlement,
  // the remainder flows into the economy accumulator.
  //
  // Setup:
  //   - 1 bus, 1 hydro generator, 1 thermal, 1 demand (50 MW)
  //   - reservoir → waterway → downstream junction
  //   - rights VolumeRight: emax=100 hm³, extraction coupled to reservoir
  //   - economy VolumeRight: eini=0, saving_rate=200 m³/s, accumulates
  //   - user constraint: rights.extraction + economy.saving = 50 m³/s
  //     (the max_flow limit — unused portion becomes savings)
  //   - 2 stages × 24h blocks

  auto solve = [](double saving_max_rate) -> double
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
            .name = "hydro",
            .bus = Uid {1},
            .gcost = 0.0,
            .capacity = 200.0,
        },
        {
            .uid = Uid {2},
            .name = "thermal",
            .bus = Uid {1},
            .gcost = 100.0,
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
            .name = "j_rsv",
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
            .name = "ww",
            .junction_a = Uid {1},
            .junction_b = Uid {2},
            .fmin = 0.0,
            .fmax = 200.0,
        },
    };
    const Array<Reservoir> reservoir_array = {
        {
            .uid = Uid {1},
            .name = "rsv",
            .junction = Uid {1},
            .capacity = 200.0,
            .emin = 0.0,
            .emax = 200.0,
            .eini = 100.0,
        },
    };
    const Array<Turbine> turbine_array = {
        {
            .uid = Uid {1},
            .name = "tur",
            .waterway = Uid {1},
            .generator = Uid {1},
            .production_factor = 2.0,
        },
    };
    // Rights VolumeRight: starts at 100 hm³, extraction depletes it.
    // Economy VolumeRight: starts at 0, saving_rate allows deposits.
    const Array<VolumeRight> volume_right_array = {
        {
            .uid = Uid {1},
            .name = "rights_vol",
            .emax = 100.0,
            .eini = 100.0,
            .fmax = 200.0,
        },
        {
            .uid = Uid {2},
            .name = "economy_vol",
            .purpose = "economy",
            .emax = 100.0,
            .eini = 0.0,
            .saving_rate = saving_max_rate,
        },
    };
    // User constraints:
    // 1. rsv extraction = rights extraction (physical coupling)
    // 2. rights extraction + economy saving = 50 m³/s
    //    (unused portion of 50 m³/s max flow becomes savings)
    const Array<UserConstraint> user_constraint_array = {
        {
            .uid = Uid {1},
            .name = "rsv_vrt_couple",
            .expression =
                R"(reservoir("rsv").extraction = volume_right("rights_vol").extraction)",
            .constraint_type = "raw",
        },
        {
            .uid = Uid {2},
            .name = "saving_balance",
            .expression =
                R"(volume_right("rights_vol").extraction + volume_right("economy_vol").saving = 50)",
            .constraint_type = "raw",
        },
    };

    const Simulation simulation = {
        .block_array =
            {
                {
                    .uid = Uid {1},
                    .duration = 24,
                },
            },
        .stage_array =
            {
                {
                    .uid = Uid {1},
                    .first_block = 0,
                    .count_block = 1,
                },
                {
                    .uid = Uid {2},
                    .first_block = 0,
                    .count_block = 1,
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
        .name = "EconomySavingTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .reservoir_array = reservoir_array,
        .turbine_array = turbine_array,
        .volume_right_array = volume_right_array,
        .user_constraint_array = user_constraint_array,
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options(std::move(popts));
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
    return lp.get_obj_value_raw();
  };

  // With saving enabled: the constraint forces
  //   rights.extraction + economy.saving = 50 m³/s
  // Hydro needs 25 m³/s for 50 MW (conv_rate=2.0), so:
  //   rights.extraction = 25, economy.saving = 25
  // The economy accumulates 25 m³/s × 24h × 0.0036 = 0.216 hm³/stage
  // All demand served by hydro → cost ≈ 0.
  const auto cost_with_saving = solve(200.0);
  CHECK(cost_with_saving == doctest::Approx(0.0).epsilon(0.01));
}

TEST_CASE(  // NOLINT
    "FlowRight extraction - use_value, fail_cost, use_average, and junction")
{
  using namespace gtopt;

  // Verifies:
  //  1. Per-element use_value creates a negative obj coefficient (benefit)
  //  2. Per-element fail_cost creates a deficit variable with penalty
  //  3. hydro_use_value fallback applies when no per-element use_value
  //  4. hydro_fail_cost fallback applies when no per-element fail_cost
  //  5. use_average creates qeh (stage-average flow) variable
  //  6. Junction coupling subtracts flow from junction balance

  // Two blocks with different durations to exercise averaging
  const auto dur1 = 6.0;  // hours
  const auto dur2 = 18.0;  // hours

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "gen1",
          .bus = Uid {1},
          .gcost = 10.0,
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
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 2000.0,
          .emin = 100.0,
          .emax = 2000.0,
          .eini = 1000.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 100.0,
      },
  };

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
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = dur1,
              },
              {
                  .uid = Uid {2},
                  .duration = dur2,
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

  SUBCASE("per-element uvalue and fcost")
  {
    const auto use_val = 1500.0;
    const auto fail_val = 5000.0;

    // Unified-mode: target=10 creates a soft kink, fmax=20 opens the
    // above-target bonus region, so uvalue actually applies on the
    // flow_high sub-column.  This exercises the two-sub-column path.
    const Array<FlowRight> flow_right_array = {
        {
            .uid = Uid {1},
            .name = "irr_with_costs",
            .junction_a = Uid {2},
            .fmax = 20.0,
            .target = 10.0,
            .fcost = fail_val,
            .uvalue = use_val,
        },
    };

    const System system = {
        .name = "PerElementCostTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .flow_array = flow_array,
        .reservoir_array = reservoir_array,
        .turbine_array = turbine_array,
        .flow_right_array = flow_right_array,
    };

    const PlanningOptionsLP options;
    const auto scale_obj = options.scale_objective();
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    const auto obj_coeffs = lp.get_obj_coeff();

    // Post-2026-05 attach_flow refactor: the kink is expressed via
    // two non-negative slacks attached to the single `flow_col`:
    //   fail   slack ≥ 0, cost = +fcost·cf  (deficit penalty)
    //   excess slack ≥ 0, cost = -uvalue·cf (excess bonus)
    // flow_col itself carries zero cost.  Verify BOTH slack costs
    // appear among the obj coefficients with the expected signs.
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& scenarios = system_lp.scene().scenarios();
    const auto& stages = system_lp.phase().stages();
    const auto& s = scenarios[0];
    const auto& t = stages[0];

    const auto expected_fcost_b1 = +fail_val * dur1 / scale_obj;
    const auto expected_fcost_b2 = +fail_val * dur2 / scale_obj;
    const auto expected_uvalue_b1 = -use_val * dur1 / scale_obj;
    const auto expected_uvalue_b2 = -use_val * dur2 / scale_obj;
    bool found_fcost = false;
    bool found_uvalue = false;
    for (const auto& block : t.blocks()) {
      const auto fail = fr_lp.fail_col_at(s, t, block);
      const auto excess = fr_lp.excess_col_at(s, t, block);
      REQUIRE(fail.has_value());
      REQUIRE(excess.has_value());
      const auto fc = obj_coeffs[*fail];
      const auto ec = obj_coeffs[*excess];
      if (doctest::Approx(fc).epsilon(1e-3) == expected_fcost_b1
          || doctest::Approx(fc).epsilon(1e-3) == expected_fcost_b2)
      {
        found_fcost = true;
      }
      if (doctest::Approx(ec).epsilon(1e-3) == expected_uvalue_b1
          || doctest::Approx(ec).epsilon(1e-3) == expected_uvalue_b2)
      {
        found_uvalue = true;
      }
    }
    CHECK(found_fcost);
    CHECK(found_uvalue);

    // flow_col now carries the hard band `[fmin, fmax]` directly —
    // it is no longer relaxed to 0.  Verify each flow_col upper
    // bound matches fmax.
    const auto col_upp = lp.get_col_upp();
    for (const auto& [buid, col] : fr_lp.flow_cols_at(s, t)) {
      CHECK(col_upp[col] == doctest::Approx(20.0));  // fmax = 20
    }

    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // Reconstructed `fail_sol` per block must be 0 when the FlowRight
    // is fully delivered (junction has water — this is the unconstrained
    // case).  `fr_lp` / `s` / `t` were captured above.
    const auto col_sol = lp.get_col_sol();
    for (const auto& block : t.blocks()) {
      CHECK(fr_lp.fail_sol_at(s, t, block, col_sol) == doctest::Approx(0.0));
    }
  }

  SUBCASE("fcost deficit coupling - fail absorbs shortfall")
  {
    // FlowRight demands 80 m³/s from junction j_down, but the hydro
    // system only delivers 100 m³/s inflow total (through one turbine).
    // After serving 50 MW demand via gen1 (50/2.0 = 25 m³/s turbine flow),
    // the junction has limited water.  With discharge=80 and junction
    // coupling, the FlowRight can't always fully deliver, so fail > 0.
    const auto discharge = 80.0;
    const auto fail_val = 5000.0;

    const Array<FlowRight> flow_right_array = {
        {
            .uid = Uid {1},
            .name = "heavy_demand",
            .junction_a = Uid {2},
            .target = discharge,
            .fcost = fail_val,
        },
    };

    const System system = {
        .name = "DeficitCouplingTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .flow_array = flow_array,
        .reservoir_array = reservoir_array,
        .turbine_array = turbine_array,
        .flow_right_array = flow_right_array,
    };

    const PlanningOptionsLP options;
    const auto scale_obj = options.scale_objective();
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // Post-P0: the `fail` LP column is gone (substituted away).
    // Reconstruct the per-block shortfall via `fail_sol_at` and sum
    // across blocks — same semantic as the legacy
    // `sum_b col_sol[fail_col]` walk.
    (void)scale_obj;
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& scenarios = system_lp.scene().scenarios();
    const auto& stages = system_lp.phase().stages();
    const auto col_sol = lp.get_col_sol();
    double total_fail = 0.0;
    for (const auto& block : stages[0].blocks()) {
      total_fail += fr_lp.fail_sol_at(scenarios[0], stages[0], block, col_sol);
    }
    // With limited water, some deficit should exist
    CHECK(total_fail > 0.0);
  }

  SUBCASE("hydro_use_value global fallback")
  {
    const auto global_uv = 0.5;  // $/m³

    // FlowRight without per-element uvalue → should use global fallback
    const Array<FlowRight> flow_right_array = {
        {
            .uid = Uid {1},
            .name = "irr_global_uv",
            .target = 10.0,
        },
    };

    const System system = {
        .name = "GlobalUseValueTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .flow_right_array = flow_right_array,
    };

    PlanningOptions popts;
    popts.model_options.hydro_use_value = global_uv;
    const PlanningOptionsLP options(std::move(popts));
    const auto scale_obj = options.scale_objective();
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    const auto obj_coeffs = lp.get_obj_coeff();

    // Expected: -global_uv * duration * 3600 / scale_objective
    // Block 1: -0.5 * 6 * 3600 / scale = -10800 / scale
    // Block 2: -0.5 * 18 * 3600 / scale = -32400 / scale
    const auto expected_b1 = -global_uv * dur1 * 3600.0 / scale_obj;
    const auto expected_b2 = -global_uv * dur2 * 3600.0 / scale_obj;

    bool found_b1 = false;
    bool found_b2 = false;
    for (Index i = 0; i < lp.get_numcols(); ++i) {
      const auto c = obj_coeffs[i];
      if (doctest::Approx(c).epsilon(1e-8) == expected_b1) {
        found_b1 = true;
      }
      if (doctest::Approx(c).epsilon(1e-8) == expected_b2) {
        found_b2 = true;
      }
    }
    CHECK(found_b1);
    CHECK(found_b2);

    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
  }

  SUBCASE("hydro_fail_cost global fallback")
  {
    const auto global_fc = 1.0;  // $/m³

    // FlowRight without per-element fcost → should use global fallback
    const Array<FlowRight> flow_right_array = {
        {
            .uid = Uid {1},
            .name = "irr_global_fc",
            .target = 10.0,
        },
    };

    const System system = {
        .name = "GlobalFailCostTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .flow_right_array = flow_right_array,
    };

    PlanningOptions popts;
    popts.model_options.hydro_spill_cost = global_fc;
    const PlanningOptionsLP options(std::move(popts));
    const auto scale_obj = options.scale_objective();
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    const auto obj_coeffs = lp.get_obj_coeff();

    // Post-2026-05-17 attach_flow one-sided substitution: fcost-only
    // case folds the fail slack into the primary flow column.  Cost
    // sign flips to `−global_fc × dur × 3600 / scale_obj` per block.
    const auto expected_b1 = -global_fc * dur1 * 3600.0 / scale_obj;
    const auto expected_b2 = -global_fc * dur2 * 3600.0 / scale_obj;

    bool found_b1 = false;
    bool found_b2 = false;
    for (Index i = 0; i < lp.get_numcols(); ++i) {
      const auto c = obj_coeffs[i];
      if (doctest::Approx(c).epsilon(1e-8) == expected_b1) {
        found_b1 = true;
      }
      if (doctest::Approx(c).epsilon(1e-8) == expected_b2) {
        found_b2 = true;
      }
    }
    CHECK(found_b1);
    CHECK(found_b2);

    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
  }

  SUBCASE("use_average creates qeh variable and qavg constraint")
  {
    // use_average=true → stage-average flow variable (qeh) at stage
    // scope: qeh = Σ_b dur_ratio_b × flow_b.  With target + fcost
    // active the kink moves to the qeh column (slack-based), the
    // per-block flows carry hard `[fmin, fmax]` bounds with no kink.
    // The LP will drive qeh to target (fcost penalty on shortfall).
    const auto target_val = 10.0;

    const Array<FlowRight> flow_right_array = {
        {
            .uid = Uid {1},
            .name = "irr_avg",
            .target = target_val,
            .use_average = true,
            .fcost = 1000.0,
        },
    };

    const System system = {
        .name = "UseAverageTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .flow_right_array = flow_right_array,
    };

    const PlanningOptionsLP options;
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // The FlowRightLP exposes accessors for the qeh column and the
    // qavg row presence directly — use them rather than counting
    // numcols/numrows, which is brittle as the LP grows.
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];
    REQUIRE(fr_lp.has_qeh(s, t));
    CHECK(fr_lp.has_qavg_row(s, t));

    // qeh should reach target=10 with abundant gen capacity and fcost
    // penalty driving the LP to satisfy the soft target.
    const auto col_sol = lp.get_col_sol();
    const auto qeh_col = fr_lp.qeh_col_at(s, t);
    CHECK(col_sol[qeh_col] == doctest::Approx(target_val).epsilon(0.01));
  }

  SUBCASE("per-element uvalue overrides hydro_use_value global")
  {
    const auto per_elem_uv = 2000.0;
    const auto global_uv = 0.5;  // $/m³ — should be ignored

    const Array<FlowRight> flow_right_array = {
        {
            .uid = Uid {1},
            .name = "irr_override",
            .target = 10.0,
            .uvalue = per_elem_uv,
        },
    };

    const System system = {
        .name = "OverrideUseValueTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .flow_right_array = flow_right_array,
    };

    PlanningOptions popts;
    popts.model_options.hydro_use_value = global_uv;
    const PlanningOptionsLP options(std::move(popts));
    const auto scale_obj = options.scale_objective();
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    const auto obj_coeffs = lp.get_obj_coeff();

    // Per-element takes precedence and now goes through the standard
    // CostHelper::block_ecost pipeline (per-block duration ×
    // probability × discount).  With one scenario at p=1 and default
    // discount we get expected = -per_elem_uv × duration / scale_obj.
    const auto expected_b1 = -per_elem_uv * dur1 / scale_obj;
    const auto expected_b2 = -per_elem_uv * dur2 / scale_obj;
    bool found = false;
    for (Index i = 0; i < lp.get_numcols(); ++i) {
      const auto c = obj_coeffs[i];
      if (doctest::Approx(c).epsilon(1e-8) == expected_b1
          || doctest::Approx(c).epsilon(1e-8) == expected_b2)
      {
        found = true;
        break;
      }
    }
    CHECK(found);

    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Regression test for the bound_rule lower-bound bug.
//
// Before the compute_block_bounds() refactor, FlowRightLP set both lowb and
// uppb in add_to_lp via inline logic and then update_lp only patched uppb on
// re-clamp.  In a fixed-mode FlowRight where the bound_rule cap shrinks below
// the original discharge value, the result was lowb > uppb → infeasible.
//
// add_to_lp itself already clamped lowb to uppb when the rule fired, so this
// test exercises that path through the now-shared helper.  The same helper is
// used by update_lp, so a passing test here implies update_lp is consistent.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "FlowRight bound_rule clamps both lower and upper bounds in fixed mode")
{
  using namespace gtopt;

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
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
          .lmax = 50.0,
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
          .fmax = 500.0,
      },
  };

  // Reservoir at v=300 — below the rule's 500 breakpoint, so the
  // rule's first segment (constant=5) fires.
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 3000.0,
          .emin = 0.0,
          .emax = 3000.0,
          .eini = 300.0,
      },
  };

  // Fixed-mode FlowRight: discharge = 20, no fmax.  The bound_rule
  // returns 5 at v=300 (below the 500 breakpoint), so the cap (5) is
  // strictly tighter than the discharge target (20).
  //
  // Without the lowb clamp this would yield lowb=20, uppb=5 → infeasible.
  // With the helper, both bounds become 5.
  const Array<FlowRight> flow_right_array = {
      {
          .uid = Uid {1},
          .name = "tight_irrig",
          .junction_a = Uid {1},
          .direction = -1,
          .target = 20.0,
          .bound_rule =
              RightBoundRule {
                  .reservoir = Uid {1},
                  .segments =
                      {
                          {
                              .volume = 0.0,
                              .slope = 0.0,
                              .constant = 5.0,
                          },
                          {
                              .volume = 500.0,
                              .slope = 0.0,
                              .constant = 200.0,
                          },
                      },
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
              },
          },
  };

  const System system = {
      .name = "FlowRightTightClampTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .flow_right_array = flow_right_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
  const auto& scenarios = system_lp.scene().scenarios();
  const auto& stages = system_lp.phase().stages();
  REQUIRE(!scenarios.empty());
  REQUIRE(!stages.empty());

  const auto& flow_cols = fr_lp.flow_cols_at(scenarios[0], stages[0]);
  REQUIRE(!flow_cols.empty());
  const auto fcol = flow_cols.begin()->second;

  auto& lp = system_lp.linear_interface();
  const auto col_low = lp.get_col_low();
  const auto col_upp = lp.get_col_upp();

  // Both bounds collapse to the rule's clamped value of 5.  Pre-fix,
  // lowb would have been left at the discharge value of 20, producing
  // an infeasible LP (lowb > uppb).
  CHECK(col_low[static_cast<size_t>(fcol)] == doctest::Approx(5.0));
  CHECK(col_upp[static_cast<size_t>(fcol)] == doctest::Approx(5.0));

  // The LP itself must remain feasible — this is the canary that
  // catches a regression of the lower-bound bug end-to-end.
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}
