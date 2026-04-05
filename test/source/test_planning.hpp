#include <filesystem>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

TEST_CASE("Planning - Default construction")
{
  using namespace gtopt;

  const Planning opt {};

  // Default planning should have empty components
  CHECK(opt.system.name.empty());
}

TEST_CASE("Planning - Construction with properties")
{
  using namespace gtopt;

  // Create basic components
  const PlanningOptions options {};
  const Simulation simulation {};

  // Create minimal system
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const System system {.name = "TestSystem", .bus_array = bus_array};

  // Create planning with components
  Planning opt {.options = options, .simulation = simulation, .system = system};

  // Verify the planning properties
  CHECK(opt.system.name == "TestSystem");
  REQUIRE(opt.system.bus_array.size() == 1);
  CHECK(opt.system.bus_array[0].name == "b1");
}

TEST_CASE("Planning - Merge operation")
{
  using namespace gtopt;

  // Create first planning
  Planning opt1 {
      .system = {.name = "Opt1System"},
  };

  // Create second planning with different components
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  Planning opt2 {
      .system = {.name = "Opt2System", .bus_array = bus_array},
  };

  // Merge second into first
  opt1.merge(std::move(opt2));

  // Verify the merge result (should contain components from both)
  // Exact behavior depends on how merge is implemented in child components
  // Here we're assuming last-wins behavior based on the merge() implementation
  CHECK(opt1.system.name == "Opt2System");
  REQUIRE(opt1.system.bus_array.size() == 1);
  CHECK(opt1.system.bus_array[0].name == "b1");
}

TEST_CASE("Planning - JSON serialization/deserialization")
{
  using namespace gtopt;

  // Create planning with components
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Planning original {
      .system = {.name = "JsonSystem", .bus_array = bus_array},
  };

  // Serialize to JSON
  const auto json_data = daw::json::to_json(original);

  // Should be able to deserialize back to an object
  const auto deserialized = daw::json::from_json<Planning>(json_data);

  // Verify the deserialized object
  CHECK(deserialized.system.name == "JsonSystem");
  REQUIRE(deserialized.system.bus_array.size() == 1);
  CHECK(deserialized.system.bus_array[0].name == "b1");
}

TEST_CASE("PlanningLP - Default construction base")
{
  using namespace gtopt;
  // Create minimal components
  const PlanningOptions options {};
  const Simulation simulation {};
  const Planning planning {};

  // Test constructor
  const PlanningLP planning_lp(planning);

  // Verify construction was successful

  CHECK(planning_lp.planning().system.name == "");
  REQUIRE(planning_lp.planning().system.bus_array.size() == 0);
}

TEST_CASE("PlanningLP - Default construction")
{
  // Create minimal components
  const PlanningOptions options {};
  const Simulation simulation {};

  // Create minimal system with one bus
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const System system {.name = "TestSystem", .bus_array = bus_array};

  // Create planning with components
  const Planning planning {
      .options = options,
      .simulation = simulation,
      .system = system,
  };

  // Convert options to flat options
  const LpMatrixOptions flat_options;

  // Test constructor
  const PlanningLP planning_lp(planning, flat_options);

  // Verify construction was successful

  CHECK(planning_lp.planning().system.name == "TestSystem");
  REQUIRE(planning_lp.planning().system.bus_array.size() == 1);
  CHECK(planning_lp.planning().system.bus_array[0].name == "b1");
}

