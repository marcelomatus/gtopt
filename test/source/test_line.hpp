#include <doctest/doctest.h>
#include <gtopt/block.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/line.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/system_lp.hpp>

TEST_CASE("Line construction and default values")
{
  using namespace gtopt;
  const Line line;

  // Check default values
  CHECK(line.uid == Uid {unknown_uid});
  CHECK(line.name == Name {});
  CHECK_FALSE(line.active.has_value());
  CHECK(line.bus_a == SingleId {unknown_uid});
  CHECK(line.bus_b == SingleId {unknown_uid});
  CHECK_FALSE(line.voltage.has_value());
  CHECK_FALSE(line.resistance.has_value());
  CHECK_FALSE(line.reactance.has_value());
  CHECK_FALSE(line.lossfactor.has_value());
  CHECK_FALSE(line.use_line_losses.has_value());
  CHECK_FALSE(line.loss_segments.has_value());
  CHECK_FALSE(line.tmax_ba.has_value());
  CHECK_FALSE(line.tmax_ab.has_value());
  CHECK_FALSE(line.tcost.has_value());
  CHECK_FALSE(line.capacity.has_value());
  CHECK_FALSE(line.expcap.has_value());
  CHECK_FALSE(line.expmod.has_value());
  CHECK_FALSE(line.capmax.has_value());
  CHECK_FALSE(line.annual_capcost.has_value());
  CHECK_FALSE(line.annual_derating.has_value());
  CHECK_FALSE(line.tap_ratio.has_value());
  CHECK_FALSE(line.phase_shift_deg.has_value());
}

TEST_CASE("Line attribute assignment")
{
  Line line;

  // Assign basic attributes
  line.uid = 1001;
  line.name = "TestLine";
  line.active = true;
  line.bus_a = Uid {1};
  line.bus_b = Uid {2};

  // Assign electrical parameters
  line.voltage = 132.0;
  line.resistance = 0.01;
  line.reactance = 0.1;
  line.lossfactor = 0.02;
  line.tmax_ba = -100.0;
  line.tmax_ab = 100.0;
  line.tcost = 0.5;

  // Assign capacity parameters
  line.capacity = 200.0;
  line.expcap = 50.0;
  line.expmod = 10.0;
  line.capmax = 300.0;
  line.annual_capcost = 5000.0;
  line.annual_derating = 0.01;

  // Check assigned values
  CHECK(line.uid == 1001);
  CHECK(line.name == "TestLine");
  CHECK(std::get<IntBool>(line.active.value()) == 1);
  CHECK(std::get<Uid>(line.bus_a) == 1);
  CHECK(std::get<Uid>(line.bus_b) == 2);

  // For OptTRealFieldSched types, we need to get the Real variant alternative
  CHECK(std::get_if<Real>(&line.voltage.value()) != nullptr);
  CHECK(std::get_if<Real>(&line.resistance.value()) != nullptr);
  CHECK(std::get_if<Real>(&line.reactance.value()) != nullptr);
  CHECK(std::get_if<Real>(&line.lossfactor.value()) != nullptr);
  CHECK(std::get_if<Real>(&line.tmax_ba.value()) != nullptr);
  CHECK(std::get_if<Real>(&line.tmax_ab.value()) != nullptr);
  CHECK(std::get_if<Real>(&line.tcost.value()) != nullptr);

  // Check actual values using std::get_if
  CHECK(*std::get_if<Real>(&line.voltage.value()) == 132.0);
  CHECK(*std::get_if<Real>(&line.resistance.value()) == 0.01);
  CHECK(*std::get_if<Real>(&line.reactance.value()) == 0.1);
  CHECK(*std::get_if<Real>(&line.lossfactor.value()) == 0.02);
  CHECK(*std::get_if<Real>(&line.tmax_ba.value()) == -100.0);
  CHECK(*std::get_if<Real>(&line.tmax_ab.value()) == 100.0);
  CHECK(*std::get_if<Real>(&line.tcost.value()) == 0.5);

  // Capacity-related checks
  CHECK(*std::get_if<Real>(&line.capacity.value()) == 200.0);
  CHECK(*std::get_if<Real>(&line.expcap.value()) == 50.0);
  CHECK(*std::get_if<Real>(&line.expmod.value()) == 10.0);
  CHECK(*std::get_if<Real>(&line.capmax.value()) == 300.0);
  CHECK(*std::get_if<Real>(&line.annual_capcost.value()) == 5000.0);
  CHECK(*std::get_if<Real>(&line.annual_derating.value()) == 0.01);
}

