/**
 * @file      test_hydro_lp.hpp
 * @brief     Unit tests for hydro system LP components (junction, waterway,
 *            flow, reservoir, turbine, seepage)
 * @date      2026-02-18
 * @copyright BSD-3-Clause
 */

#include <algorithm>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/reservoir_lp.hpp>
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

/// Solve a minimal single-block hydro LP and assert that the reservoir's
/// per-block extraction column reaches the expected positive primal value.
///
/// Topology:
///   bus b1 ── demand 50 MW
///   junction j1 (reservoir) ──ww1──> junction j2 (drain)
///   turbine on ww1 with production_factor = 2 MW/(m³/s), generator on b1
///
/// With a 50 MW demand served entirely by the hydro turbine (gcost=5 beats
/// the 1000 $/MWh demand-fail cost), the turbine flow must be
/// 50 MW / 2 MW/(m³/s) = 25 m³/s.  That flow is drawn out of junction j1
/// via ww1, so the reservoir's extraction column — which adds to the j1
/// balance with coefficient +1.0 — must equal 25 m³/s.
TEST_CASE(  // NOLINT
    "ReservoirLP - extraction column primal value matches turbine draw")
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
      .name = "ReservoirExtractionSolve",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto& rsv_lps = system_lp.elements<ReservoirLP>();
  REQUIRE(rsv_lps.size() == 1);
  const auto& rsv_lp = rsv_lps.front();

  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& block_lp = simulation_lp.blocks().front();

  const auto& ext_cols = rsv_lp.extraction_cols_at(scenario_lp, stage_lp);
  const auto it = ext_cols.find(block_lp.uid());
  REQUIRE(it != ext_cols.end());
  const auto ext_col = it->second;

  const auto ext_val = lp.get_col_sol()[ext_col];
  // 50 MW demand / 2 MW per (m³/s) turbine production factor = 25 m³/s.
  CHECK(ext_val == doctest::Approx(25.0).epsilon(1e-6));
}

/// Verify that WaterwayLP creates a flow column whose primal solution equals
/// the water flow forced by the junction water balance.  Topology:
///
///   [Flow 50 m³/s] -> (j_up) --[ww1, turbine PF=2]--> (j_down, drain)
///                                      |
///                                      +--> generator --> 100 MW demand
///
/// Water balance at j_up: inflow (50) must leave via ww1 → ww1.flow = 50.
/// Power balance at b1: turbine output = PF · flow = 2 · 50 = 100 MW, which
/// matches the 100 MW demand exactly, so no thermal back-up is used.
TEST_CASE("WaterwayLP flow column solution matches forced water balance")
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
          .fmax = 200.0,
      },
  };

  // Natural inflow of 50 m³/s into j_up (direction=+1).
  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow_up",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 50.0,
      },
  };

  // Turbine on ww1 with production factor 2 MW·s/m³ → 50 m³/s ≡ 100 MW.
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
      .name = "WaterwayFlowSolve",
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
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Look up the flow column of the single WaterwayLP via flow_cols_at.
  const auto& ww_lps = system_lp.elements<WaterwayLP>();
  REQUIRE(ww_lps.size() == 1);
  const auto& ww_lp = ww_lps.front();
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& block_lp = simulation_lp.blocks().front();
  const auto& fcols = ww_lp.flow_cols_at(scenario_lp, stage_lp);
  const auto it = fcols.find(block_lp.uid());
  REQUIRE(it != fcols.end());
  const auto flow_val = lp.get_col_sol()[it->second];
  CHECK(flow_val == doctest::Approx(50.0).epsilon(1e-6));
}

