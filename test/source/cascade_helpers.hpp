// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      cascade_helpers.hpp
 * @brief     Shared helper functions for cascade test files
 * @date      2026-04-05
 *
 * Provides make_*_planning() factory functions used by multiple cascade test
 * files.  Extracted to avoid duplicate definitions across unity build batches.
 */

#pragma once

#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_method.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

/// Create a 3-phase 2-bus hydro system with a transmission line.
/// Bus 1: hydro generator + reservoir.  Bus 2: demand + thermal backup.
/// Line connects bus 1 → bus 2 with limited capacity.
inline auto make_3phase_2bus_hydro_planning() -> Planning
{
  // ── Blocks: 6 per phase × 3 phases = 18 total ──
  Array<Block> block_array;
  for (int i = 0; i < 18; ++i) {
    block_array.push_back(Block {
        .uid = Uid {i + 1},
        .duration = 1.0,
    });
  }

  Array<Stage> stage_array = {
      Stage {
          .uid = Uid {1},
          .first_block = 0,
          .count_block = 6,
      },
      Stage {
          .uid = Uid {2},
          .first_block = 6,
          .count_block = 6,
      },
      Stage {
          .uid = Uid {3},
          .first_block = 12,
          .count_block = 6,
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
          .name = "bus_hydro",
      },
      {
          .uid = Uid {2},
          .name = "bus_demand",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 80.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {2},
          .gcost = 50.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load1",
          .bus = Uid {2},
          .capacity = 60.0,
      },
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "line_1_2",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .reactance = 0.05,
          .tmax_ba = 100.0,
          .tmax_ab = 100.0,
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
          .capacity = 300.0,
          .emin = 0.0,
          .emax = 300.0,
          .eini = 150.0,
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
  options.demand_fail_cost = 1000.0;
  options.use_single_bus = OptBool {false};
  options.use_kirchhoff = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  System system = {
      .name = "cascade_2bus_hydro",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
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

/// 6-phase variant of the 2-bus hydro planning for cascade tests that need
/// more Benders iterations (more phases ⇒ more state links ⇒ harder for cuts).
inline auto make_6phase_2bus_hydro_planning() -> Planning
{
  constexpr Size num_phases = 6;
  constexpr Size blocks_per_phase = 4;
  constexpr Size total_blocks = num_phases * blocks_per_phase;

  Array<Block> block_array;
  for (Size i = 0; i < total_blocks; ++i) {
    block_array.push_back(Block {
        .uid = Uid {static_cast<int>(i) + 1},
        .duration = 1.0,
    });
  }

  Array<Stage> stage_array;
  Array<Phase> phase_array;
  for (Size p = 0; p < static_cast<Size>(num_phases); ++p) {
    stage_array.push_back(Stage {
        .uid = Uid {static_cast<int>(p) + 1},
        .first_block = p * blocks_per_phase,
        .count_block = blocks_per_phase,
    });
    phase_array.push_back(Phase {
        .uid = Uid {static_cast<int>(p) + 1},
        .first_stage = p,
        .count_stage = 1,
    });
  }

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "bus_hydro",
      },
      {
          .uid = Uid {2},
          .name = "bus_demand",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 80.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {2},
          .gcost = 50.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load1",
          .bus = Uid {2},
          .capacity = 60.0,
      },
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "line_1_2",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .reactance = 0.05,
          .tmax_ba = 100.0,
          .tmax_ab = 100.0,
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
          .capacity = 500.0,
          .emin = 0.0,
          .emax = 500.0,
          .eini = 250.0,
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
  options.demand_fail_cost = 1000.0;
  options.use_single_bus = OptBool {false};
  options.use_kirchhoff = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  System system = {
      .name = "cascade_6ph_2bus_hydro",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
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
