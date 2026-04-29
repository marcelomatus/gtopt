// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      sddp_helpers.hpp
 * @brief     Shared helper functions for SDDP test files
 * @date      2026-04-05
 *
 * Provides make_*_planning() factory functions used by multiple SDDP test
 * files.  Extracted from test_sddp_method.hpp to avoid duplicate test
 * registration when included across unity build batches.
 */

#pragma once

#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_method.hpp>
#include <gtopt/sddp_method.hpp>

#include "fixture_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)
using gtopt::test_fixtures::make_single_stage_phases;
using gtopt::test_fixtures::make_uniform_blocks;
using gtopt::test_fixtures::make_uniform_stages;

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
inline auto make_3phase_hydro_planning() -> Planning
{
  // ── Blocks: 72 total (24 per phase × 3 phases) ──
  Array<Block> block_array = make_uniform_blocks(72, 1.0);

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
          .capacity = 50.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

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

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 500.0,
          .emin = 0.0,
          .emax = 500.0,
          .eini = 250.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 10.0,
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

  // ── Simulation ──
  Simulation simulation = {
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

  // ── PlanningOptions ──
  PlanningOptions options;
  options.demand_fail_cost = 1000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

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

/// Create a simple single-phase planning problem for monolithic solver tests.
inline auto make_single_phase_planning() -> Planning
{
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

  PlanningOptions options;
  options.demand_fail_cost = 1000.0;

  return Planning {
      .options = std::move(options),
      .simulation = simulation,
      .system = system,
  };
}

/// Create a 5-phase reservoir+thermal planning problem for SDDP vs monolithic
/// comparison.
///
/// - 1 bus, single-bus mode
/// - 1 thermal generator (200 MW, $80/MWh)
/// - 1 hydro generator (50 MW, $5/MWh)
/// - 1 demand (100 MW, 3-hour blocks → varying daily profile)
/// - 1 reservoir (500 dam³ capacity, starts at 300 dam³)
/// - Natural inflow: 8 dam³/h
/// - Hydro topology: 2 junctions, 1 waterway, 1 turbine
/// - 5 phases, each with 1 stage of 8 blocks (3 hours each = 24h per stage)
inline auto make_5phase_reservoir_planning() -> Planning
{
  constexpr int num_phases = 5;
  constexpr int blocks_per_phase = 8;
  constexpr double block_duration = 3.0;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  auto block_array = make_uniform_blocks(static_cast<std::size_t>(total_blocks),
                                         block_duration);

  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

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
          .capacity = 50.0,
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
          .name = "load1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  // Hydro system
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

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 500.0,
          .emin = 0.0,
          .emax = 500.0,
          .eini = 300.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 8.0,
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

  Simulation simulation = {
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

  PlanningOptions options;
  options.demand_fail_cost = 5000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  System system = {
      .name = "sddp_reservoir_5phase",
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

/// Create a 5-phase small-reservoir+thermal planning problem to test
/// state-variable coupling between phases.
///
/// Uses a simpler reservoir (smaller, more constrained) than the main
/// reservoir test to exercise a different state-coupling regime:
/// - The reservoir is small (200 dam³) with low inflow (5 dam³/h)
/// - The hydro generator is large (80 MW) relative to the reservoir
/// - This forces the reservoir to deplete across phases, creating
///   non-trivial state variable values at phase boundaries
///
/// - 1 bus, single-bus mode
/// - 1 thermal generator (200 MW, $80/MWh)
/// - 1 hydro generator (80 MW, $3/MWh)
/// - 1 demand (100 MW constant)
/// - 1 small reservoir (200 dam³ capacity, starts at 180 dam³)
/// - Natural inflow: 5 dam³/h (low, so reservoir depletes over time)
/// - Hydro topology: 2 junctions, 1 waterway, 1 turbine
/// - 5 phases, each with 1 stage of 8 blocks (3 hours each)
inline auto make_5phase_small_reservoir_planning() -> Planning
{
  constexpr int num_phases = 5;
  constexpr int blocks_per_phase = 8;
  constexpr double block_duration = 3.0;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  auto block_array = make_uniform_blocks(static_cast<std::size_t>(total_blocks),
                                         block_duration);

  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

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
          .gcost = 3.0,
          .capacity = 80.0,
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
          .name = "load1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  // Hydro system — small reservoir with low inflow
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

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv_small",
          .junction = Uid {1},
          .capacity = 200.0,
          .emin = 0.0,
          .emax = 200.0,
          .eini = 180.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 5.0,
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

  Simulation simulation = {
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

  PlanningOptions options;
  options.demand_fail_cost = 5000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  System system = {
      .name = "sddp_small_reservoir_5phase",
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

/// Create a 5-phase generator-expansion planning problem for SDDP vs
/// monolithic comparison.
///
/// Tests that the `capainst` (installed-capacity) state variable is
/// correctly coupled between phases.  The generator starts with 0 MW
/// capacity and must expand (invest in modules) across phases to serve
/// a 100 MW demand.
///
/// - 1 bus, single-bus mode
/// - 1 expandable generator (0 MW initial, 50 MW/module, max 10 modules,
///   $80/MWh operating cost, $500/module-year investment cost)
/// - 1 cheap backup generator (200 MW, $200/MWh — expensive "peaker" that
///   makes expansion worthwhile)
/// - 1 demand (100 MW constant)
/// - 5 phases, each with 1 stage of 8 blocks (3 hours each)
inline auto make_5phase_expansion_planning() -> Planning
{
  constexpr int num_phases = 5;
  constexpr int blocks_per_phase = 8;
  constexpr double block_duration = 3.0;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  auto block_array = make_uniform_blocks(static_cast<std::size_t>(total_blocks),
                                         block_duration);

  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "bus1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "expandable_gen",
          .bus = Uid {1},
          .gcost = 80.0,
          .capacity = 0.0,
          .expcap = 50.0,
          .expmod = 10.0,
          .annual_capcost = 500.0,
      },
      {
          .uid = Uid {2},
          .name = "backup_gen",
          .bus = Uid {1},
          .gcost = 200.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };

  Simulation simulation = {
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

  PlanningOptions options;
  options.demand_fail_cost = 5000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  System system = {
      .name = "sddp_expansion_5phase",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = std::move(system),
  };
}

/// Create a year-long 12-phase hydro+thermal planning problem for SDDP vs
/// monolithic comparison.  Inspired by the sddp_hydro_3phase case.
///
/// Each phase represents one month (1 stage of 24 hourly blocks = one
/// representative day per month).  The reservoir has seasonal inflow:
/// higher in winter/spring (months 5–8), lower in summer (months 1–4, 9–12).
///
/// - 1 bus, single-bus mode
/// - 1 hydro generator (25 MW, $5/MWh)
/// - 1 thermal generator (200 MW, $80/MWh)
/// - 1 demand (50 MW constant)
/// - 1 reservoir (150 dam³ capacity, starts at 100 dam³)
/// - Variable inflow: 5–15 dam³/h seasonal pattern
/// - 12 phases × 1 stage × 24 blocks (1h each) = 288 blocks total
inline auto make_12phase_yearly_hydro_planning() -> Planning
{
  constexpr int num_phases = 12;
  constexpr int blocks_per_phase = 24;
  constexpr double block_duration = 1.0;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  auto block_array = make_uniform_blocks(static_cast<std::size_t>(total_blocks),
                                         block_duration);

  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

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
          .capacity = 50.0,
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
          .name = "load1",
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
          .fmax = 200.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 500.0,
          .emin = 0.0,
          .emax = 500.0,
          .eini = 250.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
      },
  };

  // Seasonal inflow: 10 dam³/h average (same as sddp_hydro_3phase)
  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 10.0,
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

  Simulation simulation = {
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

  PlanningOptions options;
  options.demand_fail_cost = 5000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  System system = {
      .name = "sddp_yearly_hydro_12phase",
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

inline auto make_2scene_3phase_hydro_planning(double prob1 = 0.7,
                                              double prob2 = 0.3) -> Planning
{
  constexpr int num_phases = 3;
  constexpr int blocks_per_phase = 4;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(total_blocks), 1.0);

  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
                  .probability_factor = prob1,
              },
              {
                  .uid = Uid {2},
                  .probability_factor = prob2,
              },
          },
      .phase_array = std::move(phase_array),
      .scene_array =
          {
              {
                  .uid = Uid {1},
                  .name = "scene1",
                  .active = true,
                  .first_scenario = 0,
                  .count_scenario = 1,
              },
              {
                  .uid = Uid {2},
                  .name = "scene2",
                  .active = true,
                  .first_scenario = 1,
                  .count_scenario = 1,
              },
          },
  };

  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 50.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 200.0,
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
          .fmax = 100.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 200.0,
          .emin = 0.0,
          .emax = 200.0,
          .eini = 100.0,
          .fmin = -1000.0,
          .fmax = 1000.0,
          .flow_conversion_rate = 1.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 8.0,
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

  const System system = {
      .name = "sddp_2scene_3phase",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  PlanningOptions options;
  options.demand_fail_cost = 1000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = system,
  };
}

/// Create a minimal 2-phase hydro+thermal planning problem.
///
/// - 1 bus
/// - 1 hydro generator (20 MW, $5/MWh)
/// - 1 thermal generator (100 MW, $50/MWh)
/// - 1 demand (30 MW constant)
/// - 1 reservoir (capacity 100 dam³, starts at 50 dam³, inflow 5 dam³/h)
/// - 2 phases, each with 1 stage of 4 blocks (1 hour each)
///
/// This is the simplest possible SDDP test case with a state variable
/// (reservoir volume) linking the two phases via Benders cuts.
inline auto make_2phase_linear_planning() -> Planning
{
  constexpr int num_phases = 2;
  constexpr int blocks_per_phase = 4;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(total_blocks), 1.0);

  Array<Stage> stage_array = {
      Stage {
          .uid = Uid {1},
          .first_block = 0,
          .count_block = blocks_per_phase,
      },
      Stage {
          .uid = Uid {2},
          .first_block = blocks_per_phase,
          .count_block = blocks_per_phase,
      },
  };

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
  };

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
          .capacity = 20.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
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

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 50.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 100.0,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 50.0,
          .fmin = -500.0,
          .fmax = +500.0,
          .flow_conversion_rate = 1.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 5.0,
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

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
                  .probability_factor = 1.0,
              },
          },
      .phase_array = std::move(phase_array),
  };

  PlanningOptions options;
  options.demand_fail_cost = 1000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  System system = {
      .name = "sddp_linear_2phase",
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

/// Create a 2-phase planning with 2 scenarios for aperture testing.
/// Each scenario has a different inflow (5 and 15 dam³/h).
inline auto make_2phase_2scenario_planning() -> Planning
{
  auto planning = make_2phase_linear_planning();

  // Add second scenario
  planning.simulation.scenario_array.push_back(Scenario {
      .uid = Uid {2},
      .probability_factor = 0.5,
  });
  // Set first scenario probability
  planning.simulation.scenario_array[0].probability_factor = OptReal {0.5};

  // Add second flow with different discharge for scenario 2
  planning.system.flow_array.push_back(Flow {
      .uid = Uid {2},
      .name = "inflow_dry",
      .direction = 1,
      .junction = Uid {1},
      .discharge = 15.0,
  });

  return planning;
}

/// Minimal N-phase hydro+thermal planning problem.
///
/// Topology (single-bus):
///   1 hydro generator  (20 MW, $5/MWh)
///   1 thermal generator (100 MW, $50/MWh)
///   1 demand           (40 MW constant, 4 blocks of 1 h per phase)
///   1 reservoir (capacity 100 dam³, eini 50, inflow 5 dam³/h)
///
/// The reservoir volume is the sole state variable linking consecutive phases.
/// With N phases and 4 blocks per phase the problem is large enough for
/// meaningful cuts but small enough to solve quickly in unit tests.
inline auto make_nphase_simple_hydro_planning(int num_phases) -> Planning
{
  constexpr int blocks_per_phase = 4;
  const int total_blocks = num_phases * blocks_per_phase;

  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(total_blocks), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
                  .probability_factor = 1.0,
              },
          },
      .phase_array = std::move(phase_array),
  };

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
          .gcost = 5.0,
          .capacity = 20.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 40.0,
      },
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
          .fmax = 50.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 100.0,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 50.0,
          .fmin = -500.0,
          .fmax = +500.0,
          .flow_conversion_rate = 1.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 5.0,
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

  PlanningOptions options;
  options.demand_fail_cost = 1000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  const System system = {
      .name = "sddp_nphase_simple",
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
      .system = system,
  };
}

