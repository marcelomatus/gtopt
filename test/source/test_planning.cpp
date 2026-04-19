#include <filesystem>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

#include "fixture_helpers.hpp"
#include "log_capture.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)
using gtopt::test_fixtures::make_single_stage_phases;
using gtopt::test_fixtures::make_uniform_blocks;
using gtopt::test_fixtures::make_uniform_stages;

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
  const auto deserialized = parse_planning_json(json_data);

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
      .options = {.demand_fail_cost = 1000.0},
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
      .options = {.demand_fail_cost = 1000.0},
      .simulation = simulation,
      .system = system,
  };

  // Create flat options with all names enabled so row names are tracked.
  LpMatrixOptions flat_options;
  flat_options.row_with_names = true;
  flat_options.row_with_name_map = true;
  flat_options.col_with_names = true;
  flat_options.col_with_name_map = true;

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

TEST_CASE(  // NOLINT
    "PlanningLP - drop_sim_snapshots under compress keeps cache "
    "intact")
{
  // Mirrors the behaviour the SDDP simulation_pass relies on: after
  // Pass 1 each cell's flat-LP snapshot can be dropped, but the
  // Phase-2a primal/dual/cost cache must survive so
  // `PlanningLP::write_out` reads optimal values from the cache.
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
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
  const Planning planning {
      .options = {.demand_fail_cost = 1000.0},
      .simulation = simulation,
      .system = system,
  };

  LpMatrixOptions flat_options;
  flat_options.low_memory_mode = LowMemoryMode::compress;
  flat_options.memory_codec = CompressionCodec::lz4;

  PlanningLP planning_lp(planning, flat_options);
  REQUIRE(!planning_lp.systems().empty());

  // Resolve so each cell solves to optimum and caches values at
  // release_backend time.  The solve call is the only way the Phase-2a
  // cache gets populated in a real run.
  auto res = planning_lp.resolve();
  REQUIRE(res.has_value());

  // Record obj + cache state before dropping snapshots, so we can
  // compare against the post-drop reads.
  double total_obj_before = 0.0;
  std::size_t optimal_cells = 0;
  for (const auto& phase_systems : planning_lp.systems()) {
    for (const auto& sys : phase_systems) {
      const auto& li = sys.linear_interface();
      if (li.is_optimal()) {
        total_obj_before += li.get_obj_value_physical();
        ++optimal_cells;
      }
    }
  }
  REQUIRE(optimal_cells > 0);

  planning_lp.drop_sim_snapshots();

  // Same obj, same cell count — the cache still serves reads.
  double total_obj_after = 0.0;
  std::size_t optimal_after = 0;
  for (const auto& phase_systems : planning_lp.systems()) {
    for (const auto& sys : phase_systems) {
      const auto& li = sys.linear_interface();
      if (li.is_optimal()) {
        total_obj_after += li.get_obj_value_physical();
        ++optimal_after;
      }
    }
  }
  CHECK(optimal_after == optimal_cells);
  CHECK(total_obj_after == doctest::Approx(total_obj_before));

  // write_out should still succeed — it reads from the Phase-2a cache
  // and re-flattens from System element arrays.  No fresh snapshot
  // needed.
  CHECK_NOTHROW(planning_lp.write_out());
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
      .options = {.demand_fail_cost = 1000.0},
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

  // Create planning; name-bearing bool fields are set directly on
  // flat_options below so LP files include row/column names.
  const Planning planning {
      .options = {.demand_fail_cost = 1000.0},
      .simulation = simulation,
      .system = system,
  };

  // Create flat options with all names enabled so row names are tracked.
  LpMatrixOptions flat_options;
  flat_options.row_with_names = true;
  flat_options.row_with_name_map = true;
  flat_options.col_with_names = true;
  flat_options.col_with_name_map = true;

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
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .forced = true,
          .capacity = 200.0,
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
      .options = {.demand_fail_cost = 1000.0},
      .simulation = simulation,
      .system = system,
  };

  // Create planning_lp
  PlanningLP planning_lp(planning);

  // Run the LP
  auto result = planning_lp.resolve();

  REQUIRE(result.has_value());

  auto&& systems = planning_lp.systems();
  CHECK(systems.size() == 1);

  auto&& system_lp = systems.front().front();
  auto&& lp_interface = system_lp.linear_interface();

  const auto sol = lp_interface.get_col_sol();
  REQUIRE(sol[0] == doctest::Approx(100));  // demand load
  REQUIRE(sol[1] == doctest::Approx(0));  // demand fail
  REQUIRE(sol[2] == doctest::Approx(100));  // generation

  const auto dual = lp_interface.get_row_dual();
  // get_row_dual() returns physical duals (already includes scale_objective).
  REQUIRE(dual[0] == doctest::Approx(50));
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
      .options = {.demand_fail_cost = 1000.0},
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
      .options = {.demand_fail_cost = 1000.0, .scale_theta = 42.0},
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
      .options = {.demand_fail_cost = 1000.0, .use_kirchhoff = false},
      .simulation = simulation,
      .system = system,
  };

  PlanningLP planning_lp(planning);

  CHECK(planning_lp.options().scale_theta() == doctest::Approx(1.0));
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
      .options = {.demand_fail_cost = 1000.0, .use_single_bus = true},
      .simulation = simulation,
      .system = system,
  };

  PlanningLP planning_lp(planning);

  CHECK(planning_lp.options().scale_theta() == doctest::Approx(1.0));
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
      .options = {.demand_fail_cost = 1000.0},
      .simulation = simulation,
      .system = system,
  };

  PlanningLP planning_lp(planning);

  CHECK(planning_lp.options().scale_theta() == doctest::Approx(0.07));
}