TEST_CASE("PlanningLP - Create simulations")
{
  // Create a simulation with blocks, stages, and scenarios
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  // Create minimal system with one bus, one generator, and one demand
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  const System system {
      .name = "TestSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  // Create planning with components
  const Planning planning {
      .simulation = simulation,
      .system = system,
  };

  // Create flat options
  const LpMatrixOptions flat_options;

  // Create planning_lp
  const PlanningLP planning_lp(planning, flat_options);

  // Verify systems were created as expected (indirect test)
  // Further tests would depend on PlanningLP internal implementation
}

TEST_CASE("PlanningLP - Write LP file")
{
  // Create a simulation with blocks, stages, and scenarios
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  // Create minimal system with one bus, one generator, and one demand
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  const System system {
      .name = "TestSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  // Create planning with components
  const Planning planning {
      .simulation = simulation,
      .system = system,
  };

  // Create flat options with lp_names_level=1 so row names are tracked.
  LpMatrixOptions flat_options;
  flat_options.row_with_names = true;
  flat_options.row_with_name_map = true;
  flat_options.lp_names_level = LpNamesLevel::only_cols;

  // Create planning_lp
  const PlanningLP planning_lp(planning, flat_options);

  // Test writing LP file
  planning_lp.write_lp("test_planning");

  // Check if the file was created
  const std::string lp_file = "test_planning_scene_0_phase_0.lp";
  const bool file_exists = std::filesystem::exists(lp_file);

  // Clean up the file if it exists
  if (file_exists) {
    std::filesystem::remove(lp_file);
  }

  // Verify the file was created
  CHECK(file_exists);
}

TEST_CASE("PlanningLP - Run LP")
{
  // Create a simulation with blocks, stages, and scenarios
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  // Create minimal system with one bus, one generator, and one demand
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 80.0},
  };

  const System system {
      .name = "TestSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  // Create planning with components
  const Planning planning {
      .simulation = simulation,
      .system = system,
  };

  // Create flat options
  const LpMatrixOptions flat_options;

  // Create planning_lp
  PlanningLP planning_lp(planning, flat_options);

  // Run the LP
  auto result = planning_lp.resolve();

  REQUIRE(result);
}

TEST_CASE("PlanningLP - Run with write_only flag")
{
  // Create a simulation with blocks, stages, and scenarios
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  // Create minimal system with one bus, one generator, and one demand
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 80.0},
  };

  const System system {
      .name = "TestSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  // Create planning with names_level=1 so LP files include row names.
  const Planning planning {
      .simulation = simulation,
      .system = system,
  };

  // Create flat options with lp_names_level=1 so row names are tracked.
  LpMatrixOptions flat_options;
  flat_options.row_with_names = true;
  flat_options.row_with_name_map = true;
  flat_options.lp_names_level = LpNamesLevel::only_cols;

  // Create planning_lp
  PlanningLP planning_lp(planning, flat_options);

  planning_lp.write_lp("test_planning_lp_write_only");
  // Run the LP (should only create LP model, not solve)

  auto result = planning_lp.resolve();

  // Check that we got a successful result
  REQUIRE(result.has_value());

  // Check if the file was created
  const std::string lp_file = "test_planning_lp_write_only_scene_0_phase_0.lp";
  const bool file_exists = std::filesystem::exists(lp_file);

  // Clean up the file if it exists
  if (file_exists) {
    std::filesystem::remove(lp_file);
  }

  // Verify the file was created
  CHECK(file_exists);
}

TEST_CASE("PlanningLP - Error handling")
{
  // Setup test with invalid data that should cause a solver error
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  // Create system with conflicting constraints
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 200.0},
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

  const System system = {
      .name = "TestSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  const Planning planning = {
      .simulation = simulation,
      .system = system,
  };

  PlanningLP planning_lp(planning);

  // Test error handling
  auto result = planning_lp.resolve();
  REQUIRE(!result);
  CHECK(result.error().code == ErrorCode::SolverError);
  CHECK(result.error().message.find("Solver returned non-optimal")
        != std::string::npos);
}