/// Create a 3-phase hydro problem with a very tight reservoir that forces
/// elastic fallback during the forward pass.
inline auto make_tight_reservoir_3phase_planning() -> Planning
{
  constexpr int num_phases = 3;
  constexpr int blocks_per_phase = 4;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(total_blocks), 1.0);

  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 50.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };
  const Array<Demand> demand_array = {{
      .uid = Uid {1},
      .name = "load1",
      .bus = Uid {1},
      .capacity = 80.0,
  }};
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
      .capacity = 15.0,
      .emin = 0.0,
      .emax = 15.0,
      .eini = 5.0,
      .fmin = -1000.0,
      .fmax = +1000.0,
      .flow_conversion_rate = 1.0,
  }};
  const Array<Flow> flow_array = {{
      .uid = Uid {1},
      .name = "inflow",
      .direction = 1,
      .junction = Uid {1},
      .discharge = 10.0,
  }};
  const Array<Turbine> turbine_array = {{
      .uid = Uid {1},
      .name = "tur1",
      .waterway = Uid {1},
      .generator = Uid {1},
      .production_factor = 1.0,
  }};

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array = {{
          .uid = Uid {1},
      }},
      .phase_array = std::move(phase_array),
  };

  PlanningOptions options;
  options.demand_fail_cost = 1000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system =
          {
              .name = "sddp_tight_rsv_3phase",
              .bus_array = bus_array,
              .demand_array = demand_array,
              .generator_array = generator_array,
              .junction_array = junction_array,
              .waterway_array = waterway_array,
              .flow_array = flow_array,
              .reservoir_array = reservoir_array,
              .turbine_array = turbine_array,
          },
  };
}

