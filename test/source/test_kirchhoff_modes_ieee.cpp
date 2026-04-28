// SPDX-License-Identifier: BSD-3-Clause
//
// Cross-mode equivalence tests for the Kirchhoff Voltage Law strategies
// on the IEEE benchmark cases (4-bus, 9-bus, 14-bus original).  Each
// fixture is solved twice — once with the classical B–θ formulation
// (`kirchhoff_mode = "node_angle"`) and once with the loop-flow
// formulation (`"cycle_basis"`) — and the objectives must agree to
// numerical tolerance.
//
// Why this matters:
//   * Node-angle emits one KVL row per active line per block (`|L|`
//     rows) and pins one θ per island as the gauge reference.
//   * Cycle-basis emits one row per fundamental cycle per block
//     (`|L| − |B| + #islands` rows) and uses NO θ variables — the
//     gauge is fixed implicitly by the spanning tree.
//
// Mathematically the two formulations describe the same flow polytope.
// The objectives are therefore identical at the optimum (any
// θ-feasible solution telescopes to a cycle-feasible one and back).
// The cycle-basis form trades fewer rows for wider rows (each cycle
// row touches every line in the cycle).
//
// The fixtures below are independent copies of the JSON used in
// `test_ieee{4,9,14}b_ori_planning.cpp` so each test file stays
// self-contained — keep them in sync if the upstream cases change.

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ── IEEE fixtures (mirrors of the test_ieee*_ori_planning.cpp blobs) ──