TEST_CASE("PlanningLP - Solver test")
{
  using Uid = Uid;
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "b1", .bus = Uid {1}, .capacity = 100.0},
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 1000.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {.uid = Uid {3}, .duration = 1},
              {.uid = Uid {4}, .duration = 2},
              {.uid = Uid {5}, .duration = 3},
          },
      .stage_array =
          {
              {.uid = Uid {1}, .first_block = 0, .count_block = 1},
              {.uid = Uid {2}, .first_block = 1, .count_block = 2},
          },
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "SEN",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  REQUIRE(simulation.scenario_array.size() == 1);
  REQUIRE(simulation.stage_array.size() == 2);
  REQUIRE(simulation.block_array.size() == 3);
  REQUIRE(system.bus_array.size() == 1);
  REQUIRE(system.bus_array.size() == 1);
  REQUIRE(system.demand_array.size() == 1);
  REQUIRE(system.generator_array.size() == 1);
  REQUIRE(!system.line_array.empty() == false);

  // Create planning with components
  const Planning planning {
      .simulation = simulation,
      .system = system,
  };

  // Create planning_lp
  PlanningLP planning_lp(planning);

  // Run the LP - should result in an error
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());

  auto&& systems = planning_lp.systems();
  CHECK(systems.size() == 1);

  auto&& system_lp = systems.front().front();
  auto&& lp_interface = system_lp.linear_interface();

  const auto sol = lp_interface.get_col_sol();
  REQUIRE(sol[0] == doctest::Approx(100));  // demand
  REQUIRE(sol[1] == doctest::Approx(100));  // generation

  const auto dual = lp_interface.get_row_dual();
  REQUIRE(dual[0] * system_lp.options().scale_objective()
          == doctest::Approx(50));
}

// ── auto_scale_theta tests ────────────────────────────────────────────────

TEST_CASE("PlanningLP - auto_scale_theta computes median reactance")
{
  using namespace gtopt;

  // Three lines with reactances: 0.05, 0.10, 0.20 → median = 0.10
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
      {.uid = Uid {3}, .name = "b3"},
      {.uid = Uid {4}, .name = "b4"},
  };
  const Array<Generator> gen_array = {
      {.uid = Uid {1},
       .name = "g1",
       .bus = Uid {1},
       .gcost = 50.0,
       .capacity = 200.0},
  };
  const Array<Demand> dem_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {4}, .capacity = 80.0},
  };
  const Array<Line> line_array = {
      {.uid = Uid {1},
       .name = "l1",
       .bus_a = Uid {1},
       .bus_b = Uid {2},
       .reactance = 0.05,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
      {.uid = Uid {2},
       .name = "l2",
       .bus_a = Uid {2},
       .bus_b = Uid {3},
       .reactance = 0.10,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
      {.uid = Uid {3},
       .name = "l3",
       .bus_a = Uid {3},
       .bus_b = Uid {4},
       .reactance = 0.20,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
  };

  const System system {
      .name = "AutoScaleTest",
      .bus_array = bus_array,
      .demand_array = dem_array,
      .generator_array = gen_array,
      .line_array = line_array,
  };

  // No explicit scale_theta → auto_scale_theta should set it to median
  Planning planning {
      .simulation = simulation,
      .system = system,
  };

  PlanningLP planning_lp(planning);

  // After construction, scale_theta should be the median reactance (0.10)
  CHECK(planning_lp.options().scale_theta() == doctest::Approx(0.10));
}

TEST_CASE("PlanningLP - auto_scale_theta skips when explicitly set")
{
  using namespace gtopt;

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
  };
  const Array<Generator> gen_array = {
      {.uid = Uid {1},
       .name = "g1",
       .bus = Uid {1},
       .gcost = 50.0,
       .capacity = 200.0},
  };
  const Array<Demand> dem_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {2}, .capacity = 80.0},
  };
  const Array<Line> line_array = {
      {.uid = Uid {1},
       .name = "l1",
       .bus_a = Uid {1},
       .bus_b = Uid {2},
       .reactance = 0.05,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
  };

  const System system {
      .name = "ExplicitScaleTest",
      .bus_array = bus_array,
      .demand_array = dem_array,
      .generator_array = gen_array,
      .line_array = line_array,
  };

  // Explicitly set scale_theta → auto_scale_theta should NOT override
  Planning planning {
      .options = {.scale_theta = 42.0},
      .simulation = simulation,
      .system = system,
  };

  PlanningLP planning_lp(planning);

  CHECK(planning_lp.options().scale_theta() == doctest::Approx(42.0));
}