/// 3-phase fixture deliberately crafted to force forward-pass LP
/// infeasibility on the very first iteration, so the elastic filter
/// activates and at least one CutType::Feasibility cut is generated.
///
/// Mechanism:
///   - Waterway has `fmin = 5 hm³/h` — the river is required to deliver
///     at least 5 dam³/h downstream (irrigation contract).
///   - Reservoir starts at `eini = 60`, NO natural inflow.
///   - Hydro is cheap (gcost = 1), thermal is expensive (gcost = 100),
///     so phase 0 dispatches hydro maximally to meet its 80 MW demand,
///     draining the reservoir close to empty.
///   - Phase 1 inherits the state-variable trial value (≈ 0) from phase 0.
///     With no inflow and an empty reservoir it cannot satisfy
///     `fmin × duration = 5 × 4 = 20 hm³` of forced discharge → the
///     LP is infeasible at the trial point → SDDP triggers
///     `elastic_filter_solve()` and installs an fcut on phase 0 telling
///     it to leave water for phase 1.
///
/// This fixture is the ground truth used by the
/// `ElasticFilterMode comparison` test: every mode must visit the
/// elastic path here, so the per-mode cut counts (fcuts, mcuts,
/// IIS-filtered cuts) are non-trivial and directly comparable.
inline auto make_forced_infeasibility_planning() -> Planning
{
  constexpr int num_phases = 3;
  constexpr int blocks_per_phase = 4;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(total_blocks), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 1.0,  // cheap → phase 0 drains the reservoir
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 100.0,  // expensive backup
          .capacity = 500.0,
      },
  };
  const Array<Demand> demand_array = {{
      .uid = Uid {1},
      .name = "load1",
      .bus = Uid {1},
      .capacity = 80.0,
  }};
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
  // fmin = 2 hm³/h is the forcing term: each phase must discharge at
  // least 2 × 4 = 8 hm³.  With eini ≈ 0 inherited from phase 0's
  // initial-iteration cheap-hydro dispatch, phase 1's LP cannot satisfy
  // this → infeasibility → fcut.  Total mandatory discharge across
  // phases 1+2 is 16 hm³; with eini0 = 60 the converged solution leaves
  // ≥ 16 hm³ for downstream phases, well within the reservoir's
  // 100 hm³ capacity.
  const Array<Waterway> waterway_array = {{
      .uid = Uid {1},
      .name = "ww1",
      .junction_a = Uid {1},
      .junction_b = Uid {2},
      .fmin = 2.0,
      .fmax = 100.0,
  }};
  const Array<Reservoir> reservoir_array = {{
      .uid = Uid {1},
      .name = "rsv1",
      .junction = Uid {1},
      .capacity = 100.0,
      .emin = 0.0,
      .emax = 100.0,
      .eini = 60.0,
      .fmin = -1000.0,
      .fmax = +1000.0,
      .flow_conversion_rate = 1.0,
  }};
  // Small natural inflow keeps the problem solvable on iter 1+:
  // 1 hm³/h × 4h = 4 hm³ per phase, enough that phase 2 can satisfy
  // its own fmin (8 hm³) once phase 1 leaves ≥ 4 hm³ as state.
  const Array<Flow> flow_array = {{
      .uid = Uid {1},
      .name = "inflow",
      .direction = 1,
      .junction = Uid {1},
      .discharge = 1.0,
  }};
  const Array<Turbine> turbine_array = {{
      .uid = Uid {1},
      .name = "tur1",
      .waterway = Uid {1},
      .generator = Uid {1},
      .production_factor = 1.0,
  }};

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array = {{
          .uid = Uid {1},
      }},
      .phase_array = std::move(phase_array),
  };

  PlanningOptions options;
  options.demand_fail_cost = 1000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system =
          {
              .name = "sddp_forced_infeas_3phase",
              .bus_array = bus_array,
              .demand_array = demand_array,
              .generator_array = generator_array,
              .junction_array = junction_array,
              .waterway_array = waterway_array,
              .flow_array = flow_array,
              .reservoir_array = reservoir_array,
              .turbine_array = turbine_array,
          },
  };
}