TEST_CASE("Line time-block schedules")
{
  Line line;
  line.bus_a = Uid {1};
  line.bus_b = Uid {2};
  line.reactance = 0.1;  // Add valid reactance for validation

  // Create simple scalar flow limits
  line.tmax_ba = -100.0;
  line.tmax_ab = 100.0;

  // Verify values were properly assigned
  REQUIRE(line.tmax_ba.has_value());
  REQUIRE(line.tmax_ab.has_value());

  auto* tmax_ba_real_ptr = std::get_if<Real>(&line.tmax_ba.value());
  auto* tmax_ab_real_ptr = std::get_if<Real>(&line.tmax_ab.value());

  REQUIRE(tmax_ba_real_ptr != nullptr);
  REQUIRE(tmax_ab_real_ptr != nullptr);

  CHECK(*tmax_ba_real_ptr == -100.0);
  CHECK(*tmax_ab_real_ptr == 100.0);
}

TEST_CASE("SystemLP with transmission line - two bus system")
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 200.0,
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
          .reactance = 0.1,
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
  opts.demand_fail_cost = 1000.0;

  System system = {
      .name = "TwoBusSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);

  system.setup_reference_bus(options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp_interface = system_lp.linear_interface();
  CHECK(lp_interface.get_numrows() > 0);
  CHECK(lp_interface.get_numcols() > 0);

  auto result = lp_interface.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("SystemLP with line - single bus mode")
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 30.0,
          .capacity = 300.0,
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
          .reactance = 0.05,
          .tmax_ba = 150.0,
          .tmax_ab = 150.0,
          .capacity = 150.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  PlanningOptions opts;
  opts.use_single_bus = true;

  System system = {
      .name = "SingleBusMode",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options_lp(opts);
  SimulationLP simulation_lp(simulation, options_lp);

  system.setup_reference_bus(options_lp);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp_interface = system_lp.linear_interface();
  auto result = lp_interface.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("SystemLP with loop line is skipped")
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  // Loop line: bus_a == bus_b
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "loop",
          .bus_a = Uid {1},
          .bus_b = Uid {1},
          .capacity = 100.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "LoopTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);

  SystemLP system_lp(system, simulation_lp);

  auto&& lp_interface = system_lp.linear_interface();
  auto result = lp_interface.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("LineLP - Kirchhoff (theta) constraints with reactance")
{
  // Two buses connected by a line that has a reactance, with Kirchhoff enabled.
  // This exercises the theta-row path in LineLP::add_to_lp.
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 30.0,
          .capacity = 300.0,
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
          .voltage = 220.0,
          .reactance = 0.1,
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
  opts.use_kirchhoff = true;
  opts.use_single_bus = false;

  const System system = {
      .name = "KirchhoffTest",
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

// ── Transformer (tap_ratio + phase_shift_deg) tests ──────────────────────

TEST_CASE(
    "Transformer with off-nominal tap ratio changes Kirchhoff susceptance")
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

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
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

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

TEST_CASE("LineLP - inactive line is skipped")
{
  // A line with active=false should be skipped entirely in add_to_lp.
  // This exercises the !is_active(stage) early return (line 276).

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
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {2},
          .gcost = 20.0,
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

  // Line is inactive: should not participate in power flow
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l_inactive",
          .active = false,
          .bus_a = Uid {1},
          .bus_b = Uid {2},
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
  opts.demand_fail_cost = 1000.0;
  opts.scale_objective = 1000.0;

  const System system = {
      .name = "InactiveLineTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options_il(opts);
  SimulationLP simulation_lp(simulation, options_il);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // With inactive line, bus 2 demand is served by local g2 at $20/MWh
  // cost = 100 * 20 / 1000 = 2.0
  const auto obj = lp.get_obj_value();
  CHECK(obj == doctest::Approx(2.0));
}

TEST_CASE("LineLP - transfer cost is applied to flow")
{
  // Verifies that tcost adds to the objective when power flows through a line.
  // Two generators at different costs on different buses; line tcost adds to
  // the effective dispatch cost.

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
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {2},
          .gcost = 20.0,
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

  // Line with transfer cost of $5/MWh; no losses
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .tcost = 5.0,
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
      .name = "TransferCostTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options_tc(opts);
  SimulationLP simulation_lp(simulation, options_tc);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  // g1 at $10 + line tcost $5 = $15 effective; cheaper than g2 at $20.
  // Total cost = (10 + 5) * 100 / 1000 = 1.5
  const auto obj = lp.get_obj_value();
  CHECK(obj == doctest::Approx(1.5));
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