TEST_CASE("PlanningLP - auto_scale_theta uses median X/V² on mixed voltages")
{
  using namespace gtopt;

  // Three lines with mixed voltage classes:
  //   l1: V=110 kV, X=12.0 Ω → x_tau = 12.0 / 12100    ≈ 9.917e-04
  //   l2: V=220 kV, X=10.0 Ω → x_tau = 10.0 / 48400    ≈ 2.066e-04
  //   l3: V=500 kV, X= 0.1 Ω → x_tau =  0.1 / 250000   ≈ 4.000e-07
  //
  // median(X/V²) = 2.066e-04  (the 220 kV line)
  // Contrast: median(raw X) would be 10.0 — off by ~5 orders of magnitude.
  //
  // This regression prevents the heuristic from silently reverting to the
  // old median(X) choice, which produced a global matrix ratio of 3.02e+07
  // and κ > 1e11 on the CEN IPLP case.
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
       .name = "l1_110kV",
       .bus_a = Uid {1},
       .bus_b = Uid {2},
       .voltage = 110.0,
       .reactance = 12.0,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
      {.uid = Uid {2},
       .name = "l2_220kV",
       .bus_a = Uid {2},
       .bus_b = Uid {3},
       .voltage = 220.0,
       .reactance = 10.0,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
      {.uid = Uid {3},
       .name = "l3_500kV",
       .bus_a = Uid {3},
       .bus_b = Uid {4},
       .voltage = 500.0,
       .reactance = 0.1,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
  };

  const System system {
      .name = "MixedVoltageTest",
      .bus_array = bus_array,
      .demand_array = dem_array,
      .generator_array = gen_array,
      .line_array = line_array,
  };

  Planning planning {
      .options = {.demand_fail_cost = 1000.0},
      .simulation = simulation,
      .system = system,
  };

  PlanningLP planning_lp(planning);

  // median of { 9.917e-4, 2.066e-4, 4.0e-7 } = 2.066e-4
  CHECK(planning_lp.options().scale_theta()
        == doctest::Approx(10.0 / (220.0 * 220.0)).epsilon(1e-9));

  // Sanity: should be many orders of magnitude away from median(raw X)=10.
  CHECK(planning_lp.options().scale_theta() < 1.0);
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
      .options = {.demand_fail_cost = 1000.0},
      .simulation = simulation,
      .system = system,
  };

  // Const Planning → constructor template uses const path, no auto_scale_theta
  const PlanningLP planning_lp(planning);

  CHECK(planning_lp.options().scale_theta() == doctest::Approx(1.0));
}