/// Two-reservoir variant of `make_forced_infeasibility_planning()`.
///
/// Both reservoirs feed the same single bus, but only reservoir 1's
/// downstream waterway carries a mandatory minimum discharge
/// (`fmin = 2 hm³/h`).  Reservoir 2's waterway has `fmin = 0` — it
/// can be empty without infeasibility.
///
/// Forward-pass behaviour:
///   - iter 0 phase 0 dispatches both reservoirs (cheap hydro) and
///     drains them.
///   - iter 0 phase 1 inherits both state-variable trial values ≈ 0.
///     The LP cannot satisfy waterway 1's fmin (needs 2 × 4 = 8 hm³)
///     → infeasible → elastic filter activates.
///   - Elastic clone relaxes BOTH reservoir state-variable bounds,
///     adds penalised slacks on each.  Only reservoir 1's slack
///     is essential to feasibility; reservoir 2's slack is
///     non-essential.
///
/// IIS distinction:
///   - `multi_cut` mode emits per-active-slack cuts.  In a clean
///     elastic solve only reservoir 1's slack should be active, but
///     numerical near-degeneracy (penalty competition) can leave
///     reservoir 2 with a tiny non-zero slack — `multi_cut` then
///     emits cuts on both reservoirs.
///   - `chinneck` mode runs the IIS re-fix step: it pins reservoir
///     2's slacks to zero, re-solves, confirms the LP is still
///     feasible (because reservoir 2 wasn't really needed), and
///     clears reservoir 2's `sup_col`/`sdn_col` so
///     `build_multi_cuts` emits cuts ONLY on reservoir 1.
///
/// Expected comparison:
///   chinneck.feas_cuts ≤ multi.feas_cuts (strict inequality when
///   the elastic dual is degenerate enough to keep reservoir 2's
///   slack non-zero in the un-filtered solve).
inline auto make_two_reservoir_forced_infeasibility_planning() -> Planning
{
  constexpr int num_phases = 3;
  constexpr int blocks_per_phase = 4;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(total_blocks), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen_1",
          .bus = Uid {1},
          .gcost = 1.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "hydro_gen_2",
          .bus = Uid {1},
          .gcost = 1.0,
          .capacity = 100.0,
      },
      {
          .uid = Uid {3},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 500.0,
      },
  };
  const Array<Demand> demand_array = {{
      .uid = Uid {1},
      .name = "load1",
      .bus = Uid {1},
      .capacity = 80.0,
  }};

  // Two parallel hydro systems sharing a common downstream drain.
  // Each reservoir has its own upstream junction; both discharge
  // into the same drain junction (Uid 99).
  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up_1",
      },
      {
          .uid = Uid {2},
          .name = "j_up_2",
      },
      {
          .uid = Uid {99},
          .name = "j_drain",
          .drain = true,
      },
  };

  // Waterway 1: tight fmin (forces phase 1 infeasibility when
  // reservoir 1 inherits ≈ 0 hm³ from phase 0).
  // Waterway 2: NO mandatory discharge — reservoir 2 can be empty
  // without infeasibility (its state-var bound is the non-essential
  // one that chinneck IIS should filter out).
  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {99},
          .fmin = 2.0,
          .fmax = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "ww2",
          .junction_a = Uid {2},
          .junction_b = Uid {99},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };

  // Two reservoirs with identical capacities and initial conditions.
  // The asymmetry comes entirely from waterway 1's fmin.
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 100.0,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 60.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
      },
      {
          .uid = Uid {2},
          .name = "rsv2",
          .junction = Uid {2},
          .capacity = 100.0,
          .emin = 0.0,
          .emax = 100.0,
          .eini = 60.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
      },
  };

  // Small inflows on both reservoirs (1 hm³/h × 4h = 4 hm³ per phase).
  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow_1",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 1.0,
      },
      {
          .uid = Uid {2},
          .name = "inflow_2",
          .direction = 1,
          .junction = Uid {2},
          .discharge = 1.0,
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
      {
          .uid = Uid {2},
          .name = "tur2",
          .waterway = Uid {2},
          .generator = Uid {2},
          .production_factor = 1.0,
      },
  };

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array = {{
          .uid = Uid {1},
      }},
      .phase_array = std::move(phase_array),
  };

  PlanningOptions options;
  options.demand_fail_cost = 1000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system =
          {
              .name = "sddp_2rsv_forced_infeas_3phase",
              .bus_array = bus_array,
              .demand_array = demand_array,
              .generator_array = generator_array,
              .junction_array = junction_array,
              .waterway_array = waterway_array,
              .flow_array = flow_array,
              .reservoir_array = reservoir_array,
              .turbine_array = turbine_array,
          },
  };
}