// clang-format off
static constexpr std::string_view ieee4b_ori_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "use_kirchhoff": true
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "ieee_4b_ori",
    "bus_array": [
      {"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"},
      {"uid": 3, "name": "b3"}, {"uid": 4, "name": "b4"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 300, "gcost": 20, "capacity": 300},
      {"uid": 2, "name": "g2", "bus": "b2", "pmin": 0, "pmax": 200, "gcost": 35, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d3", "bus": "b3", "lmax": [[150.0]]},
      {"uid": 2, "name": "d4", "bus": "b4", "lmax": [[100.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l1_2", "bus_a": "b1", "bus_b": "b2", "reactance": 0.02, "tmax_ab": 300, "tmax_ba": 300},
      {"uid": 2, "name": "l1_3", "bus_a": "b1", "bus_b": "b3", "reactance": 0.02, "tmax_ab": 300, "tmax_ba": 300},
      {"uid": 3, "name": "l2_3", "bus_a": "b2", "bus_b": "b3", "reactance": 0.03, "tmax_ab": 200, "tmax_ba": 200},
      {"uid": 4, "name": "l2_4", "bus_a": "b2", "bus_b": "b4", "reactance": 0.02, "tmax_ab": 200, "tmax_ba": 200},
      {"uid": 5, "name": "l3_4", "bus_a": "b3", "bus_b": "b4", "reactance": 0.03, "tmax_ab": 150, "tmax_ba": 150}
    ]
  }
})";

static constexpr std::string_view ieee9b_ori_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "use_kirchhoff": true
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "ieee_9b_ori",
    "bus_array": [
      {"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"}, {"uid": 3, "name": "b3"},
      {"uid": 4, "name": "b4"}, {"uid": 5, "name": "b5"}, {"uid": 6, "name": "b6"},
      {"uid": 7, "name": "b7"}, {"uid": 8, "name": "b8"}, {"uid": 9, "name": "b9"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 10, "pmax": 250, "gcost": 20, "capacity": 250},
      {"uid": 2, "name": "g2", "bus": "b2", "pmin": 10, "pmax": 300, "gcost": 35, "capacity": 300},
      {"uid": 3, "name": "g3", "bus": "b3", "pmin": 0,  "pmax": 270, "gcost": 30, "capacity": 270}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b5", "lmax": [[125.0]]},
      {"uid": 2, "name": "d2", "bus": "b7", "lmax": [[100.0]]},
      {"uid": 3, "name": "d3", "bus": "b9", "lmax":  [[90.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l1_4", "bus_a": "b1", "bus_b": "b4", "reactance": 0.0576,  "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 2, "name": "l2_7", "bus_a": "b2", "bus_b": "b7", "reactance": 0.0625,  "tmax_ab": 300, "tmax_ba": 300},
      {"uid": 3, "name": "l3_9", "bus_a": "b3", "bus_b": "b9", "reactance": 0.0586,  "tmax_ab": 270, "tmax_ba": 270},
      {"uid": 4, "name": "l4_5", "bus_a": "b4", "bus_b": "b5", "reactance": 0.085,   "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 5, "name": "l4_6", "bus_a": "b4", "bus_b": "b6", "reactance": 0.092,   "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 6, "name": "l5_7", "bus_a": "b5", "bus_b": "b7", "reactance": 0.161,   "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 7, "name": "l6_9", "bus_a": "b6", "bus_b": "b9", "reactance": 0.17,    "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 8, "name": "l7_8", "bus_a": "b7", "bus_b": "b8", "reactance": 0.072,   "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 9, "name": "l8_9", "bus_a": "b8", "bus_b": "b9", "reactance": 0.1008,  "tmax_ab": 250, "tmax_ba": 250}
    ]
  }
})";

static constexpr std::string_view ieee14b_ori_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "use_kirchhoff": true
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "ieee_14b_ori",
    "bus_array": [
      {"uid": 1,  "name": "b1"},  {"uid": 2,  "name": "b2"},
      {"uid": 3,  "name": "b3"},  {"uid": 4,  "name": "b4"},
      {"uid": 5,  "name": "b5"},  {"uid": 6,  "name": "b6"},
      {"uid": 7,  "name": "b7"},  {"uid": 8,  "name": "b8"},
      {"uid": 9,  "name": "b9"},  {"uid": 10, "name": "b10"},
      {"uid": 11, "name": "b11"}, {"uid": 12, "name": "b12"},
      {"uid": 13, "name": "b13"}, {"uid": 14, "name": "b14"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 260, "gcost": 20, "capacity": 260},
      {"uid": 2, "name": "g2", "bus": "b2", "pmin": 0, "pmax": 130, "gcost": 35, "capacity": 130},
      {"uid": 3, "name": "g3", "bus": "b3", "pmin": 0, "pmax": 130, "gcost": 30, "capacity": 130},
      {"uid": 4, "name": "g6", "bus": "b6", "pmin": 0, "pmax": 100, "gcost": 40, "capacity": 100},
      {"uid": 5, "name": "g8", "bus": "b8", "pmin": 0, "pmax": 80,  "gcost": 45, "capacity": 80}
    ],
    "demand_array": [
      {"uid": 1,  "name": "d2",  "bus": "b2",  "lmax": [[21.7]]},
      {"uid": 2,  "name": "d3",  "bus": "b3",  "lmax": [[94.2]]},
      {"uid": 3,  "name": "d4",  "bus": "b4",  "lmax": [[47.8]]},
      {"uid": 4,  "name": "d5",  "bus": "b5",  "lmax": [[7.6]]},
      {"uid": 5,  "name": "d6",  "bus": "b6",  "lmax": [[11.2]]},
      {"uid": 6,  "name": "d9",  "bus": "b9",  "lmax": [[29.5]]},
      {"uid": 7,  "name": "d10", "bus": "b10", "lmax": [[9.0]]},
      {"uid": 8,  "name": "d11", "bus": "b11", "lmax": [[3.5]]},
      {"uid": 9,  "name": "d12", "bus": "b12", "lmax": [[6.1]]},
      {"uid": 10, "name": "d13", "bus": "b13", "lmax": [[13.5]]},
      {"uid": 11, "name": "d14", "bus": "b14", "lmax": [[14.9]]}
    ],
    "line_array": [
      {"uid": 1,  "name": "l1_2",   "bus_a": "b1",  "bus_b": "b2",  "reactance": 0.05917, "tmax_ab": 150, "tmax_ba": 150},
      {"uid": 2,  "name": "l1_5",   "bus_a": "b1",  "bus_b": "b5",  "reactance": 0.22304, "tmax_ab": 75,  "tmax_ba": 75},
      {"uid": 3,  "name": "l2_3",   "bus_a": "b2",  "bus_b": "b3",  "reactance": 0.19797, "tmax_ab": 80,  "tmax_ba": 80},
      {"uid": 4,  "name": "l2_4",   "bus_a": "b2",  "bus_b": "b4",  "reactance": 0.17632, "tmax_ab": 80,  "tmax_ba": 80},
      {"uid": 5,  "name": "l2_5",   "bus_a": "b2",  "bus_b": "b5",  "reactance": 0.17388, "tmax_ab": 80,  "tmax_ba": 80},
      {"uid": 6,  "name": "l3_4",   "bus_a": "b3",  "bus_b": "b4",  "reactance": 0.17103, "tmax_ab": 60,  "tmax_ba": 60},
      {"uid": 7,  "name": "l4_5",   "bus_a": "b4",  "bus_b": "b5",  "reactance": 0.04211, "tmax_ab": 80,  "tmax_ba": 80},
      {"uid": 8,  "name": "l4_7",   "bus_a": "b4",  "bus_b": "b7",  "reactance": 0.20912, "tmax_ab": 70,  "tmax_ba": 70},
      {"uid": 9,  "name": "l4_9",   "bus_a": "b4",  "bus_b": "b9",  "reactance": 0.55618, "tmax_ab": 40,  "tmax_ba": 40},
      {"uid": 10, "name": "l5_6",   "bus_a": "b5",  "bus_b": "b6",  "reactance": 0.25202, "tmax_ab": 70,  "tmax_ba": 70},
      {"uid": 11, "name": "l6_11",  "bus_a": "b6",  "bus_b": "b11", "reactance": 0.19890, "tmax_ab": 35,  "tmax_ba": 35},
      {"uid": 12, "name": "l6_12",  "bus_a": "b6",  "bus_b": "b12", "reactance": 0.25581, "tmax_ab": 25,  "tmax_ba": 25},
      {"uid": 13, "name": "l6_13",  "bus_a": "b6",  "bus_b": "b13", "reactance": 0.13027, "tmax_ab": 50,  "tmax_ba": 50},
      {"uid": 14, "name": "l7_8",   "bus_a": "b7",  "bus_b": "b8",  "reactance": 0.17615, "tmax_ab": 50,  "tmax_ba": 50},
      {"uid": 15, "name": "l7_9",   "bus_a": "b7",  "bus_b": "b9",  "reactance": 0.11001, "tmax_ab": 70,  "tmax_ba": 70},
      {"uid": 16, "name": "l9_10",  "bus_a": "b9",  "bus_b": "b10", "reactance": 0.08450, "tmax_ab": 40,  "tmax_ba": 40},
      {"uid": 17, "name": "l9_14",  "bus_a": "b9",  "bus_b": "b14", "reactance": 0.27038, "tmax_ab": 40,  "tmax_ba": 40},
      {"uid": 18, "name": "l10_11", "bus_a": "b10", "bus_b": "b11", "reactance": 0.19207, "tmax_ab": 30,  "tmax_ba": 30},
      {"uid": 19, "name": "l12_13", "bus_a": "b12", "bus_b": "b13", "reactance": 0.19988, "tmax_ab": 20,  "tmax_ba": 20},
      {"uid": 20, "name": "l13_14", "bus_a": "b13", "bus_b": "b14", "reactance": 0.34802, "tmax_ab": 30,  "tmax_ba": 30}
    ]
  }
})";
// clang-format on

