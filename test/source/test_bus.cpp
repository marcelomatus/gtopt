#include <doctest/doctest.h>
#include <gtopt/bus.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/json/json_bus.hpp>
#include <gtopt/line.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>

#include "gtopt/simulation.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Bus")
{
  using namespace gtopt;

  const Bus bus(1, "bus_1");

  CHECK(bus.uid == 1);
  CHECK(bus.name == "bus_1");
}

TEST_CASE("Json Bus 1")
{
  using namespace gtopt;

  std::string_view json_data = R"({
    "uid":5,
    "name":"CRUCERO",
    })";

  const Bus bus = daw::json::from_json<Bus>(json_data);

  CHECK(bus.uid == 5);
  CHECK(bus.name == "CRUCERO");
  CHECK(!bus.voltage);
  CHECK(!bus.reference_theta);
  CHECK(!bus.use_kirchhoff);
}

TEST_CASE("Json Bus 2")
{
  using namespace gtopt;

  std::string_view json_data = R"({
    "uid":5,
    "name":"CRUCERO",
    "voltage":200,
    "reference_theta":1,
    "use_kirchhoff":true,
    })";

  const Bus bus = daw::json::from_json<Bus>(json_data);

  CHECK(bus.uid == 5);
  CHECK(bus.name == "CRUCERO");
  CHECK(bus.voltage.value_or(-1.0) == 200);
  CHECK(bus.reference_theta.value_or(-1.0) == 1);
  CHECK(bus.use_kirchhoff.value_or(-1.0) == true);
}

TEST_CASE("Bus needs_kirchhoff method")
{
  using namespace gtopt;

  SUBCASE("Default values")
  {
    const Bus bus(1, "bus_1");
    CHECK(bus.needs_kirchhoff(0.5)
          == true);  // Default is true with default voltage > threshold
  }

  SUBCASE("With explicit true")
  {
    Bus bus(1, "bus_1");
    bus.use_kirchhoff = true;
    CHECK(bus.needs_kirchhoff(0.5) == true);
  }

  SUBCASE("With explicit false")
  {
    Bus bus(1, "bus_1");
    bus.use_kirchhoff = false;
    CHECK(bus.needs_kirchhoff(0.5) == false);
  }

  SUBCASE("With low voltage")
  {
    Bus bus(1, "bus_1");
    bus.voltage = 0.1;
    CHECK(bus.needs_kirchhoff(0.5) == false);
  }

  SUBCASE("With high voltage")
  {
    Bus bus(1, "bus_1");
    bus.voltage = 1.5;
    CHECK(bus.needs_kirchhoff(0.5) == true);
  }
}

TEST_CASE("Bus serialization")
{
  using namespace gtopt;

  SUBCASE("Minimal bus")
  {
    const Bus bus(1, "bus_1");
    auto json = daw::json::to_json(bus);
    auto roundtrip = daw::json::from_json<Bus>(json);

    CHECK(roundtrip.uid == 1);
    CHECK(roundtrip.name == "bus_1");
    CHECK(!roundtrip.voltage);
    CHECK(!roundtrip.reference_theta);
    CHECK(!roundtrip.use_kirchhoff);
  }

  SUBCASE("Full bus")
  {
    Bus bus(5, "CRUCERO");
    bus.voltage = 200;
    bus.reference_theta = 1;
    bus.use_kirchhoff = true;

    auto json = daw::json::to_json(bus);
    const Bus roundtrip = daw::json::from_json<Bus>(json);

    CHECK(roundtrip.uid == 5);
    CHECK(roundtrip.name == "CRUCERO");
    CHECK(roundtrip.voltage.value_or(-1.0) == 200);
    CHECK(roundtrip.reference_theta.value_or(-1.0) == 1);
    CHECK(roundtrip.use_kirchhoff.value_or(-1.0) == true);
  }
}

