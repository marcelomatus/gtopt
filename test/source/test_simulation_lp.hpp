/**
 * @file      test_simulation_lp.hpp
 * @brief     Tests for the Simulation class
 * @date      Sat Apr 19 12:30:00 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Simulation - Constructor initialization")
{
  using Uid = gtopt::Uid;

  // Create arrays for system components
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };

  // Create minimal system
  const Options options = {};

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system {
      .name = "TestSys",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = {},
  };

  // Test constructor
  const OptionsLP options_lp(options);
  SimulationLP simulation_lp(simulation, options_lp);

  SystemLP system_lp(system, simulation_lp);
  const SystemContext system_context(simulation_lp, system_lp);
  const InputContext ic(system_context);

  CHECK(ic.system_context().system().elements<BusLP>().size()
        == system.bus_array.size());
}

TEST_CASE("Simulation - Basic LP run without solving")
{
  using namespace gtopt;
  using Uid = gtopt::Uid;

  // Create arrays for system components
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };

  const Options options = {};

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system {
      .name = "TestSys",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = {},
  };

  // Test constructor
  const OptionsLP options_lp(options);
  SimulationLP simulation_lp(simulation, options_lp);

  SystemLP system_lp(system, simulation_lp);
  const SystemContext system_context(simulation_lp, system_lp);
  const InputContext ic(system_context);

  // Create minimal system

  // Test just creating the LP without solving
  // simulation_lp.create_lp(system_lp);
}

TEST_CASE("SimulationLP - empty phase_array falls back to default Phase")
{  // NOLINT
  // Regression test: when JSON provides "phase_array": [] the Simulation
  // member is empty, overriding the struct default {Phase{}}.
  // SimulationLP must still produce exactly one active phase.

  const OptionsLP options_lp({});

  const Simulation simulation {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
      .phase_array =
          {},  // explicitly empty – simulates "phase_array": [] in JSON
  };

  const SimulationLP simulation_lp(simulation, options_lp);

  CHECK(simulation_lp.phases().size() == 1);
  CHECK(simulation_lp.scenes().size() == 1);
}

TEST_CASE("SimulationLP - empty scene_array falls back to default Scene")
{  // NOLINT
  // Regression test: when JSON provides "scene_array": [] the Simulation
  // member is empty, overriding the struct default {Scene{}}.
  // SimulationLP must still produce exactly one active scene.

  const OptionsLP options_lp({});

  const Simulation simulation {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
      .scene_array =
          {},  // explicitly empty – simulates "scene_array": [] in JSON
  };

  const SimulationLP simulation_lp(simulation, options_lp);

  CHECK(simulation_lp.scenes().size() == 1);
  CHECK(simulation_lp.phases().size() == 1);
}

TEST_CASE(
    "SimulationLP - both phase_array and scene_array empty fall back to "
    "defaults")
{  // NOLINT
  const OptionsLP options_lp({});

  const Simulation simulation {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
      .phase_array = {},
      .scene_array = {},
  };

  const SimulationLP simulation_lp(simulation, options_lp);

  CHECK(simulation_lp.phases().size() == 1);
  CHECK(simulation_lp.scenes().size() == 1);
}

TEST_CASE(
    "PlanningLP - resolves correctly with empty phase_array and scene_array")
{  // NOLINT
  // Verifies the full solve pipeline works when phase_array/scene_array are
  // empty (as can happen via direct JSON deserialization without merge).

  const Simulation simulation {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
      .phase_array = {},
      .scene_array = {},
  };

  const System system {
      .name = "empty_arrays_test",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .demand_array =
          {{.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 80.0}},
      .generator_array = {{
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      }},
  };

  Planning planning {.simulation = simulation, .system = system};
  PlanningLP planning_lp(planning);

  const auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(*result == 1);  // 1 scene resolved
}

static constexpr std::string_view planning_empty_phase_scene_json = R"({
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
    "scenario_array": [{"uid": 1}],
    "phase_array": [],
    "scene_array": []
  },
  "system": {
    "name": "json_empty_arrays",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": 1, "gcost": 50, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 1, "capacity": 80}
    ]
  }
})";

TEST_CASE(
    "PlanningLP - JSON with empty phase_array and scene_array resolves "
    "correctly")
{  // NOLINT
  // Verifies that a JSON file explicitly containing empty phase_array and
  // scene_array does not break the solver; the fallback defaults are used.
  Planning planning =
      daw::json::from_json<Planning>(planning_empty_phase_scene_json);

  CHECK(planning.simulation.phase_array.empty());
  CHECK(planning.simulation.scene_array.empty());

  PlanningLP planning_lp(planning);
  const auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  CHECK(*result == 1);  // 1 scene resolved
}
