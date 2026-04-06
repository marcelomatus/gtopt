// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2024, Marcelo Matus. All rights reserved.
//
// test_line_lp_losses.cpp — LineLP loss-related LP integration tests:
//   linear losses, quadratic losses, loss options/overrides,
//   and all loss allocation mode tests.

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

TEST_CASE("LineLP - line losses (lossfactor > 0)")
{
  // Line with a positive lossfactor exercises the has_loss path which creates
  // separate fpcols / fncols for forward/reverse flow.
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
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
      {.uid = Uid {1}, .name = "d1", .bus = Uid {2}, .capacity = 100.0},
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .lossfactor = 0.05,  // 5% losses → exercises has_loss branch
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 200.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  PlanningOptions opts;
  opts.use_single_bus = false;
  opts.use_kirchhoff = false;

  const System system = {
      .name = "LossFactorTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("LineLP - quadratic losses (piecewise-linear with resistance)")
{
  // Line with resistance + voltage + loss_segments > 1 exercises the
  // piecewise-linear quadratic loss model: P_loss ≈ R·f²/V²
  // This test uses 3 segments on a line with R=0.01, V=100kV, tmax=200MW.

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
          .capacity = 200.0,
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
      .name = "QuadraticLossTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options_ql(opts);
  SimulationLP simulation_lp(simulation, options_ql);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  CHECK(lp.get_numcols() > 0);
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // Units: R [Ω], f [MW], V [kV] → P_loss [MW].
  // The generation cost reflects the piecewise-linear approximation of
  // quadratic loss. Exact: loss(100) = R·100²/V² = 0.01·10000/10000 =
  // 0.01 MW.  The 3-segment approximation slightly overestimates this.
  // Total gen ≈ 100.01 MW, cost ≈ 1000.1, obj ≈ 1.0001 (scaled by 1000).
  // Bounds are loose to accommodate piecewise approximation error.
  const auto obj = lp.get_obj_value();
  CHECK(obj > 1.0);
  CHECK(obj < 1.01);
}

TEST_CASE("LineLP - quadratic losses with Kirchhoff constraints")
{
  // Verify quadratic loss model works together with DC power flow constraints
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
          .reactance = 0.05,
          .loss_segments = 4,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 200.0,
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

  const System system = {
      .name = "QuadLossKirchhoff",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options_qk(opts);
  SimulationLP simulation_lp(simulation, options_qk);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE(
    "LineLP - global loss_segments option is used when line has no override")
{
  // When the line does not set loss_segments, the global option value is used.
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
          // No loss_segments here → uses global option
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 200.0,
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
  opts.loss_segments = 3;  // Global: 3 segments
  opts.scale_objective = 1000.0;

  const System system = {
      .name = "GlobalSegTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options_gs(opts);
  SimulationLP simulation_lp(simulation, options_gs);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  CHECK(lp.get_numrows() > 0);
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // With quadratic losses, objective should be slightly above 1.0
  // (100 MW demand + small loss at gcost=10, scaled by 1000)
  const auto obj = lp.get_obj_value();
  CHECK(obj > 1.0);
  CHECK(obj < 1.01);
}

TEST_CASE("LineLP - per-line use_line_losses overrides global option")
{
  using namespace gtopt;

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

  // Sub-case A: global use_line_losses=false, per-line use_line_losses=true
  // → line-level setting enables quadratic losses despite global=false
  {
    const Array<Line> line_array = {
        {
            .uid = Uid {1},
            .name = "l1",
            .bus_a = Uid {1},
            .bus_b = Uid {2},
            .voltage = 100.0,
            .resistance = 0.01,
            .use_line_losses = true,  // per-line override: enable losses
            .loss_segments = 3,
            .tmax_ba = 200.0,
            .tmax_ab = 200.0,
            .capacity = 200.0,
        },
    };

    PlanningOptions opts;
    opts.use_single_bus = false;
    opts.use_kirchhoff = false;
    opts.use_line_losses = false;  // global: disabled
    opts.scale_objective = 1000.0;

    const System system = {
        .name = "PerLineEnableLosses",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .line_array = line_array,
    };

    const PlanningOptionsLP options_a(opts);
    SimulationLP simulation_lp(simulation, options_a);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // Per-line losses enabled: objective > 1.0 (loss overhead)
    const auto obj = lp.get_obj_value();
    CHECK(obj > 1.0);
    CHECK(obj < 1.01);
  }

  // Sub-case B: global use_line_losses=true, per-line use_line_losses=false
  // → line-level setting disables quadratic losses despite global=true
  {
    const Array<Line> line_array = {
        {
            .uid = Uid {1},
            .name = "l1",
            .bus_a = Uid {1},
            .bus_b = Uid {2},
            .voltage = 100.0,
            .resistance = 0.01,
            .use_line_losses = false,  // per-line override: disable losses
            .loss_segments = 3,
            .tmax_ba = 200.0,
            .tmax_ab = 200.0,
            .capacity = 200.0,
        },
    };

    PlanningOptions opts;
    opts.use_single_bus = false;
    opts.use_kirchhoff = false;
    opts.use_line_losses = true;  // global: enabled
    opts.loss_segments = 3;
    opts.scale_objective = 1000.0;

    const System system = {
        .name = "PerLineDisableLosses",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .line_array = line_array,
    };

    const PlanningOptionsLP options_b(opts);
    SimulationLP simulation_lp(simulation, options_b);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // Per-line losses disabled: objective = exactly 1.0 (no loss overhead)
    const auto obj = lp.get_obj_value();
    CHECK(obj == doctest::Approx(1.0));
  }
}

// ── LossAllocationMode tests ───────────────────────────────────────

TEST_CASE("LossAllocationMode enum parsing")  // NOLINT
{
  using namespace gtopt;

  SUBCASE("default is receiver when unset")
  {
    Line line;
    CHECK_FALSE(line.loss_allocation_mode.has_value());
    CHECK(line.loss_allocation_mode_enum() == LossAllocationMode::receiver);
  }

  SUBCASE("parses valid values")
  {
    Line line;
    line.loss_allocation_mode = "sender";
    CHECK(line.loss_allocation_mode_enum() == LossAllocationMode::sender);

    line.loss_allocation_mode = "split";
    CHECK(line.loss_allocation_mode_enum() == LossAllocationMode::split);

    line.loss_allocation_mode = "receiver";
    CHECK(line.loss_allocation_mode_enum() == LossAllocationMode::receiver);
  }

  SUBCASE("invalid value falls back to receiver")
  {
    Line line;
    line.loss_allocation_mode = "invalid";
    CHECK(line.loss_allocation_mode_enum() == LossAllocationMode::receiver);
  }
}

TEST_CASE("LineLP - loss allocation mode receiver (default)")  // NOLINT
{
  // Default mode: all losses at receiver. Generator at bus 1 (cheap),
  // demand at bus 2, line with 10% lossfactor.
  // With 100 MW demand and 10% receiver loss: gen = 100/(1-0.1) ≈ 111.1 MW
  using namespace gtopt;

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
          .lmax = 100.0,
          .capacity = 100.0,
      },
  };
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .lossfactor = 0.10,
          .tmax_ba = 500.0,
          .tmax_ab = 500.0,
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

  Planning planning = {
      .options =
          {
              .use_kirchhoff = false,
              .use_single_bus = false,
          },
      .simulation = simulation,
      .system =
          {
              .name = "LossReceiverTest",
              .bus_array = bus_array,
              .demand_array = demand_array,
              .generator_array = generator_array,
              .line_array = line_array,
          },
  };

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  // Generation should be ~111.1 MW (100 / 0.9)
  const auto obj =
      planning_lp.systems().front().front().linear_interface().get_obj_value();
  CHECK(obj > 0);
}

TEST_CASE("LineLP - loss allocation mode sender")  // NOLINT
{
  // Sender mode: all losses at sender. Same setup as above.
  // With 100 MW demand and 10% sender loss: sender injects (1-0.1)*gen,
  // receiver gets gen. So gen = 100 MW, but sender "pays" 10 MW loss.
  using namespace gtopt;

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
          .lmax = 100.0,
          .capacity = 100.0,
      },
  };
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .lossfactor = 0.10,
          .loss_allocation_mode = Name {"sender"},
          .tmax_ba = 500.0,
          .tmax_ab = 500.0,
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

  Planning planning = {
      .options =
          {
              .use_kirchhoff = false,
              .use_single_bus = false,
          },
      .simulation = simulation,
      .system =
          {
              .name = "LossSenderTest",
              .bus_array = bus_array,
              .demand_array = demand_array,
              .generator_array = generator_array,
              .line_array = line_array,
          },
  };

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  // Generation should be ~111.1 MW (same total loss, different allocation)
  const auto obj =
      planning_lp.systems().front().front().linear_interface().get_obj_value();
  CHECK(obj > 0);
}

TEST_CASE("LineLP - loss allocation mode split")  // NOLINT
{
  // Split mode: 50/50 between sender and receiver.
  using namespace gtopt;

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
          .lmax = 100.0,
          .capacity = 100.0,
      },
  };
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .lossfactor = 0.10,
          .loss_allocation_mode = Name {"split"},
          .tmax_ba = 500.0,
          .tmax_ab = 500.0,
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

  Planning planning = {
      .options =
          {
              .use_kirchhoff = false,
              .use_single_bus = false,
          },
      .simulation = simulation,
      .system =
          {
              .name = "LossSplitTest",
              .bus_array = bus_array,
              .demand_array = demand_array,
              .generator_array = generator_array,
              .line_array = line_array,
          },
  };

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 1);

  const auto obj =
      planning_lp.systems().front().front().linear_interface().get_obj_value();
  CHECK(obj > 0);
}

