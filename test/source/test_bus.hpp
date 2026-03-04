#include <doctest/doctest.h>
#include <gtopt/bus.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/json/json_bus.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/options.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>

#include "gtopt/simulation.hpp"

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

using namespace gtopt;

TEST_CASE("BusLP construction and basic properties")
{
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
    const OptionsLP options({});
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
    const Options opt;
    const OptionsLP options(opt);
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
  const Options opt;
  const OptionsLP options(opt);

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