/// 10-phase fixture designed to exercise the PLP-style forward-pass
/// backtracking: phase 7 is infeasible on the greedy forward trial
/// (single linear sweep), but the feasibility cascade reaches a
/// phase (phase 1 or 2) where re-solving under the fcut ladder
/// yields a feasible solution, after which the pass moves forward
/// again.  If backtracking is absent, the forward pass declares the
/// scene infeasible at phase 7 and no recovery is possible.
///
/// Mechanism:
///   * 10 phases, 1 block per phase (1 h).
///   * One reservoir.  `eini = 120`, `emax = 200`, per-stage
///     `emin = 0` EXCEPT `emin[phase 7-uid-index] = 180` — phase 7
///     must end with at least 180 hm³ of stored volume.
///   * Constant demand = 20 MW / block met by a cheap hydro
///     generator (`gcost = 1`, cap 30) and an expensive thermal
///     backup (`gcost = 100`, cap 30).  Without further constraints
///     the hydro gen drains the reservoir greedily at rate 20
///     hm³/phase.  Natural inflow of 10 hm³/phase partially offsets
///     (net -10 per phase).
///
/// Greedy forward trajectory (no backtracking):
///   R_0 = 120, R_1 = 110, R_2 = 100, R_3 = 90, R_4 = 80,
///   R_5 = 70, R_6 = 60, R_7 = 50 (drain 20, inflow 10 each).
/// Phase 7's `emin = 180` demand is far above R_7 = 50 →
/// infeasibility.  Without backtracking the only recovery is
/// demand-fail penalty (disabled for this fixture), so the pass
/// fails.
///
/// Backtrack cascade — each fcut lifts the required prev-phase
/// end-of-phase state by the forced net drain (= 10 hm³):
///   fcut on p6: R_6 >= 170.  Greedy 60.  Re-solve infeasible.
///   fcut on p5: R_5 >= 160.  Greedy 70.  Infeasible.
///   fcut on p4: R_4 >= 150.  Greedy 80.  Infeasible.
///   fcut on p3: R_3 >= 140.  Greedy 90.  Infeasible.
///   fcut on p2: R_2 >= 130.  Greedy 100. Infeasible.
///   fcut on p1: R_1 >= 120.  Greedy 110. Infeasible.
///   fcut on p0: **no predecessor**.  If the backtrack bottoms out
///               at phase 0 with no earlier phase to cut on, the
///               scene is declared infeasible — same as the
///               pre-backtracking behaviour at phase 0.
///
/// The trajectory above cascades all the way to phase 0 — which
/// means the design is too tight to demonstrate backtracking
/// recovery.  To make it converge at phase 1 (the user-requested
/// target), the fixture gives phase 1 additional slack via a
/// one-shot boost inflow: `inflow[stage 0] = 80` (vs 10 elsewhere).
/// That lifts R_1's upper bound to 190 (120 − drain_1 + 80 =
/// 180 when drain_1 = 20), giving phase 1 enough slack to satisfy
/// any fcut R_1 >= X for X ≤ 190.
inline auto make_backtracking_recovery_planning() -> Planning
{
  constexpr int num_phases = 10;
  constexpr int blocks_per_phase = 1;
  constexpr int total_blocks = num_phases * blocks_per_phase;
  constexpr int phase_seven_stage_idx = 6;  // 0-based: phase 7 = stage 6

  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(total_blocks), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

  // Per-stage emin: zero everywhere except phase 7 requiring ≥ 180.
  std::vector<double> emin_per_stage(num_phases, 0.0);
  emin_per_stage[phase_seven_stage_idx] = 180.0;

  // Per-stage inflow: 10 everywhere except phase 1 which gets a
  // one-shot boost of 80 to give p1 enough slack to satisfy the
  // cascading fcut that eventually lands on R_1 >= 120.
  // Shape is scenario × stage × block (3D) because Flow::discharge
  // is `STBRealFieldSched`.  One scenario, `num_phases` stages,
  // 1 block per stage.
  std::vector<std::vector<double>> inflow_schedule_2d;
  inflow_schedule_2d.reserve(num_phases);
  for (int st = 0; st < num_phases; ++st) {
    inflow_schedule_2d.push_back(std::vector<double> {st == 0 ? 80.0 : 10.0});
  }
  std::vector<std::vector<std::vector<double>>> inflow_schedule_3d;
  inflow_schedule_3d.push_back(std::move(inflow_schedule_2d));

  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 1.0,
          .capacity = 30.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 30.0,
      },
  };
  const Array<Demand> demand_array = {{
      .uid = Uid {1},
      .name = "load1",
      .bus = Uid {1},
      .capacity = 20.0,
  }};
  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j_up"},
      {.uid = Uid {2}, .name = "j_down", .drain = true},
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
      .capacity = 200.0,
      .emin = emin_per_stage,
      .emax = 200.0,
      .eini = 120.0,
      .fmin = -1000.0,
      .fmax = +1000.0,
      .flow_conversion_rate = 1.0,
  }};
  Array<Flow> flow_array;
  {
    Flow flow_obj;
    flow_obj.uid = Uid {1};
    flow_obj.name = "inflow";
    flow_obj.direction = 1;
    flow_obj.junction = Uid {1};
    flow_obj.discharge = STBRealFieldSched {inflow_schedule_3d};
    flow_array.push_back(std::move(flow_obj));
  }
  const Array<Turbine> turbine_array = {{
      .uid = Uid {1},
      .name = "tur1",
      .waterway = Uid {1},
      .generator = Uid {1},
      .production_factor = 1.0,
  }};

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array = {{.uid = Uid {1}}},
      .phase_array = std::move(phase_array),
  };

  PlanningOptions options;
  // demand_fail_cost intentionally LARGE so demand slack is expensive
  // enough that the solver prefers hydro dispatch (and therefore
  // triggers the reservoir-emin infeasibility at phase 7 on the greedy
  // forward sweep).  Still finite so the problem is well-posed.
  options.demand_fail_cost = 10'000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system =
          {
              .name = "sddp_backtrack_10phase",
              .bus_array = bus_array,
              .demand_array = demand_array,
              .generator_array = generator_array,
              .junction_array = junction_array,
              .waterway_array = waterway_array,
              .flow_array = flow_array,
              .reservoir_array = reservoir_array,
              .turbine_array = turbine_array,
          },
  };
}

