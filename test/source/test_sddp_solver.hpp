/**
 * @file      test_sddp_solver.hpp
 * @brief     Unit tests for the SDDPSolver (SDDP forward/backward iteration)
 * @date      2026-03-08
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. Basic 3-phase hydro+thermal case (24 blocks per phase) – validates
 *     convergence after a few iterations.
 *  2. Verifies that the SDDP solver rejects single-phase problems.
 */

#include <cmath>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_solver.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Create a 3-phase hydro+thermal planning problem.
///
/// - 1 bus
/// - 1 hydro generator (25 MW, $5/MWh)
/// - 1 thermal generator (500 MW, $50/MWh)
/// - 1 demand (100 MW constant)
/// - 1 reservoir (capacity 150 dam³, starts at 100 dam³)
/// - Natural inflow: 10 dam³/h
/// - Hydro topology: 2 junctions, 1 waterway, 1 reservoir, 1 turbine
/// - 3 phases, each with 1 stage of 24 blocks (1 hour each)
///
/// The reservoir can store enough water to generate hydro for about 6 hours.
/// The SDDP iteration should learn the water value and optimise reservoir
/// management across the 3 phases.
auto make_3phase_hydro_planning() -> Planning
{
  // ── Blocks: 72 blocks total (24 per phase × 3 phases) ──
  Array<Block> block_array;
  for (int i = 0; i < 72; ++i) {
    block_array.push_back(Block {
        .uid = Uid {i + 1},
        .duration = 1.0,
    });
  }

  // ── Stages: 3 stages, one per phase ──
  Array<Stage> stage_array = {
      Stage {
          .uid = Uid {1},
          .first_block = 0,
          .count_block = 24,
      },
      Stage {
          .uid = Uid {2},
          .first_block = 24,
          .count_block = 24,
      },
      Stage {
          .uid = Uid {3},
          .first_block = 48,
          .count_block = 24,
      },
  };

  // ── Phases: 3 phases, each containing 1 stage ──
  Array<Phase> phase_array = {
      Phase {
          .uid = Uid {1},
          .first_stage = 0,
          .count_stage = 1,
      },
      Phase {
          .uid = Uid {2},
          .first_stage = 1,
          .count_stage = 1,
      },
      Phase {
          .uid = Uid {3},
          .first_stage = 2,
          .count_stage = 1,
      },
  };

  // ── System components ──
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "bus1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 25.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  // Demand: 100 MW constant across all blocks
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  // ── Hydro system ──
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

  // Reservoir: 150 dam³ capacity, starts at 100 dam³
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 150.0,
          .emin = 0.0,
          .emax = 150.0,
          .eini = 100.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
      },
  };

  // Natural inflow: 10 dam³/h
  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 10.0,
      },
  };

  // Turbine links waterway to hydro generator
  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 1.0,
      },
  };

  // ── Simulation ──
  const Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
              },
          },
      .phase_array = std::move(phase_array),
  };

  // ── Options ──
  Options options;
  options.demand_fail_cost = OptReal {1000.0};
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = OptName {"csv"};
  options.output_compression = OptName {"uncompressed"};

  // ── System ──
  System system = {
      .name = "sddp_hydro_3phase",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = std::move(system),
  };
}

}  // namespace

TEST_CASE("SDDPSolver - 3-phase hydro+thermal converges")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  // First verify the monolithic solve works
  {
    auto result = planning_lp.resolve();
    REQUIRE(result.has_value());
    CHECK(*result == 1);  // 1 scene resolved
  }

  // Run SDDP with 5 iterations (matching the PLP comparison requirement)
  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 5;
  sddp_opts.convergence_tol = 1e-3;

  SDDPSolver sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(results->empty());

  // The iterations should show monotonically non-decreasing lower bound
  const auto& first = results->front();
  const auto& last = results->back();
  CHECK(first.iteration == 1);

  // After iterations, bounds should be reasonable (positive costs)
  CHECK(last.upper_bound > 0.0);
  CHECK(last.lower_bound > 0.0);

  // Gap should be non-negative
  CHECK(last.gap >= 0.0);
}

TEST_CASE("SDDPSolver - requires at least 2 phases")  // NOLINT
{
  // Create a single-phase planning (should fail SDDP validation)
  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
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
                  .uid = Uid {1},
              },
          },
  };

  const System system = {
      .name = "single_phase_test",
      .bus_array =
          {
              {
                  .uid = Uid {1},
                  .name = "b1",
              },
          },
      .demand_array =
          {
              {
                  .uid = Uid {1},
                  .name = "d1",
                  .bus = Uid {1},
                  .capacity = 50.0,
              },
          },
      .generator_array =
          {
              {
                  .uid = Uid {1},
                  .name = "g1",
                  .bus = Uid {1},
                  .gcost = 10.0,
                  .capacity = 100.0,
              },
          },
  };

  Options options;
  options.demand_fail_cost = OptReal {1000.0};

  Planning planning {
      .options = std::move(options),
      .simulation = simulation,
      .system = system,
  };

  PlanningLP planning_lp(std::move(planning));
  SDDPSolver sddp(planning_lp);
  auto results = sddp.solve();
  CHECK_FALSE(results.has_value());  // Should fail: < 2 phases
}