TEST_CASE("BusLP construction and basic properties")
{
  using namespace gtopt;
  SUBCASE("Default construction")
  {
    const Bus bus;
    CHECK(bus.uid == unknown_uid);
    CHECK(bus.name.empty());
    CHECK(!bus.voltage.has_value());
    CHECK(!bus.reference_theta.has_value());
    CHECK(!bus.use_kirchhoff.has_value());
  }

  SUBCASE("Parameterized construction")
  {
    const Bus bus(1, "bus_1");
    CHECK(bus.uid == 1);
    CHECK(bus.name == "bus_1");
    CHECK(!bus.use_kirchhoff.has_value());
  }

  SUBCASE("LP wrapper construction")
  {
    const PlanningOptionsLP options({});
    const Simulation simu = {
        .block_array = {{.uid = Uid {1}, .duration = 1}},
        .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
        .scenario_array = {{.uid = Uid {0}}},
    };

    SimulationLP simulation(simu, options);
    SystemLP system({}, simulation);  // NOLINT
    const SystemContext sc(simulation, system);
    const InputContext ic(sc);

    Bus bus(1, "bus_1");
    bus.voltage = 220.0;
    bus.reference_theta = 0.0;
    bus.use_kirchhoff = true;

    const BusLP bus_lp(bus, ic);

    CHECK(bus_lp.uid() == 1);
    CHECK(bus_lp.voltage() == 220.0);
    CHECK(bus_lp.reference_theta().has_value());
    CHECK(bus_lp.reference_theta().value_or(-1.0) == 0.0);
    CHECK(bus_lp.use_kirchhoff() == true);
  }
}

TEST_CASE("BusLP needs_kirchhoff method")
{
  // Create minimal input context

  SUBCASE("Default behavior")
  {
    const PlanningOptions opt;
    const PlanningOptionsLP options(opt);
    const Simulation simu = {
        .block_array = {{.uid = Uid {1}, .duration = 1}},
        .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
        .scenario_array = {{.uid = Uid {0}}},
    };

    SimulationLP simulation(simu, options);
    SystemLP system({}, simulation);  // NOLINT
    const SystemContext sc(simulation, system);
    const InputContext ic(sc);

    // Create a bus with defaults
    const Bus bus(1, "bus_1");
    const BusLP bus_lp(bus, ic);

    // With default options (use_kirchhoff=true, use_single_bus=false)
    // needs_kirchhoff should be true
    CHECK(bus_lp.needs_kirchhoff(sc) == true);
  }

  SUBCASE("When single bus is enabled")
  {
    PlanningOptions opt;

    // Create system with single bus mode
    opt.use_single_bus = true;

    const PlanningOptionsLP options(opt);
    const Simulation simu = {
        .block_array = {{.uid = Uid {1}, .duration = 1}},
        .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
        .scenario_array = {{.uid = Uid {0}}},
    };

    SimulationLP simulation(simu, options);

    SystemLP system({}, simulation);  // NOLINT
    const SystemContext sc(simulation, system);
    const InputContext ic(sc);

    const Bus bus(1, "bus_1");
    const BusLP bus_lp(bus, ic);

    // With single bus mode, needs_kirchhoff should be false
    CHECK(bus_lp.needs_kirchhoff(sc) == false);
  }

  SUBCASE("When kirchhoff is disabled in options")
  {
    PlanningOptions opt;

    // Create system with single bus mode
    opt.use_kirchhoff = false;

    const PlanningOptionsLP options(opt);
    const Simulation simu = {
        .block_array = {{.uid = Uid {1}, .duration = 1}},
        .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
        .scenario_array = {{.uid = Uid {0}}},
    };

    SimulationLP simulation(simu, options);

    SystemLP system({}, simulation);  // NOLINT
    const SystemContext sc(simulation, system);
    const InputContext ic(sc);

    const Bus bus(1, "bus_1");
    const BusLP bus_lp(bus, ic);

    // With kirchhoff disabled in options, needs_kirchhoff should be false
    CHECK(bus_lp.needs_kirchhoff(sc) == false);
  }

  SUBCASE("When kirchhoff threshold is higher than voltage")
  {
    PlanningOptions opt;

    // Create system with single bus mode
    opt.kirchhoff_threshold = 1.5;

    const PlanningOptionsLP options(opt);

    const Simulation simu = {
        .block_array = {{.uid = Uid {1}, .duration = 1}},
        .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
        .scenario_array = {{.uid = Uid {0}}},
    };

    SimulationLP simulation(simu, options);

    SystemLP system({}, simulation);  // NOLINT
    const SystemContext sc(simulation, system);
    const InputContext ic(sc);

    Bus bus(1, "bus_1");
    bus.voltage = 1.0;
    const BusLP bus_lp(bus, ic);

    // With voltage below threshold, needs_kirchhoff should be false
    CHECK(bus_lp.needs_kirchhoff(sc) == false);
  }
}