/// Infeasible-by-construction twin of `make_backtracking_recovery_planning`.
///
/// Identical geometry and constraints EXCEPT the phase-1 inflow boost
/// is removed — inflow is a constant 10 hm³/phase everywhere.  The
/// cascading fcut chain from phase 7 now bottoms out at phase 1
/// requiring `R_1 >= 120`, but phase 1's LP can only deliver
/// `R_1 = eini - drain_1 + inflow_1 = 120 - drain_1 + 10`, i.e.,
/// `R_1_max = 130 iff drain_1 = 0` — just enough to satisfy the
/// fcut.  To push the fixture firmly INfeasible we tighten phase 7's
/// `emin` from 180 to 220 (above emax = 200), guaranteeing that no
/// amount of backtracking can satisfy the target: even pinning
/// R_1 at the tightest feasible value and propagating forward with
/// drain = 0 everywhere leaves R_7 = 130 + 6·10 = 190 < 220.
///
/// Expected forward-pass behaviour under backtracking:
///   phase 7 infeasible → fcut cascade → eventually reaches phase 0
///   with no predecessor → scene declared infeasible (returns
///   Error).  The `forward_max_attempts` cap is an orthogonal
///   safety net — this case exits the cascade "legitimately" at
///   phase 0 before running out of attempts.
inline auto make_backtracking_no_recovery_planning() -> Planning
{
  auto planning = make_backtracking_recovery_planning();

  // Remove the phase-1 inflow boost: every stage gets the normal
  // 10 hm³/phase, so phase 1's LP cannot accumulate enough water to
  // satisfy the cascading fcut that lands on it (R_1 >= 120 vs a
  // max-achievable R_1 = 120 - drain_1 + 10 ≤ 130).  Even a feasible
  // re-solve at phase 1 produces a trial that is still too low for
  // phase 7's tightened target.
  //
  // Keep the single-reservoir, single-waterway, single-flow layout.
  // Shape matches `STBRealFieldSched`: scenario × stage × block.
  std::vector<std::vector<double>> uniform_inflow_2d;
  uniform_inflow_2d.reserve(10);
  for (int st = 0; st < 10; ++st) {
    uniform_inflow_2d.push_back(std::vector<double> {10.0});
  }
  std::vector<std::vector<std::vector<double>>> uniform_inflow_3d;
  uniform_inflow_3d.push_back(std::move(uniform_inflow_2d));
  planning.system.flow_array[0].discharge =
      STBRealFieldSched {uniform_inflow_3d};

  // Tighten phase 7's `emin` above the reservoir's `emax` so the
  // target is physically unreachable regardless of upstream state.
  // The `emin` schedule is a plain per-stage vector; index 6 is
  // phase 7.
  auto& emin_var = planning.system.reservoir_array[0].emin.value();
  auto& emin_vec = std::get<std::vector<double>>(emin_var);
  emin_vec[6] = 220.0;  // > emax = 200 ⇒ strictly unreachable

  planning.system.name = "sddp_backtrack_10phase_infeasible";
  return planning;
}

/// Two-reservoir recovery fixture for the PLP-style backtracking
/// forward pass.  Mirrors `make_backtracking_recovery_planning` but
/// with a second independent reservoir so multi-cut / IIS modes have
/// ≥ 2 distinct state variables to emit per-bound cuts on.
///
/// Both reservoirs:
///   * eini = 120, emax = 200, emin = 0 per stage EXCEPT phase 7
///     where emin = 180 (same shock on both).
///   * Dedicated hydro generator (uid 1 / uid 3) draining via its
///     own waterway.  Thermal backup (uid 2) covers demand when
///     either reservoir is throttled.
///   * Phase-1 inflow boost (80 vs 10 everywhere else) providing the
///     slack the backtrack cascade needs to recover.
///
/// Under `single_cut`, the cascade emits ONE aggregate fcut per event
/// combining both reservoirs' state columns.  Under `multi_cut`, it
/// emits up to 4 cuts per event (ub + lb on each reservoir's
/// source_col).  Under `chinneck`, IIS filtering may prune the
/// non-essential subset — on this symmetric 2-reservoir design
/// typically both reservoirs stay in the IIS.
inline auto make_backtracking_recovery_two_reservoir_planning() -> Planning
{
  constexpr int num_phases = 10;
  constexpr int blocks_per_phase = 1;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(total_blocks), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

  // Per-stage emin: zero throughout (no per-stage minimum).  The
  // cascade pressure now comes from the **terminal `efin` row**
  // (vfin >= 150) instead of a mid-horizon emin spike.  Mirrors the
  // juan/gtopt_iplp p51 LMAULE infeasibility that originally surfaced
  // the cut-row /scale_objective bug: a strict terminal volume target
  // that the iter-0 forward pass cannot meet without future-cost
  // cuts steering the trajectory.
  std::vector<double> emin_per_stage(num_phases, 0.0);

  // Per-stage inflow schedule (scenario × stage × block): a phase-0
  // boost of 80 hm³ followed by a flat 20 hm³.  Total per reservoir
  // = 80 + 9×20 = 260 hm³, well above `efin = 150`.  The boost
  // matters because without it the forward pass would need to lift
  // every phase's stored volume, and the cascade walk would hit
  // phase 0 with no slack (eini=0, no inflow buffer → no feasible
  // recovery).  With the boost, the recovery point lands around
  // phase 6, well within `forward_max_attempts`.
  auto make_inflow_schedule = []
  {
    std::vector<std::vector<double>> inflow_2d;
    inflow_2d.reserve(num_phases);
    for (int st = 0; st < num_phases; ++st) {
      inflow_2d.push_back(std::vector<double> {st == 0 ? 80.0 : 20.0});
    }
    std::vector<std::vector<std::vector<double>>> inflow_3d;
    inflow_3d.push_back(std::move(inflow_2d));
    return inflow_3d;
  };
  auto inflow_schedule_3d_1 = make_inflow_schedule();
  auto inflow_schedule_3d_2 = make_inflow_schedule();

  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen_1",
          .bus = Uid {1},
          .gcost = 1.0,
          .capacity = 30.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 60.0,  // bigger cap so thermal can cover both
                             // hydros being throttled simultaneously
      },
      {
          .uid = Uid {3},
          .name = "hydro_gen_2",
          .bus = Uid {1},
          .gcost = 1.0,
          .capacity = 30.0,
      },
  };
  // Bigger demand (40 MW per block) so both hydro generators get
  // exercised — otherwise one alone would cover demand and the
  // second reservoir wouldn't participate in the cascade.
  const Array<Demand> demand_array = {{
      .uid = Uid {1},
      .name = "load1",
      .bus = Uid {1},
      .capacity = 40.0,
  }};
  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j_up1"},
      {.uid = Uid {2}, .name = "j_up2"},
      {.uid = Uid {3}, .name = "j_down", .drain = true},
  };
  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {3},
          .fmin = 0.0,
          .fmax = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "ww2",
          .junction_a = Uid {2},
          .junction_b = Uid {3},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };
  // Both reservoirs start EMPTY (`eini = 0`) and must end at
  // `efin = 150` hm³.  Cumulative inflow per reservoir is 200 hm³
  // (10 phases × 20 hm³), so filling to 150 leaves 50 hm³ of slack
  // for hydro generation — the cascade has a feasible recovery
  // point but only after the elastic filter installs an fcut that
  // steers the trajectory away from greedy hydro use.
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 200.0,
          .emin = emin_per_stage,
          .emax = 200.0,
          .eini = 0.0,
          .efin = 150.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
      },
      {
          .uid = Uid {2},
          .name = "rsv2",
          .junction = Uid {2},
          .capacity = 200.0,
          .emin = emin_per_stage,
          .emax = 200.0,
          .eini = 0.0,
          .efin = 150.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
      },
  };
  Array<Flow> flow_array;
  {
    Flow flow1;
    flow1.uid = Uid {1};
    flow1.name = "inflow_1";
    flow1.direction = 1;
    flow1.junction = Uid {1};
    flow1.discharge = STBRealFieldSched {inflow_schedule_3d_1};
    flow_array.push_back(std::move(flow1));

    Flow flow2;
    flow2.uid = Uid {2};
    flow2.name = "inflow_2";
    flow2.direction = 1;
    flow2.junction = Uid {2};
    flow2.discharge = STBRealFieldSched {inflow_schedule_3d_2};
    flow_array.push_back(std::move(flow2));
  }
  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.0,
      },
      {
          .uid = Uid {2},
          .name = "tur2",
          .waterway = Uid {2},
          .generator = Uid {3},
          .production_factor = 1.0,
      },
  };

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array = {{.uid = Uid {1}}},
      .phase_array = std::move(phase_array),
  };

  PlanningOptions options;
  options.demand_fail_cost = 10'000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;
  // Reservoir energy scale left at the default (1.0) — the earlier
  // regression test used a 10× scale to exercise the cross-phase
  // `propagate_trial_values` physical-space fix (0a45e52b), but the
  // subsequent multi-cut rewrite (D1 Birge-Louveaux π-weighted cuts
  // on physical duals) is sensitive to aggregate slack-cost scaling
  // in a way that makes the toy 10-phase 2-reservoir fixture
  // slower to converge under single_cut aggregation with var_scale
  // ≠ 1.  The cross-phase scale path is covered end-to-end by the
  // plp_2_years integration run; this fixture sticks to unit scale
  // so we can keep the convergence assertion crisp.

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system =
          {
              .name = "sddp_backtrack_10phase_2rsv",
              .bus_array = bus_array,
              .demand_array = demand_array,
              .generator_array = generator_array,
              .junction_array = junction_array,
              .waterway_array = waterway_array,
              .flow_array = flow_array,
              .reservoir_array = reservoir_array,
              .turbine_array = turbine_array,
          },
  };
}