/// Verify JunctionLP drain column primal at LP optimum.
///
/// Topology:
///   inflow (10 m³/s) → j_up (drain=true) ──ww1──▶ j_down (drain=true)
/// The turbine on ww1 (production_factor=1 MW·s/m³) feeds a 5 MW demand
/// via a single bus, so generation must equal demand = 5 MW → waterway
/// flow = 5 m³/s.  Balance at j_up:  10 − ww1_flow − drain_up = 0  forces
/// drain_up = 5 m³/s.  Balance at j_down: ww1_flow − drain_down = 0 forces
/// drain_down = 5 m³/s.  We assert the primal value of the drain column
/// of j_up (where the inflow surplus is absorbed).
TEST_CASE("JunctionLP drain col primal absorbs flow surplus")  // NOLINT
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
          .gcost = 1.0,
          .capacity = 100.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 5.0,
      },
  };

  // Two drain-enabled junctions so the waterway can carry less than
  // the full inflow (drain at j_up absorbs the surplus).
  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
          .drain = true,
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  // Natural inflow of 10 m³/s into j_up (direction=+1).
  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow_up",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 10.0,
      },
  };

  // Waterway j_up → j_down, turbine draws from it.
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

  // Turbine: gen = production_factor × waterway_flow  (equality, no drain).
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
      .name = "JunctionDrainTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Locate the drain-bearing junction j_up (uid=1).
  const auto& junction_lps = system_lp.elements<JunctionLP>();
  REQUIRE(junction_lps.size() == 2);
  const auto j_up_it = std::ranges::find_if(
      junction_lps, [](const auto& j) { return j.uid() == Uid {1}; });
  REQUIRE(j_up_it != junction_lps.end());
  const auto& j_up_lp = *j_up_it;

  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& block_lp = simulation_lp.blocks().front();

  // Primal: drain column of j_up absorbs the 5 m³/s surplus
  // (inflow 10 − waterway/turbine flow 5 = 5).
  // drain_cols_at returns BIndexHolder<ColIndex> keyed by BlockUid.
  const auto& drain_cols = j_up_lp.drain_cols_at(scenario_lp, stage_lp);
  const auto it_d = drain_cols.find(block_lp.uid());
  REQUIRE(it_d != drain_cols.end());
  const auto drain_col = it_d->second;

  const auto drain_val = lp.get_col_sol()[drain_col];
  CHECK(drain_val == doctest::Approx(5.0).epsilon(1e-6));
}

/// Verify that the ReservoirLP storage-balance row dual reflects the
/// marginal value of water when the reservoir is water-scarce.
///
/// Topology (hydro + thermal backup on a single bus):
///   reservoir(eini small, no inflow) ─► ww1 ─► j_down(drain)
///                 │
///                 └── turbine(pf=1) ─► hydro_gen
///   thermal_gen (gcost=50, capacity=1000) also feeds the bus.
///   demand = 10 MW for a single 1-hour block.
///
/// The reservoir has so little water (eini = 0.01 hm³, no inflow) that
/// only a fraction of the 10 MWh demand can be served by hydro; the
/// thermal generator supplies the rest at the marginal cost of 50
/// $/MWh.  At optimum, the last available unit of water saves one
/// thermal MWh, so the storage-balance row dual is non-zero (the
/// reservoir constraint is binding).  The exact value depends on the
/// solver's sign convention and internal scaling; we only assert that
/// the dual is numerically non-zero.
TEST_CASE(  // NOLINT
    "ReservoirLP — storage-balance row dual reflects water scarcity")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };

  // Two generators on the same bus: cheap hydro (gcost=0) fed by the
  // turbine, and an expensive thermal backup that sets the marginal
  // cost of energy at the bus.
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 0.0,
          .capacity = 1000.0,
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
          .capacity = 10.0,
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
          .fmax = 10000.0,
      },
  };

  // Water-scarce reservoir: tiny initial volume, no natural inflow.
  // eini = 0.01 hm³ ≈ 2.78 m³/s·h ≈ 2.78 MWh at production_factor = 1.
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 100000.0,
          .emin = 0.0,
          .emax = 100000.0,
          .eini = 0.01,
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
      .name = "ReservoirWaterValueTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Locate the single ReservoirLP and look up the energy-balance row
  // for (scenario, stage, first block).  This is the storage-balance
  // row whose dual is the marginal value of stored water.
  const auto& reservoir_lps = system_lp.elements<ReservoirLP>();
  REQUIRE(reservoir_lps.size() == 1);
  const auto& rsv_lp = reservoir_lps.front();

  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& block_lp = simulation_lp.blocks().front();

  const auto& erows = rsv_lp.energy_rows_at(scenario_lp, stage_lp);
  const auto it_r = erows.find(block_lp.uid());
  REQUIRE(it_r != erows.end());
  const auto energy_row = it_r->second;

  lp.ensure_duals();
  const auto row_duals = lp.get_row_dual_raw();
  REQUIRE(row_duals.size() == static_cast<std::size_t>(lp.get_numrows()));

  const auto water_dual = row_duals[energy_row];

  // Water is scarce and the reservoir balance row is binding at optimum,
  // so its dual (the "water value") must be numerically non-zero.  The
  // sign and exact magnitude depend on solver conventions and internal
  // row equilibration / objective scaling, so we only assert that the
  // dual is clearly non-zero.
  CHECK(std::abs(water_dual) > 1e-3);
}