TEST_CASE("BusLP add_to_lp method")
{
  // Basic setup for add_to_lp test
  const PlanningOptions opt;
  const PlanningOptionsLP options(opt);

  const Simulation simu = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  SimulationLP simulation(simu, options);

  SystemLP system({}, simulation);  // NOLINT
  const SystemContext sc(simulation, system);
  const InputContext ic(sc);

  LinearProblem lp;

  const Bus bus(1, "bus_1");
  BusLP bus_lp(bus, ic);

  // Add to LP should succeed
  const StageLP stage {};
  const ScenarioLP scenario {};
  CHECK(bus_lp.add_to_lp(sc, scenario, stage, lp) == true);
}

TEST_CASE("BusLP - DC OPF solve, theta col_sol")
{
  // Minimal DC OPF: 2 buses, 1 line (reactance = 0.1, V = 1 p.u.),
  // generator on b1 supplies 10 MW demand on b2.
  // Bus 1 is the swing bus (reference_theta = 0).
  //
  // Kirchhoff row (from line_lp.cpp):
  //   -θ_a + θ_b + x_tau · f_p - x_tau · f_n = -φ_rad
  // With V = 1, X = 0.1, τ = 1, φ = 0: x_tau = 0.1.
  // Generator at b1, demand at b2 => forward flow f_p = 10 MW, f_n = 0.
  // θ_a = θ_1 = 0 (pinned) => θ_2 = θ_a - x_tau · f_p = 0 - 0.1 · 10 = -1.0.
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
          .voltage = 1.0,
          .reference_theta = 0.0,
      },
      {
          .uid = Uid {2},
          .name = "b2",
          .voltage = 1.0,
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 100.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {2},
          .capacity = 10.0,
      },
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .voltage = 1.0,
          .reactance = 0.1,
          .tmax_ba = 100.0,
          .tmax_ab = 100.0,
          .capacity = 100.0,
      },
  };

  const Simulation simu = {
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

  PlanningOptions opts;
  opts.use_kirchhoff = true;
  opts.use_single_bus = false;
  opts.model_options.demand_fail_cost = 1000.0;

  const System system = {
      .name = "BusThetaDCOPF",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simu, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);

  const auto& bus_lps = system_lp.elements<BusLP>();
  REQUIRE(bus_lps.size() == 2);

  const auto& scenario_lp = simulation_lp.scenarios().front();
  const auto& stage_lp = simulation_lp.stages().front();
  const auto& block_lp = simulation_lp.blocks().front();

  // Locate the swing bus (uid=1) and the load bus (uid=2).
  const BusLP* swing_bus = nullptr;
  const BusLP* load_bus = nullptr;
  for (const auto& b : bus_lps) {
    if (b.uid() == Uid {1}) {
      swing_bus = &b;
    } else if (b.uid() == Uid {2}) {
      load_bus = &b;
    }
  }
  REQUIRE(swing_bus != nullptr);
  REQUIRE(load_bus != nullptr);

  // Swing bus theta is pinned to 0 (reference_theta = 0.0).
  const auto swing_theta_col =
      swing_bus->lookup_theta_col(scenario_lp, stage_lp, block_lp.uid());
  REQUIRE(swing_theta_col.has_value());
  const auto swing_theta_val = lp.get_col_sol()[*swing_theta_col];
  CHECK(swing_theta_val == doctest::Approx(0.0).epsilon(1e-6));

  // Load bus theta should settle at -1.0 rad per the derivation above.
  const auto load_theta_col =
      load_bus->lookup_theta_col(scenario_lp, stage_lp, block_lp.uid());
  REQUIRE(load_theta_col.has_value());
  const auto load_theta_val = lp.get_col_sol()[*load_theta_col];
  CHECK(load_theta_val == doctest::Approx(-1.0).epsilon(1e-4));
}