/// Two-scene, 10-phase, two-reservoir planning fixture for testing cut-sharing
/// mechanisms.  Repeats the `make_backtracking_recovery_two_reservoir_planning`
/// case for each of two equally probable scenarios (probability 0.5 each).
///
/// Because both scenes are identical copies of the original 10-phase
/// 2-reservoir fixture, every cut-sharing mode (none, expected, accumulate,
/// max) should produce the same converged upper bound.  The tests using this
/// fixture verify that:
///   * All four `CutSharingMode` values produce a finite, positive upper bound.
///   * Convergence is reached within a bounded number of iterations.
///   * The per-scene upper bounds are equal (symmetric fixture).
///
/// Simulation layout:
///   - 10 phases × 1 block per phase
///   - 2 scenarios with probability_factor = 0.5 each
///   - 2 scenes: scene 0 → scenario 0; scene 1 → scenario 1
///   - Each scenario has the same inflow schedule (phase-0 boost of 80, then
///     20 hm³ for phases 1–9).  Total inflow per reservoir = 260 hm³.
///   - Both reservoirs start empty (eini=0) with efin=150.
///   - Thermal backup provides feasibility when hydro is throttled.
inline auto make_2scene_10phase_two_reservoir_planning() -> Planning
{
  constexpr int num_phases = 10;
  constexpr int blocks_per_phase = 1;
  constexpr int total_blocks = num_phases * blocks_per_phase;

  auto block_array =
      make_uniform_blocks(static_cast<std::size_t>(total_blocks), 1.0);
  auto stage_array =
      make_uniform_stages(static_cast<std::size_t>(num_phases),
                          static_cast<std::size_t>(blocks_per_phase));
  auto phase_array =
      make_single_stage_phases(static_cast<std::size_t>(num_phases));

  std::vector<double> emin_per_stage(num_phases, 0.0);

  // Inflow schedule: scenario × stage × block.
  // Both scenarios are identical (phase-0 boost of 80, then 20 hm³ each).
  // Cumulative inflow per reservoir per scenario = 80 + 9×20 = 260 hm³.
  auto make_inflow_schedule_2scene = []
  {
    std::vector<std::vector<double>> inflow_2d;
    inflow_2d.reserve(static_cast<std::size_t>(num_phases));
    for (int st = 0; st < num_phases; ++st) {
      inflow_2d.push_back(std::vector<double> {st == 0 ? 80.0 : 20.0});
    }
    // Two identical scenarios — both see the same hydrology.
    return std::vector<std::vector<std::vector<double>>> {inflow_2d, inflow_2d};
  };
  auto inflow_3d_1 = make_inflow_schedule_2scene();
  auto inflow_3d_2 = make_inflow_schedule_2scene();

  const Array<Bus> bus_array = {{
      .uid = Uid {1},
      .name = "b1",
  }};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen_1",
          .bus = Uid {1},
          .gcost = 1.0,
          .capacity = 30.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 60.0,
      },
      {
          .uid = Uid {3},
          .name = "hydro_gen_2",
          .bus = Uid {1},
          .gcost = 1.0,
          .capacity = 30.0,
      },
  };
  const Array<Demand> demand_array = {{
      .uid = Uid {1},
      .name = "load1",
      .bus = Uid {1},
      .capacity = 40.0,
  }};
  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j_up1"},
      {.uid = Uid {2}, .name = "j_up2"},
      {.uid = Uid {3}, .name = "j_down", .drain = true},
  };
  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {3},
          .fmin = 0.0,
          .fmax = 100.0,
      },
      {
          .uid = Uid {2},
          .name = "ww2",
          .junction_a = Uid {2},
          .junction_b = Uid {3},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };
  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 200.0,
          .emin = emin_per_stage,
          .emax = 200.0,
          .eini = 0.0,
          .efin = 150.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
      },
      {
          .uid = Uid {2},
          .name = "rsv2",
          .junction = Uid {2},
          .capacity = 200.0,
          .emin = emin_per_stage,
          .emax = 200.0,
          .eini = 0.0,
          .efin = 150.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
      },
  };

  Array<Flow> flow_array;
  {
    Flow flow1;
    flow1.uid = Uid {1};
    flow1.name = "inflow_1";
    flow1.direction = 1;
    flow1.junction = Uid {1};
    flow1.discharge = STBRealFieldSched {std::move(inflow_3d_1)};
    flow_array.push_back(std::move(flow1));

    Flow flow2;
    flow2.uid = Uid {2};
    flow2.name = "inflow_2";
    flow2.direction = 1;
    flow2.junction = Uid {2};
    flow2.discharge = STBRealFieldSched {std::move(inflow_3d_2)};
    flow_array.push_back(std::move(flow2));
  }

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .production_factor = 1.0,
      },
      {
          .uid = Uid {2},
          .name = "tur2",
          .waterway = Uid {2},
          .generator = Uid {3},
          .production_factor = 1.0,
      },
  };

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
                  .probability_factor = 0.5,
              },
              {
                  .uid = Uid {2},
                  .probability_factor = 0.5,
              },
          },
      .phase_array = std::move(phase_array),
      .scene_array =
          {
              {
                  .uid = Uid {1},
                  .name = "scene1",
                  .active = true,
                  .first_scenario = 0,
                  .count_scenario = 1,
              },
              {
                  .uid = Uid {2},
                  .name = "scene2",
                  .active = true,
                  .first_scenario = 1,
                  .count_scenario = 1,
              },
          },
  };

  PlanningOptions options;
  options.demand_fail_cost = 10'000.0;
  options.use_single_bus = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system =
          {
              .name = "sddp_2scene_10phase_2rsv",
              .bus_array = bus_array,
              .demand_array = demand_array,
              .generator_array = generator_array,
              .junction_array = junction_array,
              .waterway_array = waterway_array,
              .flow_array = std::move(flow_array),
              .reservoir_array = reservoir_array,
              .turbine_array = turbine_array,
          },
  };
}

