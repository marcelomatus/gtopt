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
  opts.model_options.demand_fail_cost = 1000.0;

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

  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
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
  opts.model_options.demand_fail_cost = 1000.0;

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

TEST_CASE("LineLP - DC line (no reactance) skips Kirchhoff")
{
  // A DC/HVDC line has no reactance.  When use_kirchhoff=true the LP
  // should still solve — the DC line carries power without a voltage-angle
  // constraint.  Verify that the number of LP rows is smaller than for an
  // AC line (no Kirchhoff row per block for the DC line).
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

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  PlanningOptions opts;
  opts.use_kirchhoff = true;
  opts.use_single_bus = false;
  opts.model_options.demand_fail_cost = 1000.0;

  SUBCASE("no reactance field — Kirchhoff skipped")
  {
    // DC line: reactance not set (std::nullopt)
    const Array<Line> line_array = {
        {
            .uid = Uid {1},
            .name = "dc1",
            .bus_a = Uid {1},
            .bus_b = Uid {2},
            .tmax_ba = 200.0,
            .tmax_ab = 200.0,
            .capacity = 200.0,
        },
    };

    const System system = {
        .name = "DCLineTest_nullopt",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .line_array = line_array,
    };

    const PlanningOptionsLP options(opts);
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    const auto nrows_dc = lp.get_numrows();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    // Now build an AC line with reactance for comparison
    const Array<Line> ac_line_array = {
        {
            .uid = Uid {1},
            .name = "ac1",
            .bus_a = Uid {1},
            .bus_b = Uid {2},
            .reactance = 0.1,
            .tmax_ba = 200.0,
            .tmax_ab = 200.0,
            .capacity = 200.0,
        },
    };

    const System ac_system = {
        .name = "ACLineTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .line_array = ac_line_array,
    };

    SimulationLP ac_sim_lp(simulation, options);
    SystemLP ac_system_lp(ac_system, ac_sim_lp);
    auto&& ac_lp = ac_system_lp.linear_interface();
    const auto nrows_ac = ac_lp.get_numrows();

    // AC line adds Kirchhoff row(s); DC line does not
    CHECK(nrows_ac > nrows_dc);
  }

  SUBCASE("reactance = 0 — Kirchhoff skipped")
  {
    // DC line: reactance explicitly set to 0
    const Array<Line> line_array = {
        {
            .uid = Uid {1},
            .name = "dc_zero",
            .bus_a = Uid {1},
            .bus_b = Uid {2},
            .reactance = 0.0,
            .tmax_ba = 200.0,
            .tmax_ab = 200.0,
            .capacity = 200.0,
        },
    };

    const System system = {
        .name = "DCLineTest_zero",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .line_array = line_array,
    };

    const PlanningOptionsLP options(opts);
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);

    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
  }
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
  const auto obj = lp.get_obj_value_raw();
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
  opts.model_options.demand_fail_cost = 1000.0;

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
  const auto obj = lp.get_obj_value_raw();
  CHECK(obj == doctest::Approx(1.5));
}
