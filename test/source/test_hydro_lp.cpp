/**
 * @file      test_hydro_lp.hpp
 * @brief     Unit tests for hydro system LP components (junction, waterway,
 *            flow, reservoir, turbine, seepage)
 * @date      2026-02-18
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/utils.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("SystemLP with hydro components - junction, waterway, reservoir")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  // Generator connected to turbine
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
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  // Two junctions connected by a waterway
  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j_upstream"},
      {.uid = Uid {2}, .name = "j_downstream", .drain = true},
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
          .production_factor = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {1}, .duration = 1},
              {.uid = Uid {2}, .duration = 2},
          },
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 2}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "HydroTest",
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
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("SystemLP with flow component")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

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
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j1", .drain = true},
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow1",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 10.0,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {1},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "FlowTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
}

TEST_CASE("SystemLP with multi-stage hydro system")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

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
          .gcost = 100.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j_up"},
      {.uid = Uid {2}, .name = "j_down", .drain = true},
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
          .name = "natural_inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 50.0,
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
              {.uid = Uid {1}, .duration = 1},
              {.uid = Uid {2}, .duration = 2},
              {.uid = Uid {3}, .duration = 3},
          },
      .stage_array =
          {
              {.uid = Uid {1}, .first_block = 0, .count_block = 1},
              {.uid = Uid {2}, .first_block = 1, .count_block = 1},
              {.uid = Uid {3}, .first_block = 2, .count_block = 1},
          },
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "MultiStageHydro",
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
}

/// Verify that the LP solution and output are invariant to the choice of
/// energy scale via variable_scales.  Identical reservoir systems with
/// different variable scale values must produce the same objective value.
TEST_CASE(  // NOLINT
    "Reservoir energy_scale invariance – same solution for different scales")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Helper lambda: builds and solves a hydro LP with the given energy scale
  // via variable_scales option, returns the objective value.
  auto solve_with_scale = [](double scale) -> double
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
            .capacity = 300.0,
        },
        {
            .uid = Uid {2},
            .name = "thermal_gen",
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
            .capacity = 2000.0,
            .emin = 100.0,
            .emax = 2000.0,
            .eini = 1000.0,
        },
    };

    const Array<Flow> flow_array = {
        {
            .uid = Uid {1},
            .name = "natural_inflow",
            .direction = 1,
            .junction = Uid {1},
            .discharge = 50.0,
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
                    .count_block = 1,
                },
                {
                    .uid = Uid {2},
                    .first_block = 1,
                    .count_block = 1,
                },
                {
                    .uid = Uid {3},
                    .first_block = 2,
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
        .name = "VolScaleInvariance",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .flow_array = flow_array,
        .reservoir_array = reservoir_array,
        .turbine_array = turbine_array,
    };

    PlanningOptions opts;
    // Set energy scale via variable_scales option (not per-element field)
    opts.variable_scales = {
        {
            .class_name = "Reservoir",
            .variable = "energy",
            .scale = scale,
        },
    };
    const PlanningOptionsLP options {opts};
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& li = system_lp.linear_interface();
    const auto result = li.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    return li.get_obj_value();
  };

  const auto obj_scale_1 = solve_with_scale(1.0);
  const auto obj_scale_10 = solve_with_scale(10.0);
  const auto obj_scale_100 = solve_with_scale(100.0);
  const auto obj_scale_1000 = solve_with_scale(1000.0);

  // All objective values must be identical (within floating-point tolerance)
  CHECK(obj_scale_10 == doctest::Approx(obj_scale_1).epsilon(1e-8));
  CHECK(obj_scale_100 == doctest::Approx(obj_scale_1).epsilon(1e-8));
  CHECK(obj_scale_1000 == doctest::Approx(obj_scale_1).epsilon(1e-8));
}

/// Verify that PlanningOptions::variable_scales affects the Reservoir energy
/// scale the same way as the per-element Reservoir::energy_scale field. This
/// confirms the JSON variable_scales mechanism is not ignored by checking both
/// solution invariance AND that LP column bounds actually change.
TEST_CASE(  // NOLINT
    "Reservoir variable_scales option – invariance and LP coefficient change")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  struct ScaleResult
  {
    double objective;
    double max_col_upper;  // max physical upper bound
    double max_col_upper_raw;  // max raw LP upper bound
  };

  // Helper: builds and solves a hydro LP using PlanningOptions::variable_scales
  // (NOT Reservoir::energy_scale) and returns the objective + max scaled bound.
  auto solve_with_variable_scales = [](double scale) -> ScaleResult
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
            .capacity = 300.0,
        },
        {
            .uid = Uid {2},
            .name = "thermal_gen",
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

    // NOTE: no energy_scale set on the reservoir — default is used
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
            .name = "natural_inflow",
            .direction = 1,
            .junction = Uid {1},
            .discharge = 50.0,
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
                    .count_block = 1,
                },
                {
                    .uid = Uid {2},
                    .first_block = 1,
                    .count_block = 1,
                },
                {
                    .uid = Uid {3},
                    .first_block = 2,
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
        .name = "VarScaleVolInvariance",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .flow_array = flow_array,
        .reservoir_array = reservoir_array,
        .turbine_array = turbine_array,
    };

    PlanningOptions opts;
    // Set energy scale via variable_scales option (not per-element field)
    opts.variable_scales = {
        {
            .class_name = "Reservoir",
            .variable = "energy",
            .uid = Uid {1},
            .scale = scale,
            .name = "rsv1",
        },
    };
    const PlanningOptionsLP options {opts};
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto& li = system_lp.linear_interface();
    const auto result = li.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // Verify the scale factor is stored in LP column scales.
    // Volume columns get scale = energy_scale; find the maximum upper
    // bound among columns with that scale to prove the LP changed.
    const auto& col_scales = li.get_col_scales();
    REQUIRE_FALSE(col_scales.empty());
    const auto col_upp = li.get_col_upp();
    const auto col_upp_raw = li.get_col_upp_raw();
    double max_upp = 0.0;
    double max_upp_raw = 0.0;
    int n_scaled = 0;
    for (const auto ci : iota_range<ColIndex>(0, li.get_numcols())) {
      if (col_scales[ci] == doctest::Approx(scale).epsilon(1e-12)) {
        ++n_scaled;
        max_upp = std::max(max_upp, col_upp[ci]);
        max_upp_raw = std::max(max_upp_raw, col_upp_raw[ci]);
      }
    }
    // Multiple volume columns (eini + per-block + sini) should carry scale
    REQUIRE(n_scaled > 0);

    return {.objective = li.get_obj_value(),
            .max_col_upper = max_upp,
            .max_col_upper_raw = max_upp_raw};
  };

  // Use scales != 1.0 to distinguish volume columns from unscaled columns
  const auto [obj_10, max_10, max_10_raw] = solve_with_variable_scales(10.0);
  const auto [obj_1000, max_1000, max_1000_raw] =
      solve_with_variable_scales(1000.0);

  // Objective invariance: same physical solution regardless of scale
  CHECK(obj_1000 == doctest::Approx(obj_10).epsilon(1e-8));

  // Physical bounds are invariant: emax=2000 regardless of scale.
  CHECK(max_10 == doctest::Approx(2000.0).epsilon(1e-10));
  CHECK(max_1000 == doctest::Approx(2000.0).epsilon(1e-10));

  // Raw LP bounds differ: emax=2000, so max LP upper bound = 2000/scale.
  // scale=10 → max_upp_raw=200, scale=1000 → max_upp_raw=2.
  CHECK(max_10_raw == doctest::Approx(2000.0 / 10.0).epsilon(1e-10));
  CHECK(max_1000_raw == doctest::Approx(2000.0 / 1000.0).epsilon(1e-10));
  CHECK(max_10_raw != doctest::Approx(max_1000_raw));  // Proves they differ
}

/// Verify that setting both volume and flow variable_scales for a Reservoir
/// produces the same objective as volume-only scaling, confirming that
/// flow_scale is applied to the extraction flow LP columns without breaking
/// the energy balance.
TEST_CASE(  // NOLINT
    "Reservoir flow variable_scale – objective invariance with volume+flow")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Helper: builds and solves a hydro LP with energy_scale and optional
  // flow_scale via variable_scales, returns the objective value.
  auto solve_with_scales = [](double energy_scale, double flow_scale) -> double
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
            .capacity = 300.0,
        },
        {
            .uid = Uid {2},
            .name = "thermal_gen",
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
            .capacity = 2000.0,
            .emin = 100.0,
            .emax = 2000.0,
            .eini = 1000.0,
        },
    };

    const Array<Flow> flow_array = {
        {
            .uid = Uid {1},
            .name = "natural_inflow",
            .direction = 1,
            .junction = Uid {1},
            .discharge = 50.0,
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
                    .count_block = 1,
                },
                {
                    .uid = Uid {2},
                    .first_block = 1,
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
        .name = "FlowScaleTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .flow_array = flow_array,
        .reservoir_array = reservoir_array,
        .turbine_array = turbine_array,
    };

    PlanningOptions opts;
    // Set both energy and flow scales via variable_scales
    opts.variable_scales = {
        {
            .class_name = "Reservoir",
            .variable = "energy",
            .uid = Uid {1},
            .scale = energy_scale,
            .name = "rsv1",
        },
        {
            .class_name = "Reservoir",
            .variable = "flow",
            .uid = Uid {1},
            .scale = flow_scale,
            .name = "rsv1",
        },
    };
    const PlanningOptionsLP options {opts};
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto& li = system_lp.linear_interface();
    const auto result = li.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    return li.get_obj_value();
  };

  // Scenario 1: volume and flow scaled together (same value, as plp2gtopt
  // now emits) — this is the recommended configuration.
  const double obj_both_100 = solve_with_scales(100.0, 100.0);

  // Scenario 2: different volume scale, flow scale matches volume
  const double obj_both_1000 = solve_with_scales(1000.0, 1000.0);

  // Scenario 3: volume only (flow_scale=1.0, the default)
  const double obj_vol_only = solve_with_scales(100.0, 1.0);

  // Objective invariance: all scenarios solve the same physical problem
  CHECK(obj_both_100 == doctest::Approx(obj_both_1000).epsilon(1e-6));
  CHECK(obj_both_100 == doctest::Approx(obj_vol_only).epsilon(1e-6));
}
