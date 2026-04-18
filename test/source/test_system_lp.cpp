/**
 * @file      test_system_lp.hpp
 * @brief     Unit tests for SystemLP
 * @date      Sat Mar 29 22:09:55 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module contains unit tests for the SystemLP class.
 */

#include <filesystem>
#include <optional>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_system.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("SystemLP 1")
{
  using namespace gtopt;
  using Uid = Uid;
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {.uid = Uid {1},
       .name = "b1",
       .bus = Uid {1},
       .forced = true,
       .capacity = 100.0},
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

  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);

  SystemLP system_lp(system, simulation_lp);

  auto&& lp_interface = system_lp.linear_interface();
  REQUIRE(lp_interface.get_numrows() == 3);

  REQUIRE(lp_interface.get_numrows() == 3);

  const SolverOptions lp_opts {};

  auto result = lp_interface.resolve(lp_opts);
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);  // 0 = optimal

  const auto sol = lp_interface.get_col_sol();
  REQUIRE(sol[0] == doctest::Approx(100));  // demand
  REQUIRE(sol[1] == doctest::Approx(100));  // generation

  const auto dual = lp_interface.get_row_dual();
  // get_row_dual() returns physical duals (already includes scale_objective).
  REQUIRE(dual[0] == doctest::Approx(50));
}