namespace
{

/// Solve a benchmark JSON in the requested kirchhoff_mode and return
/// (raw objective, total LP row count) once the LP has been built and
/// resolved.  Row count includes every constraint type — the per-mode
/// row delta isolates the KVL contribution.
struct ModeResult
{
  double objective;
  std::size_t rows;
  std::size_t cols;
};

[[nodiscard]] ModeResult solve_in_mode(std::string_view json,
                                       std::string_view mode_name)
{
  Planning planning = parse_planning_json(json);
  planning.options.model_options.kirchhoff_mode =
      OptName {std::string {mode_name}};

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 1);  // 1 scene processed

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& lp = systems.front().front().linear_interface();
  return ModeResult {
      .objective = lp.get_obj_value_raw(),
      .rows = static_cast<std::size_t>(lp.get_numrows()),
      .cols = static_cast<std::size_t>(lp.get_numcols()),
  };
}

/// Standard cross-mode invariants.  Same fixture, both modes:
///
///   * resolve to optimal status
///   * `cb.obj ≤ na.obj` (polytope-relaxation inequality):  the
///     cycle-basis flow polytope is a *relaxation* of the node-angle
///     polytope.  Every node_angle-feasible flow telescopes to a
///     cycle_basis-feasible one, but the converse fails because
///     `bus_lp.cpp` adds a `±2π` column bound to theta — that bound
///     induces a hidden flow cap `|f_l| ≤ 2π / x_τ_l`.  When the
///     hidden cap binds (small reactance × large flow ⇒ |θ| > 2π)
///     node_angle returns a sub-optimal dispatch with load shedding,
///     while cycle_basis (no theta) returns the *true* DC-OPF
///     optimum.  The IEEE 9b and 14b benchmarks both hit this case;
///     IEEE 4b does not, so equivalence is tight there.
///   * cycle_basis emits ≤ node_angle's row count.
///   * cycle_basis has strictly fewer total cols (no theta cols).
void check_cross_mode_invariants(std::string_view json, std::string_view label)
{
  CAPTURE(label);
  const auto na = solve_in_mode(json, "node_angle");
  const auto cb = solve_in_mode(json, "cycle_basis");

  // Objective equivalence — tight under adaptive `theta_max`.
  CHECK(cb.objective == doctest::Approx(na.objective).epsilon(1e-6));
  CHECK(cb.objective > 0.0);

  // Row-count invariant.
  CHECK(cb.rows <= na.rows);

  // Column-count invariant.
  CHECK(cb.cols < na.cols);
}

}  // namespace