static constexpr std::string_view planning_json = R"({
  "options": {
    "annual_discount_rate": 0.1,
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
  auto planning = parse_planning_json(planning_json);

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
  "options": {
    "demand_fail_cost": 1000
  },
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
  auto planning = parse_planning_json(hydro_planning_json);

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

// ──────────────────────────────────────────────────────────────────────
// Parallel (scene × phase) build path
//
// PlanningLP::create_systems builds every (scene, phase) cell in
// parallel into a `vector<vector<optional<SystemLP>>>` build buffer
// and then moves the cells into the final `phase_systems_t`.  Each
// move triggers `SystemLP::operator SystemLP(SystemLP&&)`, which
// re-points the embedded SystemContext at the new owner.
//
// This test exercises the path with multiple scenes × multiple phases
// so the build buffer holds more than one cell, and verifies that
// every (scene, phase) cell ends up with:
//   - the correct phase / scene metadata,
//   - a SystemContext whose `system()` back-reference points at
//     itself (proving the rebind ran), and
//   - a non-empty bus collection (proving `m_collection_ptrs_` was
//     rebuilt to point at the moved-in collections tuple).
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("PlanningLP - parallel multi-scene multi-phase build")
{
  // 2 scenes × 3 phases × 1 stage per phase × 2 blocks per stage.
  auto block_array = make_uniform_blocks(6, 1.0);
  auto stage_array = make_uniform_stages(3, 2);
  auto phase_array = make_single_stage_phases(3);

  const Simulation simulation = {
      .block_array = block_array,
      .stage_array = stage_array,
      .scenario_array =
          {
              {.uid = Uid {1}},
              {.uid = Uid {2}},
          },
      .phase_array = phase_array,
      .scene_array =
          {
              {
                  .uid = Uid {1},
                  .name = {},
                  .active = {},
                  .first_scenario = 0,
                  .count_scenario = 1,
              },
              {
                  .uid = Uid {2},
                  .name = {},
                  .active = {},
                  .first_scenario = 1,
                  .count_scenario = 1,
              },
          },
  };

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
          .capacity = 100.0,
      },
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  const System system = {
      .name = "MultiPhaseTestSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  const Planning planning = {
      .options = {.demand_fail_cost = 1000.0},
      .simulation = simulation,
      .system = system,
  };

  PlanningLP planning_lp(planning);

  // Shape: 2 scenes × 3 phases.
  REQUIRE(planning_lp.systems().size() == 2);
  for (const auto& phase_systems : planning_lp.systems()) {
    REQUIRE(phase_systems.size() == 3);
  }

  // Each (scene, phase) cell must satisfy the post-move invariants.
  for (auto&& [scene_index, phase_systems] :
       enumerate<SceneIndex>(planning_lp.systems()))
  {
    for (auto&& [phase_index, sys] : enumerate<PhaseIndex>(phase_systems)) {
      // SystemContext back-ref points at this slot, not at the
      // build-buffer SystemLP it was moved out of.
      CHECK(&sys.system_context().system() == &sys);

      // Collection-ptr table was rebuilt: bus collection has 2 buses.
      CHECK(sys.elements<BusLP>().size() == bus_array.size());
      CHECK(sys.elements<GeneratorLP>().size() == generator_array.size());
      CHECK(sys.elements<DemandLP>().size() == demand_array.size());

      // Phase / scene metadata matches the slot we are looking at.
      CHECK(sys.phase().index() == phase_index);
      CHECK(sys.scene().index() == scene_index);
    }
  }

  // Resolve runs every (scene, phase) cell — proves SystemContext is
  // wired correctly for downstream solver work, not just for direct
  // accessor reads.
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
}