/// Regression: a reservoir whose `spillway_capacity == 0` must NOT emit
/// a `reservoir_drain_*` LP column nor reference it from the energy_balance
/// row.  Background: PLP encodes reservoirs whose spillway is structurally
/// disabled (e.g. LMAULE/ELTORO) by setting IBind/SerVer = 0 → plp2gtopt
/// outputs `spillway_capacity = 0.0`.  Before the fix, gtopt emitted a
/// `[0,0]` drain column per (block, stage, scenario), bloating the LP with
/// thousands of dead variables.  After the fix, drain emission is gated by
/// a positive capacity.
TEST_CASE(  // NOLINT
    "ReservoirLP spillway_capacity=0 omits reservoir_drain column")
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
          .name = "g1",
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
          .lmax = 50.0,
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

  // Two reservoirs: one with spillway disabled (capacity=0), one with a
  // positive capacity AND a non-zero spillway_cost (drain enabled).
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {10},
          .name = "rsv_no_spill",
          .junction = Uid {1},
          .spillway_capacity = 0.0,
          .spillway_cost = 5.0,  // cost set, but capacity=0 → still disabled
          .capacity = 1000.0,
          .emin = 0.0,
          .emax = 1000.0,
          .eini = 500.0,
          .use_state_variable = false,
      },
      {
          .uid = Uid {11},
          .name = "rsv_with_spill",
          .junction = Uid {1},
          .spillway_capacity = 1000.0,
          .spillway_cost = 5.0,
          .capacity = 1000.0,
          .emin = 0.0,
          .emax = 1000.0,
          .eini = 500.0,
          .use_state_variable = false,
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
      .name = "RsvNoSpillTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
  };

  PlanningOptions opts;
  opts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options {opts};
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto& lp = system_lp.linear_interface();
  const auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto& reservoir_lps = system_lp.elements<ReservoirLP>();
  REQUIRE(reservoir_lps.size() == 2);

  const auto find_rsv = [&](Uid u) -> const ReservoirLP&
  {
    const auto it = std::ranges::find_if(
        reservoir_lps, [u](const auto& r) { return r.uid() == u; });
    REQUIRE(it != reservoir_lps.end());
    return *it;
  };

  const auto& rsv_no_spill = find_rsv(Uid {10});
  const auto& rsv_with_spill = find_rsv(Uid {11});

  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();

  // The disabled reservoir must not register any drain columns.
  CHECK(rsv_no_spill.find_drain_cols(scenario_lp, stage_lp) == nullptr);

  // The enabled reservoir must still register one drain column per block.
  const auto* dcols_with =
      rsv_with_spill.find_drain_cols(scenario_lp, stage_lp);
  REQUIRE(dcols_with != nullptr);
  CHECK(dcols_with->size() == 1);
}
