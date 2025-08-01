#include <doctest/doctest.h>
#include <gtopt/bus_lp.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/options.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>

#include "gtopt/simulation.hpp"

using namespace gtopt;

TEST_CASE("BusLP construction and basic properties")
{
  SUBCASE("Default construction")
  {
    Bus bus;
    CHECK(bus.uid == unknown_uid);
    CHECK(bus.name.empty());
    CHECK(!bus.voltage.has_value());
    CHECK(!bus.reference_theta.has_value());
    CHECK(!bus.use_kirchhoff.has_value());
  }

  SUBCASE("Parameterized construction")
  {
    Bus bus(1, "bus_1");
    CHECK(bus.uid == 1);
    CHECK(bus.name == "bus_1");
    CHECK(!bus.use_kirchhoff.has_value());
  }

  SUBCASE("LP wrapper construction")
  {
    const OptionsLP options({});
    const Simulation simu = {
        .block_array = {{.uid = Uid {1}, .duration = 1}},
        .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
        .scenario_array = {{.uid = Uid {0}}},
    };

    SimulationLP simulation(simu, options);
    SystemLP system({}, simulation);
    const SystemContext sc(simulation, system);
    const InputContext ic(sc);

    Bus bus(1, "bus_1");
    bus.voltage = 220.0;
    bus.reference_theta = 0.0;
    bus.use_kirchhoff = true;

    const BusLP bus_lp(std::move(bus), ic);

    CHECK(bus_lp.uid() == 1);
    CHECK(bus_lp.voltage() == 220.0);
    REQUIRE(bus_lp.reference_theta().has_value());
    CHECK(bus_lp.reference_theta().value() == 0.0);
    REQUIRE(bus_lp.use_kirchhoff().has_value());
    CHECK(bus_lp.use_kirchhoff().value() == true);
  }
}

TEST_CASE("BusLP needs_kirchhoff method")
{
  // Create minimal input context

  SUBCASE("Default behavior")
  {
    const Options opt;
    const OptionsLP options(opt);
    const Simulation simu = {
        .block_array = {{.uid = Uid {1}, .duration = 1}},
        .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
        .scenario_array = {{.uid = Uid {0}}},
    };

    SimulationLP simulation(simu, options);
    SystemLP system({}, simulation);
    const SystemContext sc(simulation, system);
    const InputContext ic(sc);

    // Create a bus with defaults
    Bus bus(1, "bus_1");
    const BusLP bus_lp(std::move(bus), ic);

    // With default options (use_kirchhoff=true, use_single_bus=false)
    // needs_kirchhoff should be true
    CHECK(bus_lp.needs_kirchhoff(sc) == true);
  }

  SUBCASE("When single bus is enabled")
  {
    Options opt;

    // Create system with single bus mode
    opt.use_single_bus = true;

    const OptionsLP options(opt);
    const Simulation simu = {
        .block_array = {{.uid = Uid {1}, .duration = 1}},
        .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
        .scenario_array = {{.uid = Uid {0}}},
    };

    SimulationLP simulation(simu, options);

    SystemLP system({}, simulation);
    const SystemContext sc(simulation, system);
    const InputContext ic(sc);

    Bus bus(1, "bus_1");
    const BusLP bus_lp(std::move(bus), ic);

    // With single bus mode, needs_kirchhoff should be false
    CHECK(bus_lp.needs_kirchhoff(sc) == false);
  }

  SUBCASE("When kirchhoff is disabled in options")
  {
    Options opt;

    // Create system with single bus mode
    opt.use_kirchhoff = false;

    const OptionsLP options(opt);
    const Simulation simu = {
        .block_array = {{.uid = Uid {1}, .duration = 1}},
        .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
        .scenario_array = {{.uid = Uid {0}}},
    };

    SimulationLP simulation(simu, options);

    SystemLP system({}, simulation);
    const SystemContext sc(simulation, system);
    const InputContext ic(sc);

    Bus bus(1, "bus_1");
    const BusLP bus_lp(std::move(bus), ic);

    // With kirchhoff disabled in options, needs_kirchhoff should be false
    CHECK(bus_lp.needs_kirchhoff(sc) == false);
  }

  SUBCASE("When kirchhoff threshold is higher than voltage")
  {
    Options opt;

    // Create system with single bus mode
    opt.kirchhoff_threshold = 1.5;

    const OptionsLP options(opt);

    const Simulation simu = {
        .block_array = {{.uid = Uid {1}, .duration = 1}},
        .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
        .scenario_array = {{.uid = Uid {0}}},
    };

    SimulationLP simulation(simu, options);

    SystemLP system({}, simulation);
    const SystemContext sc(simulation, system);
    const InputContext ic(sc);

    Bus bus(1, "bus_1");
    bus.voltage = 1.0;
    const BusLP bus_lp(std::move(bus), ic);

    // With voltage below threshold, needs_kirchhoff should be false
    CHECK(bus_lp.needs_kirchhoff(sc) == false);
  }
}

TEST_CASE("BusLP add_to_lp method")
{
  // Basic setup for add_to_lp test
  const Options opt;
  const OptionsLP options(opt);

  const Simulation simu = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  SimulationLP simulation(simu, options);

  SystemLP system({}, simulation);
  const SystemContext sc(simulation, system);
  const InputContext ic(sc);

  LinearProblem lp;

  Bus bus(1, "bus_1");
  BusLP bus_lp(std::move(bus), ic);

  // Add to LP should succeed
  const StageLP stage {};
  const ScenarioLP scenario {};
  CHECK(bus_lp.add_to_lp(sc, scenario, stage, lp) == true);
}