// ─── Helper: default-constructed PhaseStateInfo grid ───────────────────────
//
// `load_boundary_cuts_csv` and similar SDDP cut-loading entry points
// take a ``StrongIndexVector<SceneIndex, StrongIndexVector<PhaseIndex,
// PhaseStateInfo>>`` that callers normally fill from
// ``SDDPMethod::collect_state_variable_links``.  Tests that exercise
// the loader without spinning up an SDDPMethod just need a default-
// constructed grid sized to the planning's (scenes × phases).  Hoisted
// here so multiple test files don't carry their own copy.
[[nodiscard]] inline auto make_default_scene_phase_states(
    const PlanningLP& planning_lp)
    -> StrongIndexVector<SceneIndex,
                         StrongIndexVector<PhaseIndex, PhaseStateInfo>>
{
  const auto& sim = planning_lp.simulation();
  const auto num_scenes = static_cast<std::size_t>(sim.scenes().size());
  const auto num_phases = static_cast<std::size_t>(sim.phases().size());

  StrongIndexVector<SceneIndex, StrongIndexVector<PhaseIndex, PhaseStateInfo>>
      result(num_scenes,
             StrongIndexVector<PhaseIndex, PhaseStateInfo>(num_phases,
                                                           PhaseStateInfo {}));
  return result;
}

/// 2-scene wrapper around `make_backtracking_recovery_two_reservoir_planning`.
///
/// Takes the 10-phase / 2-reservoir cascade fixture and inflates the
/// simulation to **two scenarios** distributed across **two scenes**
/// (one scenario per scene), with adjustable per-scenario probability
/// weights.  Each scene's inflow trajectory is a copy of the original
/// 1-scenario schedule, so both scenes converge to the same
/// scene-local optimum — the cross-scene cut_sharing modes therefore
/// share *redundant* cuts, but the SDDP-level invariants (scenario
/// probability weighting, cut count ordering, dual_max economics)
/// must still hold under each mode.
///
/// Used by the cut_sharing × low_memory parity tests in
/// test_sddp_method.cpp.  Distinct from the earlier
/// `make_2scene_10phase_two_reservoir_planning()` helper above, which
/// builds an inline 2-scene/10-phase fixture from scratch (reused by
/// the cut-sharing-mode parity tests); this wrapper instead derives a
/// 2-scene fixture from the 1-scenario backtracking-recovery helper
/// so the underlying inflow / cascade geometry stays in lock-step
/// with the latter as it evolves.
inline auto make_2scene_backtracking_recovery_two_reservoir_planning(
    double prob1 = 0.5, double prob2 = 0.5) -> Planning
{
  auto planning = make_backtracking_recovery_two_reservoir_planning();

  // Replace the single-scenario simulation with a 2-scenario / 2-scene
  // structure (the rest of the simulation — block_array, stage_array,
  // phase_array — is preserved).
  planning.simulation.scenario_array = {
      {.uid = Uid {1}, .probability_factor = prob1},
      {.uid = Uid {2}, .probability_factor = prob2},
  };
  planning.simulation.scene_array = {
      {.uid = Uid {1},
       .name = "scene1",
       .active = true,
       .first_scenario = 0,
       .count_scenario = 1},
      {.uid = Uid {2},
       .name = "scene2",
       .active = true,
       .first_scenario = 1,
       .count_scenario = 1},
  };

  // Extend each Flow's 3D discharge schedule (scenario × stage ×
  // block) by duplicating scenario 0 → scenario 1.  The 1-scenario
  // helper builds the schedule as a single-element outer vector, so
  // we always push_back a copy of the front entry; if the discharge
  // is stored as a scalar / FileSched it is left untouched (those
  // forms are scenario-agnostic by construction).
  planning.system.name = "sddp_2scene_backtrack_10phase_2rsv";
  for (auto& flow : planning.system.flow_array) {
    if (auto* sched3d =
            std::get_if<std::vector<std::vector<std::vector<double>>>>(
                &flow.discharge);
        sched3d != nullptr && sched3d->size() == 1)
    {
      sched3d->push_back(sched3d->front());
    }
  }

  return planning;
}