// ── IEEE 4-bus original — 5 lines, 4 buses, 2 fundamental cycles ──

TEST_CASE("Kirchhoff modes — IEEE 4-bus original (objectives match)")
{
  // Flows on this fixture stay below the `2π / x_τ` hidden cap on
  // every line, so the polytope-relaxation inequality is tight here.
  const auto na = solve_in_mode(ieee4b_ori_json, "node_angle");
  const auto cb = solve_in_mode(ieee4b_ori_json, "cycle_basis");
  CHECK(cb.objective == doctest::Approx(na.objective).epsilon(1e-6));
  check_cross_mode_invariants(ieee4b_ori_json, "ieee_4b_ori");
}

// ── IEEE 9-bus original — 9 lines, 9 buses, 1 fundamental cycle ──
//
// Optimal DC-OPF dispatch routes 250 MW from g1 over l1_4 (x=0.0576),
// implying θ_4 = −14.4 rad — outside `bus_lp.cpp`'s ±2π column bound.
// node_angle therefore returns a *sub-optimal* dispatch with load
// shedding; cycle_basis (no theta) returns the true DC-OPF optimum.
// `cb.obj ≤ na.obj` still holds.

TEST_CASE("Kirchhoff modes — IEEE 9-bus original (cycle_basis ≤ node_angle)")
{
  check_cross_mode_invariants(ieee9b_ori_json, "ieee_9b_ori");
  // True DC-OPF optimum (no theta-bound artifact):
  //   g1 = 250 (max, gcost=20),  g3 = 55 (gcost=30),
  //   g2 = 10  (pmin, gcost=35).
  //   obj = (250·20 + 10·35 + 55·30) / 1000 = 7.0.
  const auto cb = solve_in_mode(ieee9b_ori_json, "cycle_basis");
  CHECK(cb.objective == doctest::Approx(7.0).epsilon(1e-6));
}

// ── IEEE 14-bus original — 20 lines, 14 buses, 7 fundamental cycles ──

TEST_CASE("Kirchhoff modes — IEEE 14-bus original (cycle_basis ≤ node_angle)")
{
  check_cross_mode_invariants(ieee14b_ori_json, "ieee_14b_ori");
}

// ── Row-delta accounting ───────────────────────────────────────────
//
// Per the loop-flow theory:
//   ΔKVL_rows = (|L| − |L|_co_tree)  =  |L| − (|L| − |B| + #islands)
//             = |B| − #islands.
//
// For each connected IEEE benchmark (#islands = 1) we therefore expect
// `node_angle` to have exactly `|B| − 1` more rows than `cycle_basis`
// per scenario × stage × block — multiplied by the number of blocks.
// All three IEEE fixtures have a single block, so the expected delta
// is `|B| − 1`.  cycle_basis additionally drops `|B|` theta columns
// (one per bus) per block.

TEST_CASE("Kirchhoff modes — row-count delta matches |B| − 1")
{
  struct Case
  {
    std::string_view label;
    std::string_view json;
    std::size_t num_buses;
  };
  const std::array<Case, 3> cases = {
      Case {.label = "ieee_4b_ori", .json = ieee4b_ori_json, .num_buses = 4},
      Case {.label = "ieee_9b_ori", .json = ieee9b_ori_json, .num_buses = 9},
      Case {.label = "ieee_14b_ori", .json = ieee14b_ori_json, .num_buses = 14},
  };
  for (const auto& c : cases) {
    CAPTURE(c.label);
    const auto na = solve_in_mode(c.json, "node_angle");
    const auto cb = solve_in_mode(c.json, "cycle_basis");
    // Row delta == |B| − 1 (per single-block, single-stage, single-
    // scenario fixture; connected network ⇒ #islands = 1).
    CHECK(na.rows - cb.rows == c.num_buses - 1);
    // Column delta == |B| (theta cols dropped, one per bus per block;
    // single block ⇒ delta = |B|).
    CHECK(na.cols - cb.cols == c.num_buses);
  }
}

