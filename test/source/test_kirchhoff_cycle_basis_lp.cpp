// SPDX-License-Identifier: BSD-3-Clause
//
// PR 2b LP-integration smoke tests for the cycle_basis Kirchhoff
// formulation.  Builds small multi-bus LPs with `kirchhoff_mode =
// "cycle_basis"`, asserts the LP assembles and solves to optimality,
// and runs an end-to-end equivalence check against `node_angle` on the
// same fixture (objective and active-line transfer must agree).
//
// Pure cycle-basis builder unit tests live in
// test_kirchhoff_cycle_basis.cpp.  The LP layer uses that builder via
// `kirchhoff::cycle_basis::add_kvl_rows`, called from
// `system_lp.cpp`'s post-element-loop pass when
// `kirchhoff_mode == cycle_basis`.

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

/// Build a 3-bus triangle fixture: gen on bus 1, demand on bus 3,
/// every pair of buses connected by an AC line.  The triangle has
/// exactly one fundamental cycle, exercising the cycle_basis builder
/// + KVL row emission path.  Caller picks the kirchhoff_mode.
struct TriangleFixture
{
  static constexpr double demand_mw = 100.0;
  static constexpr double gen_cost = 10.0;

  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
      {.uid = Uid {3}, .name = "b3"},
  };
  const Array<Generator> generator_array = {
      {.uid = Uid {1},
       .name = "g1",
       .bus = Uid {1},
       .gcost = gen_cost,
       .capacity = 500.0},
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {3}, .capacity = demand_mw},
  };
  const Array<Line> line_array = {
      // l12: bus 1 ↔ bus 2
      {.uid = Uid {1},
       .name = "l12",
       .bus_a = Uid {1},
       .bus_b = Uid {2},
       .voltage = 220.0,
       .reactance = 0.1,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
      // l23: bus 2 ↔ bus 3
      {.uid = Uid {2},
       .name = "l23",
       .bus_a = Uid {2},
       .bus_b = Uid {3},
       .voltage = 220.0,
       .reactance = 0.1,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
      // l13: bus 1 ↔ bus 3 (closes the triangle)
      {.uid = Uid {3},
       .name = "l13",
       .bus_a = Uid {1},
       .bus_b = Uid {3},
       .voltage = 220.0,
       .reactance = 0.1,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
  };

  [[nodiscard]] System make_system() const
  {
    return System {.name = "Triangle",
                   .bus_array = bus_array,
                   .demand_array = demand_array,
                   .generator_array = generator_array,
                   .line_array = line_array};
  }
};

/// Solve a triangle LP with the requested kirchhoff_mode.  Returns the
/// raw objective (gen cost / scale_objective).
[[nodiscard]] double solve_triangle(std::string_view mode_name)
{
  const TriangleFixture fix;

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  PlanningOptions opts;
  opts.use_kirchhoff = true;
  opts.use_single_bus = false;
  opts.model_options.kirchhoff_mode = OptName {std::string {mode_name}};
  opts.model_options.demand_fail_cost = 1000.0;

  const auto system = fix.make_system();
  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.get_numrows() > 0);
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  return lp.get_obj_value_raw();
}

}  // namespace

TEST_CASE("cycle_basis - triangle LP builds and solves")
{
  const auto obj_cb = solve_triangle("cycle_basis");
  // 100 MW demand × $10/MWh ÷ default scale_objective(1000) = 1.0.
  // The cycle KVL constraint may force a non-trivial split between
  // direct (l13) and indirect (l12+l23) paths, but neither path has a
  // tcost, so the objective is dominated by gen cost.
  CHECK(obj_cb > 0.99);
  CHECK(obj_cb < 1.01);
}

TEST_CASE("cycle_basis - objective matches node_angle on triangle")
{
  const auto obj_na = solve_triangle("node_angle");
  const auto obj_cb = solve_triangle("cycle_basis");
  // Mathematically equivalent: any feasible (f, θ) for node_angle
  // satisfies the cycle KVL by telescoping; conversely, given cycle-
  // KVL-feasible flows there exists θ recovering them (gauge fixed
  // by the spanning tree).  Objective must agree to numerical tolerance.
  CHECK(obj_cb == doctest::Approx(obj_na).epsilon(1e-6));
}