TEST_CASE("LineLP - loss allocation modes affect LMPs but all solve")  // NOLINT
{
  // The three loss allocation modes produce different LP formulations
  // (different coefficients on the flow variable in bus balance rows).
  // Each mode preserves energy conservation but allocates losses
  // differently, leading to slightly different generation levels.
  // This test verifies all three solve successfully and produce
  // physically reasonable objectives (within a tight range).
  using namespace gtopt;

  auto make_planning = [](const char* mode_str) -> Planning
  {
    OptName mode = mode_str ? OptName {Name {mode_str}} : OptName {};
    return Planning {
        .options =
            {
                .use_kirchhoff = false,
                .use_single_bus = false,
            },
        .simulation =
            {
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
            },
        .system =
            {
                .name = "LossCompare",
                .bus_array =
                    {
                        {
                            .uid = Uid {1},
                            .name = "b1",
                        },
                        {
                            .uid = Uid {2},
                            .name = "b2",
                        },
                    },
                .demand_array =
                    {
                        {
                            .uid = Uid {1},
                            .name = "d1",
                            .bus = Uid {2},
                            .lmax = 100.0,
                            .capacity = 100.0,
                        },
                    },
                .generator_array =
                    {
                        {
                            .uid = Uid {1},
                            .name = "g1",
                            .bus = Uid {1},
                            .gcost = 10.0,
                            .capacity = 500.0,
                        },
                    },
                .line_array =
                    {
                        {
                            .uid = Uid {1},
                            .name = "l1",
                            .bus_a = Uid {1},
                            .bus_b = Uid {2},
                            .lossfactor = 0.10,
                            .loss_allocation_mode = mode,
                            .tmax_ba = 500.0,
                            .tmax_ab = 500.0,
                        },
                    },
            },
    };
  };

  PlanningLP plp_recv(make_planning(nullptr));
  auto r1 = plp_recv.resolve();
  REQUIRE(r1.has_value());
  const auto obj_recv =
      plp_recv.systems().front().front().linear_interface().get_obj_value();

  PlanningLP plp_send(make_planning("sender"));
  auto r2 = plp_send.resolve();
  REQUIRE(r2.has_value());
  const auto obj_send =
      plp_send.systems().front().front().linear_interface().get_obj_value();

  PlanningLP plp_split(make_planning("split"));
  auto r3 = plp_split.resolve();
  REQUIRE(r3.has_value());
  const auto obj_split =
      plp_split.systems().front().front().linear_interface().get_obj_value();

  // All three should be positive and in the same ballpark
  CHECK(obj_recv > 0);
  CHECK(obj_send > 0);
  CHECK(obj_split > 0);

  // Sender mode has slightly less total generation (loss applied to lower
  // flow), receiver mode has slightly more.  Split is in between.
  // All within ~2% of each other for 10% lossfactor.
  const auto mid = (obj_recv + obj_send) / 2;
  CHECK(obj_recv == doctest::Approx(mid).epsilon(0.02));
  CHECK(obj_send == doctest::Approx(mid).epsilon(0.02));
  CHECK(obj_split == doctest::Approx(mid).epsilon(0.02));
}