// ── Cross-product: line-losses × kirchhoff modes on IEEE 9-bus ────
//
// Mirrors the `IEEE 9-bus losses modes` cases in test_line_losses.cpp
// but covers BOTH `node_angle` and `cycle_basis` formulations for each
// loss model.  Catches subtle interactions between the loss-segment
// columns and the cycle-basis row assembler — in particular the
// `piecewise_direct` mode (where segments stamp directly into both
// the bus-balance and the cycle KVL row instead of being aggregated
// behind a flowp/flown column).

// clang-format off
static constexpr std::string_view ieee9b_losses_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "use_kirchhoff": true,
    "loss_segments": 5
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "ieee_9b_losses",
    "bus_array": [
      {"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"}, {"uid": 3, "name": "b3"},
      {"uid": 4, "name": "b4"}, {"uid": 5, "name": "b5"}, {"uid": 6, "name": "b6"},
      {"uid": 7, "name": "b7"}, {"uid": 8, "name": "b8"}, {"uid": 9, "name": "b9"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 10, "pmax": 250, "gcost": 20, "capacity": 250},
      {"uid": 2, "name": "g2", "bus": "b2", "pmin": 10, "pmax": 300, "gcost": 35, "capacity": 300},
      {"uid": 3, "name": "g3", "bus": "b3", "pmin":  0, "pmax": 270, "gcost": 30, "capacity": 270}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b5", "lmax": [[125.0]]},
      {"uid": 2, "name": "d2", "bus": "b7", "lmax": [[100.0]]},
      {"uid": 3, "name": "d3", "bus": "b9", "lmax":  [[90.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l1_4", "bus_a": "b1", "bus_b": "b4", "reactance": 0.0576, "resistance": 0.006, "voltage": 230, "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 2, "name": "l2_7", "bus_a": "b2", "bus_b": "b7", "reactance": 0.0625, "resistance": 0.006, "voltage": 230, "tmax_ab": 300, "tmax_ba": 300},
      {"uid": 3, "name": "l3_9", "bus_a": "b3", "bus_b": "b9", "reactance": 0.0586, "resistance": 0.006, "voltage": 230, "tmax_ab": 270, "tmax_ba": 270},
      {"uid": 4, "name": "l4_5", "bus_a": "b4", "bus_b": "b5", "reactance": 0.085,  "resistance": 0.009, "voltage": 230, "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 5, "name": "l4_6", "bus_a": "b4", "bus_b": "b6", "reactance": 0.092,  "resistance": 0.009, "voltage": 230, "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 6, "name": "l5_7", "bus_a": "b5", "bus_b": "b7", "reactance": 0.161,  "resistance": 0.016, "voltage": 230, "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 7, "name": "l6_9", "bus_a": "b6", "bus_b": "b9", "reactance": 0.17,   "resistance": 0.017, "voltage": 230, "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 8, "name": "l7_8", "bus_a": "b7", "bus_b": "b8", "reactance": 0.072,  "resistance": 0.007, "voltage": 230, "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 9, "name": "l8_9", "bus_a": "b8", "bus_b": "b9", "reactance": 0.1008, "resistance": 0.010, "voltage": 230, "tmax_ab": 250, "tmax_ba": 250}
    ]
  }
})";
// clang-format on

namespace
{

/// Solve IEEE 9b with both a per-line `line_losses_mode` override and
/// the global `kirchhoff_mode` override.  Used for the cross-product
/// matrix below.
[[nodiscard]] ModeResult solve_losses_x_kirchhoff(std::string_view losses_mode,
                                                  std::string_view kirch_mode)
{
  Planning planning = parse_planning_json(ieee9b_losses_json);
  for (auto& line : planning.system.line_array) {
    line.line_losses_mode = OptName {std::string {losses_mode}};
  }
  planning.options.model_options.kirchhoff_mode =
      OptName {std::string {kirch_mode}};

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 1);

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& lp = systems.front().front().linear_interface();
  return ModeResult {
      .objective = lp.get_obj_value_raw(),
      .rows = static_cast<std::size_t>(lp.get_numrows()),
      .cols = static_cast<std::size_t>(lp.get_numcols()),
  };
}

}  // namespace