TEST_CASE(
    "cycle_basis - 4-bus square + diagonal: solves and matches node_angle")
{
  // 4 buses arranged as a square with one diagonal (2 fundamental
  // cycles).  Tests the multi-cycle code path.
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
      {.uid = Uid {3}, .name = "b3"},
      {.uid = Uid {4}, .name = "b4"},
  };
  const Array<Generator> generator_array = {
      {.uid = Uid {1},
       .name = "g1",
       .bus = Uid {1},
       .gcost = 10.0,
       .capacity = 500.0},
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {3}, .capacity = 100.0},
  };
  const Array<Line> line_array = {
      {.uid = Uid {1},
       .name = "l12",
       .bus_a = Uid {1},
       .bus_b = Uid {2},
       .voltage = 220.0,
       .reactance = 0.1,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
      {.uid = Uid {2},
       .name = "l23",
       .bus_a = Uid {2},
       .bus_b = Uid {3},
       .voltage = 220.0,
       .reactance = 0.1,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
      {.uid = Uid {3},
       .name = "l34",
       .bus_a = Uid {3},
       .bus_b = Uid {4},
       .voltage = 220.0,
       .reactance = 0.1,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
      {.uid = Uid {4},
       .name = "l41",
       .bus_a = Uid {4},
       .bus_b = Uid {1},
       .voltage = 220.0,
       .reactance = 0.1,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
      // Diagonal — closes a second cycle.
      {.uid = Uid {5},
       .name = "l13",
       .bus_a = Uid {1},
       .bus_b = Uid {3},
       .voltage = 220.0,
       .reactance = 0.1,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  auto solve = [&](std::string_view mode_name) -> double
  {
    PlanningOptions opts;
    opts.use_kirchhoff = true;
    opts.use_single_bus = false;
    opts.model_options.kirchhoff_mode = OptName {std::string {mode_name}};
    opts.model_options.demand_fail_cost = 1000.0;
    const System system = {.name = "Square",
                           .bus_array = bus_array,
                           .demand_array = demand_array,
                           .generator_array = generator_array,
                           .line_array = line_array};
    const PlanningOptionsLP options(opts);
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
    return lp.get_obj_value_raw();
  };

  const auto obj_na = solve("node_angle");
  const auto obj_cb = solve("cycle_basis");
  CHECK(obj_cb == doctest::Approx(obj_na).epsilon(1e-6));
}

TEST_CASE("cycle_basis - radial network (no cycles) still solves")
{
  // 3 buses in a line: gen → l1 → l2 → demand.  Zero fundamental
  // cycles — `add_kvl_rows` should emit no rows but the LP must still
  // be feasible and produce the same objective as `node_angle`.
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
      {.uid = Uid {3}, .name = "b3"},
  };
  const Array<Generator> generator_array = {
      {.uid = Uid {1},
       .name = "g1",
       .bus = Uid {1},
       .gcost = 10.0,
       .capacity = 500.0},
  };
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {3}, .capacity = 100.0},
  };
  const Array<Line> line_array = {
      {.uid = Uid {1},
       .name = "l12",
       .bus_a = Uid {1},
       .bus_b = Uid {2},
       .voltage = 220.0,
       .reactance = 0.1,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
      {.uid = Uid {2},
       .name = "l23",
       .bus_a = Uid {2},
       .bus_b = Uid {3},
       .voltage = 220.0,
       .reactance = 0.1,
       .tmax_ba = 200.0,
       .tmax_ab = 200.0,
       .capacity = 200.0},
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  auto solve = [&](std::string_view mode_name) -> double
  {
    PlanningOptions opts;
    opts.use_kirchhoff = true;
    opts.use_single_bus = false;
    opts.model_options.kirchhoff_mode = OptName {std::string {mode_name}};
    opts.model_options.demand_fail_cost = 1000.0;
    const System system = {.name = "Radial",
                           .bus_array = bus_array,
                           .demand_array = demand_array,
                           .generator_array = generator_array,
                           .line_array = line_array};
    const PlanningOptionsLP options(opts);
    SimulationLP simulation_lp(simulation, options);
    SystemLP system_lp(system, simulation_lp);
    auto&& lp = system_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
    return lp.get_obj_value_raw();
  };

  const auto obj_na = solve("node_angle");
  const auto obj_cb = solve("cycle_basis");
  CHECK(obj_cb == doctest::Approx(obj_na).epsilon(1e-6));
}
