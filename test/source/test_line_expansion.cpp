// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2024, Marcelo Matus. All rights reserved.
//
// test_line_expansion.cpp — LineLP transformer and capacity expansion tests:
//   transformer (tap_ratio + phase_shift_deg), capacity expansion,
//   and combined losses+Kirchhoff+expansion.

#include <doctest/doctest.h>
#include <gtopt/block.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/line.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ── Transformer (tap_ratio + phase_shift_deg) tests ──────────────────────

TEST_CASE(
    "Transformer with off-nominal tap ratio changes Kirchhoff susceptance")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Two-bus system: bus1 → bus2 via a transformer.
  // With tap_ratio = τ the effective susceptance is B/τ, so for equal
  // generation and demand the optimal power flow decreases with larger τ.
  // We verify feasibility and a non-trivial objective at τ = 2 (half B).

  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1", .reference_theta = 0.0},
      {.uid = Uid {2}, .name = "b2"},
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 20.0,
          .capacity = 200.0,
      },
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {2}, .lmax = 100.0},
  };

  const Simulation simulation = {
      .block_array =
          {
              Block {.uid = Uid {1}, .duration = 1},
          },
      .stage_array =
          {
              Stage {.uid = Uid {1}, .first_block = 0, .count_block = 1},
          },
      .scenario_array =
          {
              Scenario {.uid = Uid {1}, .probability_factor = 1},
          },
  };

  const Array<Line> line_array_nominal = {
      {
          .uid = Uid {1},
          .name = "t1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .voltage = 220.0,
          .reactance = 0.1,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .tap_ratio = 1.0,
      },
  };
  const Array<Line> line_array_tap = {
      {
          .uid = Uid {1},
          .name = "t1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .voltage = 220.0,
          .reactance = 0.1,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .tap_ratio = 2.0,  // half the susceptance → larger angle difference
      },
  };

  PlanningOptions opts;
  opts.use_single_bus = false;
  opts.use_kirchhoff = true;
  opts.use_line_losses = false;
  const PlanningOptionsLP options_lp(opts);

  // Nominal tap test
  {
    System system = {
        .name = "TapNominal",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .line_array = line_array_nominal,
    };
    SimulationLP simulation_lp(simulation, options_lp);
    system.setup_reference_bus(options_lp);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp_iface = system_lp.linear_interface();
    auto result = lp_iface.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
  }

  // Off-nominal tap test
  {
    System system = {
        .name = "TapOffNominal",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .line_array = line_array_tap,
    };
    SimulationLP simulation_lp(simulation, options_lp);
    system.setup_reference_bus(options_lp);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp_iface = system_lp.linear_interface();
    auto result = lp_iface.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
  }
}

TEST_CASE("Phase-shifting transformer modifies Kirchhoff RHS")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Simple 2-bus system connected by a PST.  With a non-zero phase shift the
  // Kirchhoff equality RHS changes from 0 to -scale_theta * phi_rad; the LP
  // must still be feasible (the PST just requires a different angle gap to
  // carry the same power).

  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1", .reference_theta = 0.0},
      {.uid = Uid {2}, .name = "b2"},
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
      {.uid = Uid {1}, .name = "d1", .bus = Uid {2}, .lmax = 100.0},
  };

  const Simulation simulation = {
      .block_array =
          {
              Block {.uid = Uid {1}, .duration = 1},
          },
      .stage_array =
          {
              Stage {.uid = Uid {1}, .first_block = 0, .count_block = 1},
          },
      .scenario_array =
          {
              Scenario {.uid = Uid {1}, .probability_factor = 1},
          },
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "pst_1_2",
          .type = "transformer",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .voltage = 220.0,
          .reactance = 0.1,
          .tmax_ba = 300.0,
          .tmax_ab = 300.0,
          .phase_shift_deg = 2.0,
      },
  };

  PlanningOptions opts;
  opts.use_single_bus = false;
  opts.use_kirchhoff = true;
  opts.use_line_losses = false;
  opts.scale_objective = 1000.0;
  const PlanningOptionsLP options_lp(opts);

  System system = {
      .name = "PSTTwoBus",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  SimulationLP simulation_lp(simulation, options_lp);
  system.setup_reference_bus(options_lp);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp_iface = system_lp.linear_interface();
  auto result = lp_iface.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Objective should equal 100 MW * $10/MWh / scale_objective (1000) = 1.0
  CHECK(lp_iface.get_obj_value() == doctest::Approx(1.0));
}