// ──────────────────────────────────────────────────────────────────────
// PlanningLP::create_systems emits instrumentation lines that let
// users tell at a glance whether LP cells ran in parallel or were
// serialized.  We don't assert on actual parallelism (a 1×1 build is
// trivially serial and a multi-cell build on a 1-core CI box may also
// run with peak_parallel=1) — instead we assert the *log contract*:
// the three new INFO lines must appear and must parse, with sane
// metric ranges.  The numeric check is left to the integration tier.
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("PlanningLP - parallelism instrumentation logs are emitted")
{
  // 2 scenes × 3 phases — same topology as the multi-cell build test
  // above, so we exercise the path that spawns the work-pool tasks.
  auto block_array = make_uniform_blocks(6, 1.0);
  auto stage_array = make_uniform_stages(3, 2);
  auto phase_array = make_single_stage_phases(3);

  const Simulation simulation = {
      .block_array = block_array,
      .stage_array = stage_array,
      .scenario_array =
          {
              {.uid = Uid {1}},
              {.uid = Uid {2}},
          },
      .phase_array = phase_array,
      .scene_array =
          {
              {
                  .uid = Uid {1},
                  .name = {},
                  .active = {},
                  .first_scenario = 0,
                  .count_scenario = 1,
              },
              {
                  .uid = Uid {2},
                  .name = {},
                  .active = {},
                  .first_scenario = 1,
                  .count_scenario = 1,
              },
          },
  };

  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
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
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  const System system = {
      .name = "InstrumentationTestSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  // Pin to full_parallel: this test asserts the per-cell submission
  // log ("Submitted 6 LP cells to work pool"), which is only emitted
  // by the full_parallel path.  The default (scene_parallel) would
  // instead log "Submitted 2 scene tasks to work pool" and is covered
  // by a dedicated test below.
  PlanningOptions planning_options {};
  planning_options.demand_fail_cost = 1000.0;
  planning_options.build_mode = BuildMode::full_parallel;

  const Planning planning = {
      .options = planning_options,
      .simulation = simulation,
      .system = system,
  };

  test::LogCapture logs(512);

  PlanningLP planning_lp(planning);

  // Banner: "Building LP: N scene(s) × M phase(s)" — scenes × phases
  // submitted through the pool.
  CHECK(logs.contains("Building LP: 2 scene(s) × 3 phase(s)"));
  CHECK(logs.contains("[mode=full-parallel]"));

  // Submission marker: 2 × 3 = 6 cells.
  CHECK(logs.contains("Submitted 6 LP cells to work pool"));

  // First-cell marker: fires the moment one worker finishes its task,
  // proving tasks actually executed on a worker thread.
  CHECK(logs.contains("First LP cell built on worker tid="));

  // Completion marker with metrics.  All four metric keywords must be
  // present in the "Building LP done" line.
  const auto msgs = logs.messages();
  const auto done_it = std::ranges::find_if(
      msgs,
      [](const std::string& m) { return m.contains("Building LP done in"); });
  REQUIRE(done_it != msgs.end());
  const auto& done_line = *done_it;
  CHECK(done_line.contains("cells=6"));
  CHECK(done_line.contains("peak_parallel="));
  CHECK(done_line.contains("worker_threads="));
  CHECK(done_line.contains("total_cell_cpu="));
  CHECK(done_line.contains("parallelism="));
}

// ──────────────────────────────────────────────────────────────────────
// Single-cell build: a 1-scene × 1-phase planning still has to emit
// the instrumentation block.  Verifies that cells=1, peak_parallel≥1,
// and worker_threads≥1 — i.e. the single task was actually executed
// on the pool and accounted for.
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("PlanningLP - parallelism instrumentation single cell")
{
  auto block_array = make_uniform_blocks(2, 1.0);
  auto stage_array = make_uniform_stages(1, 2);
  auto phase_array = make_single_stage_phases(1);

  const Simulation simulation = {
      .block_array = block_array,
      .stage_array = stage_array,
      .scenario_array = {{.uid = Uid {1}}},
      .phase_array = phase_array,
      .scene_array =
          {
              {
                  .uid = Uid {1},
                  .name = {},
                  .active = {},
                  .first_scenario = 0,
                  .count_scenario = 1,
              },
          },
  };

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
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  const System system = {
      .name = "SingleCellSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  // Pin to full_parallel to assert the per-cell submission log.
  PlanningOptions planning_options {};
  planning_options.demand_fail_cost = 1000.0;
  planning_options.build_mode = BuildMode::full_parallel;

  const Planning planning = {
      .options = planning_options,
      .simulation = simulation,
      .system = system,
  };

  test::LogCapture logs(256);

  PlanningLP planning_lp(planning);

  CHECK(logs.contains("Building LP: 1 scene(s) × 1 phase(s)"));
  CHECK(logs.contains("[mode=full-parallel]"));
  CHECK(logs.contains("Submitted 1 LP cells to work pool"));
  CHECK(logs.contains("First LP cell built on worker tid="));

  const auto msgs = logs.messages();
  const auto done_it = std::ranges::find_if(
      msgs,
      [](const std::string& m) { return m.contains("Building LP done in"); });
  REQUIRE(done_it != msgs.end());
  // Exactly one cell, at least one worker thread executed it, and
  // peak_parallel must be ≥ 1 (a strict equality is not safe because
  // the banner prints before the task finishes, but the final line
  // reads peak_active after the join).
  CHECK(done_it->contains("cells=1"));
  CHECK(done_it->contains("worker_threads=1"));
  CHECK(done_it->contains("peak_parallel=1"));
}

// ──────────────────────────────────────────────────────────────────────
// `--cpu-factor` from the CLI lands in
// `planning.options.sddp_options.pool_cpu_factor` (see
// `include/gtopt/main_options.hpp:533-535`).  Before the fix, that
// field was only read by the SDDP method pool — the LP-build pool in
// `PlanningLP::create_systems` hard-coded its own `cpu_factor = 2.0`.
// `--cpu-factor 0.025` therefore silently had no effect on the LP
// build, so users could not get a genuine serial baseline to compare
// against the parallel wall time.
//
// The fix adds `PlanningOptionsLP::build_pool_cpu_factor()` reading
// the same field (default 2.0) and passes it through to
// `make_solver_work_pool(factor)` at `source/planning_lp.cpp:428`.
// This test pins the regression: setting `pool_cpu_factor = 0.0001`
// must force the build pool down to 1 thread, which the
// instrumentation line then reports as `peak_parallel=1` and
// `worker_threads=1` even on a multi-cell (2×3) build.
// ──────────────────────────────────────────────────────────────────────
TEST_CASE("PlanningLP - --cpu-factor reaches the LP-build pool")
{
  auto block_array = make_uniform_blocks(6, 1.0);
  auto stage_array = make_uniform_stages(3, 2);
  auto phase_array = make_single_stage_phases(3);

  const Simulation simulation = {
      .block_array = block_array,
      .stage_array = stage_array,
      .scenario_array =
          {
              {.uid = Uid {1}},
              {.uid = Uid {2}},
          },
      .phase_array = phase_array,
      .scene_array =
          {
              {
                  .uid = Uid {1},
                  .name = {},
                  .active = {},
                  .first_scenario = 0,
                  .count_scenario = 1,
              },
              {
                  .uid = Uid {2},
                  .name = {},
                  .active = {},
                  .first_scenario = 1,
                  .count_scenario = 1,
              },
          },
  };

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
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  const System system = {
      .name = "CpuFactorTestSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  // Force `build_pool_cpu_factor() → 0.0001`, which after clamping in
  // `make_solver_work_pool` becomes `max_threads = 1` regardless of
  // hardware_concurrency.  The 2×3 = 6 cells must therefore run
  // strictly serialized.
  PlanningOptions planning_options {};
  planning_options.demand_fail_cost = 1000.0;
  planning_options.sddp_options.pool_cpu_factor = 0.0001;

  const Planning planning = {
      .options = planning_options,
      .simulation = simulation,
      .system = system,
  };

  test::LogCapture logs(512);

  PlanningLP planning_lp(planning);

  const auto msgs = logs.messages();
  const auto done_it = std::ranges::find_if(
      msgs,
      [](const std::string& m) { return m.contains("Building LP done in"); });
  REQUIRE(done_it != msgs.end());
  // 6 cells, forced-serial pool: peak_parallel must be exactly 1.
  // Before the fix this was 6 (or up to hardware_concurrency×2).
  CHECK(done_it->contains("cells=6"));
  CHECK(done_it->contains("peak_parallel=1"));
  CHECK(done_it->contains("worker_threads=1"));
}

// Sanity check: the *default* (no `--cpu-factor`) must preserve the
// pre-fix behavior — the build pool spins up `2.0 ×
// hardware_concurrency` threads and a 2×3 build can go parallel (or
// not, on 1-core CI) without being forced to 1.  We only assert the
// contract "peak_parallel >= 1", because a hardware_concurrency of 1
// is a legitimate environment where even the default yields a
// 2-thread pool (`lround(2.0 × 1) = 2`), but the scheduler may still
// observe peak=1 if cells complete faster than they arrive.  The
// point of this test is: the log line *exists* and does not assert
// `peak_parallel=1` as an artifact of forced-serial mode.
TEST_CASE("PlanningLP - default cpu-factor leaves build pool at 2x HC")
{
  auto block_array = make_uniform_blocks(6, 1.0);
  auto stage_array = make_uniform_stages(3, 2);
  auto phase_array = make_single_stage_phases(3);

  const Simulation simulation = {
      .block_array = block_array,
      .stage_array = stage_array,
      .scenario_array =
          {
              {.uid = Uid {1}},
              {.uid = Uid {2}},
          },
      .phase_array = phase_array,
      .scene_array =
          {
              {
                  .uid = Uid {1},
                  .name = {},
                  .active = {},
                  .first_scenario = 0,
                  .count_scenario = 1,
              },
              {
                  .uid = Uid {2},
                  .name = {},
                  .active = {},
                  .first_scenario = 1,
                  .count_scenario = 1,
              },
          },
  };

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
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  const System system = {
      .name = "DefaultCpuFactorTestSystem",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  // No pool_cpu_factor override — `build_pool_cpu_factor()` returns
  // its 2.0 fallback.
  const Planning planning = {
      .options = {.demand_fail_cost = 1000.0},
      .simulation = simulation,
      .system = system,
  };

  test::LogCapture logs(512);

  PlanningLP planning_lp(planning);

  const auto msgs = logs.messages();
  const auto done_it = std::ranges::find_if(
      msgs,
      [](const std::string& m) { return m.contains("Building LP done in"); });
  REQUIRE(done_it != msgs.end());
  CHECK(done_it->contains("cells=6"));
  // peak_parallel could be anywhere in [1, 6] depending on
  // hardware_concurrency and scheduler timing.  The important
  // invariant is that the instrumentation line *exists*, which we
  // already checked by reaching this point.
}

// ──────────────────────────────────────────────────────────────────────
// BuildMode coverage: the three build modes (serial, scene_parallel,
// full_parallel) each take a different code path in
// `PlanningLP::create_systems`.  They all produce the same result
// (same `phase_systems_t`) but emit different intermediate log lines
// that identify which path actually ran.  The following three tests
// pin each path and assert the mode-specific log contract.
//
// Helper: build a 2-scene × 3-phase generator-only topology so every
// build mode sees at least one parallelism opportunity.  Kept as a
// lambda local to each test to avoid leaking a fixture file.
// ──────────────────────────────────────────────────────────────────────

namespace
{
struct BuildModeFixture
{
  Simulation simulation;
  System system;
};

inline auto make_build_mode_fixture() -> BuildModeFixture
{
  auto block_array = make_uniform_blocks(6, 1.0);
  auto stage_array = make_uniform_stages(3, 2);
  auto phase_array = make_single_stage_phases(3);

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {.uid = Uid {1}},
              {.uid = Uid {2}},
          },
      .phase_array = std::move(phase_array),
      .scene_array =
          {
              {
                  .uid = Uid {1},
                  .name = {},
                  .active = {},
                  .first_scenario = 0,
                  .count_scenario = 1,
              },
              {
                  .uid = Uid {2},
                  .name = {},
                  .active = {},
                  .first_scenario = 1,
                  .count_scenario = 1,
              },
          },
  };

  Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };
  Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  System system = {
      .name = "BuildModeTestSystem",
      .bus_array = std::move(bus_array),
      .demand_array = std::move(demand_array),
      .generator_array = std::move(generator_array),
  };

  return {.simulation = std::move(simulation), .system = std::move(system)};
}
}  // namespace

TEST_CASE("PlanningLP - BuildMode::serial runs in calling thread")
{
  auto fx = make_build_mode_fixture();

  // Serial mode: no pool, no work_pool submission, no "Submitted ..."
  // log.  Every cell is built on the calling thread, so peak_parallel
  // and worker_threads must both be exactly 1 regardless of
  // hardware_concurrency or --cpu-factor.
  PlanningOptions planning_options {};
  planning_options.demand_fail_cost = 1000.0;
  planning_options.build_mode = BuildMode::serial;
  // --cpu-factor is ignored under serial; set it to a non-default
  // value to prove the mode overrides pool sizing.
  planning_options.sddp_options.pool_cpu_factor = 4.0;

  const Planning planning = {
      .options = std::move(planning_options),
      .simulation = std::move(fx.simulation),
      .system = std::move(fx.system),
  };

  test::LogCapture logs(512);

  PlanningLP planning_lp(planning);

  CHECK(logs.contains("Building LP: 2 scene(s) × 3 phase(s)"));
  CHECK(logs.contains("[mode=serial]"));
  // Serial path emits neither submission log.
  const auto msgs = logs.messages();
  CHECK(std::ranges::none_of(
      msgs,
      [](const std::string& m)
      { return m.contains("Submitted ") && m.contains(" to work pool"); }));

  const auto done_it = std::ranges::find_if(
      msgs,
      [](const std::string& m) { return m.contains("Building LP done in"); });
  REQUIRE(done_it != msgs.end());
  CHECK(done_it->contains("mode=serial"));
  CHECK(done_it->contains("cells=6"));
  CHECK(done_it->contains("peak_parallel=1"));
  CHECK(done_it->contains("worker_threads=1"));
}

TEST_CASE("PlanningLP - BuildMode::scene_parallel submits one task per scene")
{
  auto fx = make_build_mode_fixture();

  // Scene-parallel mode (the default): one pool task per scene.  The
  // submission log reads "Submitted 2 scene tasks to work pool" — the
  // count is num_scenes (2), not num_scenes × num_phases (6).
  PlanningOptions planning_options {};
  planning_options.demand_fail_cost = 1000.0;
  planning_options.build_mode = BuildMode::scene_parallel;

  const Planning planning = {
      .options = std::move(planning_options),
      .simulation = std::move(fx.simulation),
      .system = std::move(fx.system),
  };

  test::LogCapture logs(512);

  PlanningLP planning_lp(planning);

  CHECK(logs.contains("Building LP: 2 scene(s) × 3 phase(s)"));
  CHECK(logs.contains("[mode=scene-parallel]"));
  CHECK(logs.contains("Submitted 2 scene tasks to work pool"));

  const auto msgs = logs.messages();
  const auto done_it = std::ranges::find_if(
      msgs,
      [](const std::string& m) { return m.contains("Building LP done in"); });
  REQUIRE(done_it != msgs.end());
  CHECK(done_it->contains("mode=scene-parallel"));
  CHECK(done_it->contains("cells=6"));
  // Per-cell instrumentation still fires from inside each scene task,
  // so cells=6 even though only 2 tasks were submitted.
}

TEST_CASE("PlanningLP - BuildMode::scene_parallel is the default")
{
  auto fx = make_build_mode_fixture();

  // Omitting `build_mode` entirely must select scene_parallel.
  const Planning planning = {
      .options = {.demand_fail_cost = 1000.0},
      .simulation = std::move(fx.simulation),
      .system = std::move(fx.system),
  };

  test::LogCapture logs(512);

  PlanningLP planning_lp(planning);

  CHECK(logs.contains("[mode=scene-parallel]"));
  CHECK(logs.contains("Submitted 2 scene tasks to work pool"));

  const auto msgs = logs.messages();
  const auto done_it = std::ranges::find_if(
      msgs,
      [](const std::string& m) { return m.contains("Building LP done in"); });
  REQUIRE(done_it != msgs.end());
  CHECK(done_it->contains("mode=scene-parallel"));
}

TEST_CASE("PlanningLP - BuildMode::full_parallel submits one task per cell")
{
  auto fx = make_build_mode_fixture();

  PlanningOptions planning_options {};
  planning_options.demand_fail_cost = 1000.0;
  planning_options.build_mode = BuildMode::full_parallel;

  const Planning planning = {
      .options = std::move(planning_options),
      .simulation = std::move(fx.simulation),
      .system = std::move(fx.system),
  };

  test::LogCapture logs(512);

  PlanningLP planning_lp(planning);

  CHECK(logs.contains("[mode=full-parallel]"));
  // Full-parallel path submits num_scenes × num_phases = 6 cells.
  CHECK(logs.contains("Submitted 6 LP cells to work pool"));

  const auto msgs = logs.messages();
  const auto done_it = std::ranges::find_if(
      msgs,
      [](const std::string& m) { return m.contains("Building LP done in"); });
  REQUIRE(done_it != msgs.end());
  CHECK(done_it->contains("mode=full-parallel"));
  CHECK(done_it->contains("cells=6"));
}