TEST_CASE("Kirchhoff × line-losses — every combination solves on IEEE 9-bus")
{
  // Both kirchhoff modes must work with every loss-model variant.
  // `piecewise_direct` is the most demanding combination because the
  // cycle-basis assembler must stamp segment columns directly into the
  // cycle KVL row when the flowp/flown aggregator is absent.
  for (const auto* losses :
       {"none", "linear", "piecewise", "bidirectional", "piecewise_direct"})
  {
    for (const auto* kirch : {"node_angle", "cycle_basis"}) {
      CAPTURE(losses);
      CAPTURE(kirch);
      const auto r = solve_losses_x_kirchhoff(losses, kirch);
      CHECK(r.objective > 0.0);
    }
  }
}

TEST_CASE(
    "Kirchhoff × line-losses — cycle_basis ≤ node_angle (polytope relaxation)")
{
  // The cycle-basis flow polytope is a relaxation of the node-angle
  // polytope (see `check_cross_mode_invariants`'s comment for the
  // ±2π hidden-flow-cap explanation).  The inequality `cb ≤ na` must
  // hold for every loss model.  IEEE 9-bus has flows that hit the
  // hidden cap, so this is a strict inequality (cycle_basis finds
  // the true DC-OPF optimum; node_angle returns a sub-optimal
  // dispatch with load shedding).
  for (const auto* losses :
       {"none", "linear", "piecewise", "bidirectional", "piecewise_direct"})
  {
    CAPTURE(losses);
    const auto na = solve_losses_x_kirchhoff(losses, "node_angle");
    const auto cb = solve_losses_x_kirchhoff(losses, "cycle_basis");
    // doctest::Approx tolerates LP-solver numerical noise on the
    // ieee9b_losses fixture (per-unit reactances ~10⁻⁶ ⇒ row scale
    // ~10⁶ ⇒ relative round-off ~10⁻⁵).  Default Approx epsilon is
    // already loose enough for an LP optimum.
    CHECK(cb.objective <= doctest::Approx(na.objective).epsilon(1e-5));
    CHECK(cb.objective > 0.0);
  }
}

TEST_CASE("Kirchhoff × line-losses — cycle_basis drops |B| theta cols")
{
  // In every loss model, switching node_angle → cycle_basis must:
  //   * remove exactly one theta column per bus per block
  //   * remove (|B| − 1) KVL rows per block (cycle count = |L|−|B|+1
  //     for a connected network, vs |L| line rows in node_angle)
  // Single-block IEEE 9-bus: |B| = 9, so col delta = 9, row delta = 8.
  for (const auto* losses :
       {"none", "linear", "piecewise", "bidirectional", "piecewise_direct"})
  {
    CAPTURE(losses);
    const auto na = solve_losses_x_kirchhoff(losses, "node_angle");
    const auto cb = solve_losses_x_kirchhoff(losses, "cycle_basis");
    CHECK(na.cols - cb.cols == 9);
    CHECK(na.rows - cb.rows == 8);
  }
}

TEST_CASE("Kirchhoff × line-losses — losses ordering preserved in both modes")
{
  // Sanity: under both kirchhoff modes,
  //   * `none` is the lower bound (no losses billed)
  //   * the three PWL modes agree on the integrated loss curve, so
  //     piecewise / bidirectional / piecewise_direct give the same
  //     objective (within a few segment-discretisation epsilons).
  // `linear` is intentionally NOT in the ordering — it linearises at
  // f_max and over-estimates losses for sub-saturating flows, so it
  // can return a *higher* objective than the PWL modes on this case.
  // Catches a regression where the cycle-basis row assembler would
  // silently drop a segment-direction stamp on `piecewise_direct`.
  for (const auto* kirch : {"node_angle", "cycle_basis"}) {
    CAPTURE(kirch);
    const auto none = solve_losses_x_kirchhoff("none", kirch);
    const auto pw = solve_losses_x_kirchhoff("piecewise", kirch);
    const auto bd = solve_losses_x_kirchhoff("bidirectional", kirch);
    const auto pd = solve_losses_x_kirchhoff("piecewise_direct", kirch);
    CHECK(none.objective <= pw.objective + 1e-6);
    CHECK(pw.objective == doctest::Approx(bd.objective).epsilon(0.01));
    CHECK(pw.objective == doctest::Approx(pd.objective).epsilon(0.01));
  }
}