TEST_CASE("SystemLP - Primal Infeasible Case")
{
  using Uid = Uid;
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "b1",
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

  const Simulation simulation = {
      .block_array = {{.uid = Uid {3}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "SEN",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  const PlanningOptionsLP options;
  SimulationLP simulation_lp(simulation, options);

  SystemLP system_lp(system, simulation_lp);

  auto&& lp_interface = system_lp.linear_interface();
  auto result = lp_interface.resolve();

  REQUIRE(!result.has_value());
  CHECK(result.error().code == ErrorCode::SolverError);
  CHECK(result.error().message.find("non-optimal") != std::string::npos);
}

TEST_CASE("SystemLP - Timeout Scenario")
{
  using Uid = Uid;
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "b1",
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
          .capacity = 1000.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {3}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "SEN",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);

  SystemLP system_lp(system, simulation_lp);

  auto&& lp_interface = system_lp.linear_interface();

  const SolverOptions opts;
  lp_interface.set_time_limit(0.001);  // Set very small timeout

  auto result = lp_interface.resolve(opts);

  // May either timeout or solve quickly - both are acceptable outcomes
  if (!result) {
    CHECK(result.error().code == ErrorCode::SolverError);
    CHECK(result.error().message.find("time") != std::string::npos);
  } else {
    CHECK(result.value() == 0);  // 0 = optimal
  }
}

// ──────────────────────────────────────────────────────────────────────
// SystemLP move semantics
//
// SystemLP holds a SystemContext which keeps a back-reference to its
// owning SystemLP (`m_system_`) plus a `m_collection_ptrs_` table of
// interior pointers into the owner's `m_collections_` tuple.  A
// defaulted move would copy these references verbatim, leaving the
// embedded context referring to the moved-from SystemLP and to a stale
// collections tuple.  The custom move-ctor / move-assign re-point both
// via `SystemContext::rebind_system(*this)`.
//
// These tests pin that contract: after a move, the SystemContext owned
// by the destination SystemLP must yield the destination's address from
// `system_context().system()`, and `system().elements<...>().size()`
// (which uses the rebound `m_collection_ptrs_` table) must still match
// the input arrays.  Without rebind, the second check segfaults or
// returns 0.
// ──────────────────────────────────────────────────────────────────────

namespace
{
SystemLP make_minimal_system_lp(SimulationLP& simulation_lp,
                                const System& system)
{
  return SystemLP(system, simulation_lp);
}
}  // namespace

TEST_CASE("SystemLP - move-ctor rebinds embedded SystemContext")
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {10}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {20},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 1000.0,
      },
  };
  const Simulation simulation = {
      .block_array = {{.uid = Uid {3}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  const System system = {
      .name = "SEN",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);

  SUBCASE("direct move construction")
  {
    SystemLP src = make_minimal_system_lp(simulation_lp, system);
    SystemLP dst {std::move(src)};

    // Back-reference must point at the new owner, not the moved-from one.
    CHECK(&dst.system_context().system() == &dst);

    // Collection table must have been rebuilt to point at dst.m_collections_.
    CHECK(dst.elements<BusLP>().size() == bus_array.size());
    CHECK(dst.elements<DemandLP>().size() == demand_array.size());
    CHECK(dst.elements<GeneratorLP>().size() == generator_array.size());
  }

  SUBCASE("move into vector reallocation")
  {
    // Force at least one move when push_back grows the vector.  Each
    // surviving SystemLP must be self-consistent post-move.
    std::vector<SystemLP> sinks;
    sinks.reserve(1);  // first push_back grows on second insertion
    sinks.emplace_back(make_minimal_system_lp(simulation_lp, system));
    sinks.emplace_back(make_minimal_system_lp(simulation_lp, system));

    for (const auto& s : sinks) {
      CHECK(&s.system_context().system() == &s);
      CHECK(s.elements<BusLP>().size() == bus_array.size());
      CHECK(s.elements<DemandLP>().size() == demand_array.size());
      CHECK(s.elements<GeneratorLP>().size() == generator_array.size());
    }
  }

  SUBCASE("move via std::optional<SystemLP> emplace + std::move")
  {
    // Mirrors the parallel-build buffer pattern in
    // PlanningLP::create_systems: build into optional<SystemLP> then
    // move into the final vector slot.
    std::optional<SystemLP> opt;
    opt.emplace(system, simulation_lp);
    REQUIRE(opt.has_value());

    SystemLP final_sys {*std::move(opt)};

    CHECK(&final_sys.system_context().system() == &final_sys);
    CHECK(final_sys.elements<BusLP>().size() == bus_array.size());
    CHECK(final_sys.elements<GeneratorLP>().size() == generator_array.size());
  }
}

TEST_CASE("SystemLP - move-assign rebinds embedded SystemContext")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Demand> demand_array = {
      {.uid = Uid {10}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {20},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 1000.0,
      },
  };
  const Simulation simulation = {
      .block_array = {{.uid = Uid {3}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  const System system = {
      .name = "SEN",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);

  // Two distinct SystemLPs; assign one over the other.
  std::optional<SystemLP> dst;
  dst.emplace(system, simulation_lp);

  SystemLP src(system, simulation_lp);
  *dst = std::move(src);

  CHECK(&dst->system_context().system() == &*dst);
  CHECK(dst->elements<BusLP>().size() == bus_array.size());
  CHECK(dst->elements<DemandLP>().size() == demand_array.size());
  CHECK(dst->elements<GeneratorLP>().size() == generator_array.size());
}

// ── Output invariance guard ─────────────────────────────────────────────────
//
// `SystemLP::m_output_written_` is the keystone for solution invariance
// across `low_memory=off` / `compress`: the SDDP simulation pass calls
// `write_out()` while the backend is still live, then any later call
// from `PlanningLP::write_out` must become a no-op so it cannot
// overwrite the sim-pass output with values read from a rehydrated
// (possibly un-solved) backend.

TEST_CASE(
    "SystemLP - output_written() starts false and write_out is idempotent")
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "b1",
          .bus = Uid {1},
          .forced = true,
          .capacity = 100.0,
      },
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
              {
                  .uid = Uid {3},
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
  const System system = {
      .name = "SEN",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  // Route output into a disposable temp directory so the test doesn't
  // pollute the repo or collide with other parallel tests.
  const auto tmp_dir = std::filesystem::temp_directory_path()
      / std::format("gtopt_write_out_idempotence_{}",
                    static_cast<std::int64_t>(::getpid()));
  std::filesystem::create_directories(tmp_dir);

  PlanningOptions popts;
  popts.demand_fail_cost = 1000.0;
  popts.output_directory = tmp_dir.string();
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  REQUIRE(system_lp.linear_interface().resolve({}).has_value());

  CHECK_FALSE(system_lp.output_written());

  // First write: emits tables and flips the flag.
  system_lp.write_out();
  CHECK(system_lp.output_written());

  // Remove the tmp dir to catch any subsequent stray writes: after the
  // idempotence guard kicks in the directory must NOT be recreated.
  std::error_code ec;
  std::filesystem::remove_all(tmp_dir, ec);
  REQUIRE(!std::filesystem::exists(tmp_dir));

  // Second write: must be a no-op.  The flag stays true and no files
  // are emitted (otherwise the tmp_dir would reappear).
  system_lp.write_out();
  CHECK(system_lp.output_written());
  CHECK_FALSE(std::filesystem::exists(tmp_dir));
}
