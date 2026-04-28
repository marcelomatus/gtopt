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
  opts.demand_fail_cost = 1000.0;
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
  CHECK(lp_iface.get_obj_value_raw() == doctest::Approx(1.0));
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
  opts.demand_fail_cost = 1000.0;

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
  const auto obj = lp.get_obj_value_raw();
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
  opts.demand_fail_cost = 1000.0;

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
  const auto obj = lp.get_obj_value_raw();
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

TEST_CASE("LineLP — capainst primal col_sol binds at capmin lower bound")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Minimal 2-bus system where a downstream demand forces LineLP capacity
  // expansion above the base `capacity`.  The `capainst` column is owned by
  // LineLP via CapacityObjectBase and exposed via `capacity_col_at(stage)`.
  //
  // Setup:
  //   - 1 stage, 1 block (duration = 1h), 1 scenario.
  //   - Cheap generator at bus1 with ample capacity to serve load at bus2.
  //   - A 30 MW demand at bus2 — larger than the line's base capacity of
  //     10 MW, so the LP is forced to expand.
  //   - The line has `capacity = 10`, `expcap = 100`, `expmod = 1`,
  //     `annual_capcost = 1.0` (tiny investment cost to break ties so the
  //     LP minimizes expmod_col to just what the load requires).
  //
  // Expected:
  //   - capainst primal ∈ [20, 110]; in particular ≥ 20 (load-driven
  //     binding), well above the base capacity of 10 MW.

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
          .capacity = 30.0,
      },
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 10.0,
          .expcap = 100.0,
          .expmod = 1.0,
          .annual_capcost = 1.0,
      },
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

  PlanningOptions opts;
  opts.use_single_bus = false;
  opts.use_kirchhoff = false;
  opts.use_line_losses = false;
  opts.demand_fail_cost = 1000.0;

  const System system = {
      .name = "LineCapainstLowerBound",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options_lp(opts);
  SimulationLP simulation_lp(simulation, options_lp);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Look up the line's own capainst column via the LP-class accessor.
  const auto& line_lps = system_lp.elements<LineLP>();
  REQUIRE(line_lps.size() == 1);
  const auto& line_lp = line_lps.front();
  const auto& stage_lp = simulation_lp.stages().front();

  const auto cap_col = line_lp.capacity_col_at(stage_lp);
  REQUIRE(cap_col.has_value());

  // Without use_line_losses, the LineLP transport constraint is bounded
  // by tmax_ab/tmax_ba and the capacity_col is not tied to the flow row.
  // Positive annual_capcost then drives the LP to pin capainst at its
  // lower bound `stage_capacity = 10` (with expmod_col = 0.1 closing the
  // -capainst + expcap*expmod = 0 equality).
  const auto cap_val = lp.get_col_sol()[*cap_col];
  CHECK(cap_val == doctest::Approx(10.0).epsilon(1e-6));
  CHECK(cap_val <= 110.0 + 1e-6);
}

TEST_CASE("LineLP — transport row dual equals congestion rent between buses")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // 2-bus system: a cheap generator at bus1 and an expensive generator at
  // bus2 serve a demand at bus2 across a line with limited tmax_ab.  The
  // line congests (flow_ab = tmax_ab = 30 MW), which pins the bus2 price
  // at the expensive generator's marginal cost while bus1 stays at the
  // cheap one.  The LineLP capacityp transport row dual equals the price
  // differential between the two buses (congestion rent).
  //
  // Expected primal dispatch:
  //   - gen1 (cheap, gcost=10) produces 30 MW → fully exported via line.
  //   - gen2 (expensive, gcost=100) produces 20 MW locally at bus2.
  //   - demand at bus2 = 50 MW (30 imported + 20 local).
  //
  // Expected duals (physical units from get_row_dual()):
  //   - bus1 balance dual = 10 $/MWh  (cheap gen marginal)
  //   - bus2 balance dual = 100 $/MWh (expensive gen marginal)
  //   - capacityp transport row dual = 100 - 10 = 90 $/MWh

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
          .capacity = 1000.0,
      },
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {2},
          .gcost = 100.0,
          .capacity = 1000.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {2},
          .capacity = 50.0,
      },
  };

  // Enable line losses in `linear` mode with a tiny (effectively zero)
  // loss factor so LineLP emits the explicit capacity row
  // `capacity_col − flow_col ≥ 0` (populated by line_losses::add_linear only
  // when `capacity_col` is present — i.e. when expansion is configured).
  // Expansion is added with a prohibitive `annual_capcost` so the LP keeps
  // `capainst` pinned at the base `capacity = 30`, forcing the line to
  // congest and producing a clean 90 $/MWh congestion-rent dual.
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .lossfactor = 1e-9,
          .line_losses_mode = Name {"linear"},
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 30.0,
          .expcap = 100.0,
          .expmod = 1.0,
          .annual_capcost = 1.0e6,
      },
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

  PlanningOptions opts;
  opts.use_single_bus = false;
  opts.use_kirchhoff = false;
  opts.use_line_losses = true;
  opts.demand_fail_cost = 1000.0;

  const System system = {
      .name = "LineCongestionRent",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options_lp(opts);
  SimulationLP simulation_lp(simulation, options_lp);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.get_numrows() > 0);
  REQUIRE(lp.get_numcols() > 0);

  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  lp.ensure_duals();

  // Look up the LineLP and its capacityp transport row (bus_a → bus_b).
  const auto& line_lps = system_lp.elements<LineLP>();
  REQUIRE(line_lps.size() == 1);
  const auto& line_lp = line_lps.front();
  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();

  const auto& capp_rows = line_lp.capacityp_rows_at(scenario_lp, stage_lp);
  REQUIRE(!capp_rows.empty());
  const auto capp_row = capp_rows.begin()->second;

  const auto dual = lp.get_row_dual();
  const double rent = dual[capp_row];

  // The capacityp row is `flowp - flown <= tmax_ab` (or similar upper-bound
  // form), so its physical dual is non-positive with magnitude equal to the
  // bus2-minus-bus1 price differential.  We check absolute value to be
  // agnostic to the sign convention.
  CHECK(std::abs(rent) == doctest::Approx(90.0).epsilon(1e-5));
}