TEST_CASE("PlanningLP - auto_scale_theta skips when Kirchhoff disabled")
{
  using namespace gtopt;

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
  };
  const Array<Generator> gen_array = {
      {.uid = Uid {1},
       .name = "g1",
       .bus = Uid {1},
       .gcost = 50.0,
       .capacity = 200.0},
  };
  const Array<Demand> dem_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {2}, .capacity = 80.0},
  };
  const Array<Line> line_array = {
      {.uid = Uid {1},
       .name = "l1",
       .bus_a = Uid {1},
       .bus_b = Uid {2},
       .reactance = 0.05,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
  };

  const System system {
      .name = "NoKirchhoffTest",
      .bus_array = bus_array,
      .demand_array = dem_array,
      .generator_array = gen_array,
      .line_array = line_array,
  };

  // Kirchhoff disabled → auto_scale_theta should skip, use compiled default
  Planning planning {
      .options = {.use_kirchhoff = false},
      .simulation = simulation,
      .system = system,
  };

  PlanningLP planning_lp(planning);

  CHECK(planning_lp.options().scale_theta()
        == doctest::Approx(PlanningOptionsLP::default_scale_theta));
}

TEST_CASE("PlanningLP - auto_scale_theta skips when single_bus enabled")
{
  using namespace gtopt;

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> gen_array = {
      {.uid = Uid {1},
       .name = "g1",
       .bus = Uid {1},
       .gcost = 50.0,
       .capacity = 200.0},
  };
  const Array<Demand> dem_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 80.0},
  };
  const Array<Line> line_array = {
      {.uid = Uid {1},
       .name = "l1",
       .bus_a = Uid {1},
       .bus_b = Uid {1},
       .reactance = 0.05,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
  };

  const System system {
      .name = "SingleBusTest",
      .bus_array = bus_array,
      .demand_array = dem_array,
      .generator_array = gen_array,
      .line_array = line_array,
  };

  // Single-bus enabled → auto_scale_theta should skip
  Planning planning {
      .options = {.use_single_bus = true},
      .simulation = simulation,
      .system = system,
  };

  PlanningLP planning_lp(planning);

  CHECK(planning_lp.options().scale_theta()
        == doctest::Approx(PlanningOptionsLP::default_scale_theta));
}

TEST_CASE("PlanningLP - auto_scale_theta with even number of lines")
{
  using namespace gtopt;

  // Four lines: 0.04, 0.06, 0.08, 0.12 → median = (0.06+0.08)/2 = 0.07
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
      {.uid = Uid {3}, .name = "b3"},
      {.uid = Uid {4}, .name = "b4"},
      {.uid = Uid {5}, .name = "b5"},
  };
  const Array<Generator> gen_array = {
      {.uid = Uid {1},
       .name = "g1",
       .bus = Uid {1},
       .gcost = 50.0,
       .capacity = 200.0},
  };
  const Array<Demand> dem_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {5}, .capacity = 80.0},
  };
  const Array<Line> line_array = {
      {.uid = Uid {1},
       .name = "l1",
       .bus_a = Uid {1},
       .bus_b = Uid {2},
       .reactance = 0.04,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
      {.uid = Uid {2},
       .name = "l2",
       .bus_a = Uid {2},
       .bus_b = Uid {3},
       .reactance = 0.06,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
      {.uid = Uid {3},
       .name = "l3",
       .bus_a = Uid {3},
       .bus_b = Uid {4},
       .reactance = 0.08,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
      {.uid = Uid {4},
       .name = "l4",
       .bus_a = Uid {4},
       .bus_b = Uid {5},
       .reactance = 0.12,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
  };

  const System system {
      .name = "EvenLinesTest",
      .bus_array = bus_array,
      .demand_array = dem_array,
      .generator_array = gen_array,
      .line_array = line_array,
  };

  Planning planning {
      .simulation = simulation,
      .system = system,
  };

  PlanningLP planning_lp(planning);

  CHECK(planning_lp.options().scale_theta() == doctest::Approx(0.07));
}

TEST_CASE("PlanningLP - auto_scale_theta with const Planning")
{
  using namespace gtopt;

  // Const planning → auto_scale_theta is skipped, uses default
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> gen_array = {
      {.uid = Uid {1},
       .name = "g1",
       .bus = Uid {1},
       .gcost = 50.0,
       .capacity = 200.0},
  };
  const Array<Demand> dem_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 80.0},
  };

  const System system {
      .name = "ConstTest",
      .bus_array = bus_array,
      .demand_array = dem_array,
      .generator_array = gen_array,
  };

  const Planning planning {
      .simulation = simulation,
      .system = system,
  };

  // Const Planning → constructor template uses const path, no auto_scale_theta
  const PlanningLP planning_lp(planning);

  CHECK(planning_lp.options().scale_theta()
        == doctest::Approx(PlanningOptionsLP::default_scale_theta));
}

static constexpr std::string_view planning_json = R"({
  "options": {
    "annual_discount_rate": 0.1,
    "lp_matrix_options": {"names_level": 1},
    "output_compression": "uncompressed",
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 1},
      {"uid": 2, "duration": 2}
    ],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1},
      {"uid": 2, "first_block": 1, "count_block": 1}
    ],
    "scenario_array": [
      {"uid": 1, "probability_factor": 1}
    ]
  },
  "system": {
    "name": "json_test_system",
    "bus_array": [
      {"uid": 1, "name": "b1"},
      {"uid": 2, "name": "b2"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": 1, "gcost": 50, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 2, "capacity": 80}
    ],
    "line_array": [
      {
        "uid": 1, "name": "l1",
        "bus_a": 1, "bus_b": 2,
        "reactance": 0.1,
        "tmax_ba": 200, "tmax_ab": 200,
        "capacity": 200
      }
    ],
    "battery_array": [
      {
        "uid": 1, "name": "bat1",
        "bus": 1,
        "input_efficiency": 0.9,
        "output_efficiency": 0.9,
        "emin": 0, "emax": 50,
        "pmax_charge": 100,
        "pmax_discharge": 100,
        "gcost": 0,
        "capacity": 50
      }
    ]
  }
})";