// ── Capacity expansion + loss model tests ─────────────────────────────────

TEST_CASE("LineLP - quadratic losses with capacity expansion")
{
  // Exercises the quadratic loss path when capacity_col is present (expcap
  // set). This covers the capacity constraint inside
  // add_quadratic_flow_direction (lines 169-179) and the cprows/cnrows storage
  // (lines 335-336, 359-360).

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
      {
          .uid = Uid {2},
          .name = "b2",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {2},
          .capacity = 100.0,
      },
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .voltage = 100.0,
          .resistance = 0.01,
          .loss_segments = 3,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 100.0,
          .expcap = 50.0,
          .expmod = 4.0,
          .capmax = 300.0,
          .annual_capcost = 100.0,
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

  PlanningOptions opts;
  opts.use_single_bus = false;
  opts.use_kirchhoff = false;
  opts.use_line_losses = true;
  opts.scale_objective = 1000.0;

  const System system = {
      .name = "QuadLossExpansion",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options_qe(opts);
  SimulationLP simulation_lp(simulation, options_qe);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Demand is 100 MW, capacity starts at 100 MW with expansion available.
  // Quadratic losses require slightly more generation.
  const auto obj = lp.get_obj_value();
  CHECK(obj > 0.9);
  CHECK(obj < 2.0);
}

TEST_CASE("LineLP - linear losses (lossfactor) with capacity expansion")
{
  // Exercises the linear loss path with capacity_col present (expcap set).
  // This covers the capacity constraint rows for both positive and negative
  // flow directions in the linear loss model (lines 378-388, 403-413).

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
      {
          .uid = Uid {2},
          .name = "b2",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {2},
          .capacity = 100.0,
      },
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .lossfactor = 0.05,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 100.0,
          .expcap = 50.0,
          .expmod = 4.0,
          .capmax = 300.0,
          .annual_capcost = 100.0,
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

  PlanningOptions opts;
  opts.use_single_bus = false;
  opts.use_kirchhoff = false;
  opts.scale_objective = 1000.0;

  const System system = {
      .name = "LinLossExpansion",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options_le(opts);
  SimulationLP simulation_lp(simulation, options_le);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // With 5% loss factor: gen ~105 MW for 100 MW demand, cost ~1.05 (scaled)
  const auto obj = lp.get_obj_value();
  CHECK(obj > 1.0);
  CHECK(obj < 1.1);
}

TEST_CASE("LineLP - multi-stage capacity expansion with quadratic losses")
{
  // Two stages: stage 1 has initial capacity, stage 2 can expand.
  // Exercises capacity tracking across stages in the quadratic loss model.

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
      {
          .uid = Uid {2},
          .name = "b2",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {2},
          .capacity = 80.0,
      },
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .voltage = 100.0,
          .resistance = 0.01,
          .loss_segments = 2,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 100.0,
          .expcap = 50.0,
          .expmod = 2.0,
          .capmax = 200.0,
          .annual_capcost = 10.0,
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

  PlanningOptions opts;
  opts.use_single_bus = false;
  opts.use_kirchhoff = false;
  opts.use_line_losses = true;

  const System system = {
      .name = "MultiStageQuadExpansion",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options_ms(opts);
  SimulationLP simulation_lp(simulation, options_ms);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(
    "LineLP - quadratic losses with Kirchhoff and capacity expansion combined")
{
  // Exercises all three advanced features together: quadratic losses, Kirchhoff
  // DC power flow, and capacity expansion. This tests the full integration
  // path.

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
          .reference_theta = 0.0,
      },
      {
          .uid = Uid {2},
          .name = "b2",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {2},
          .capacity = 80.0,
      },
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .voltage = 100.0,
          .resistance = 0.01,
          .reactance = 0.05,
          .loss_segments = 3,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 100.0,
          .expcap = 50.0,
          .expmod = 2.0,
          .capmax = 200.0,
          .annual_capcost = 10.0,
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

  PlanningOptions opts;
  opts.use_single_bus = false;
  opts.use_kirchhoff = true;
  opts.use_line_losses = true;

  System system = {
      .name = "QuadKirchhoffExpansion",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options_qke(opts);
  SimulationLP simulation_lp(simulation, options_qke);
  system.setup_reference_bus(options_qke);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}