TEST_CASE("Planning JSON parse and solve")
{
  auto planning = daw::json::from_json<Planning>(planning_json);

  CHECK(planning.system.name == "json_test_system");
  CHECK(planning.system.bus_array.size() == 2);
  CHECK(planning.system.generator_array.size() == 1);
  CHECK(planning.system.demand_array.size() == 1);
  CHECK(planning.system.line_array.size() == 1);
  CHECK(planning.system.battery_array.size() == 1);
  CHECK(planning.system.converter_array.empty());

  PlanningLP planning_lp(planning);
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
}

static constexpr std::string_view hydro_planning_json = R"({
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 1},
      {"uid": 2, "duration": 2}
    ],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 2}
    ],
    "scenario_array": [
      {"uid": 1}
    ]
  },
  "system": {
    "name": "hydro_json_test",
    "bus_array": [
      {"uid": 1, "name": "b1"}
    ],
    "generator_array": [
      {"uid": 1, "name": "hydro_gen", "bus": 1, "gcost": 5, "capacity": 200},
      {"uid": 2, "name": "thermal_gen", "bus": 1, "gcost": 100, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 1, "capacity": 50}
    ],
    "junction_array": [
      {"uid": 1, "name": "j_up"},
      {"uid": 2, "name": "j_down", "drain": true}
    ],
    "waterway_array": [
      {
        "uid": 1, "name": "ww1",
        "junction_a": 1, "junction_b": 2,
        "fmin": 0, "fmax": 500
      }
    ],
    "flow_array": [
      {"uid": 1, "name": "inflow", "direction": 1, "junction": 1, "discharge": 20}
    ],
    "reservoir_array": [
      {
        "uid": 1, "name": "rsv1",
        "junction": 1,
        "capacity": 1000,
        "emin": 0, "emax": 1000,
        "eini": 500
      }
    ],
    "turbine_array": [
      {
        "uid": 1, "name": "tur1",
        "waterway": 1, "generator": 1,
        "production_factor": 1.0
      }
    ]
  }
})";

TEST_CASE("Planning JSON parse and solve - hydro system")
{
  auto planning = daw::json::from_json<Planning>(hydro_planning_json);

  CHECK(planning.system.name == "hydro_json_test");
  CHECK(planning.system.junction_array.size() == 2);
  CHECK(planning.system.waterway_array.size() == 1);
  CHECK(planning.system.flow_array.size() == 1);
  CHECK(planning.system.reservoir_array.size() == 1);
  CHECK(planning.system.turbine_array.size() == 1);

  PlanningLP planning_lp(planning);
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
}
