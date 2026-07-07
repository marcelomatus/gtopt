// SPDX-License-Identifier: BSD-3-Clause
//
// Unit tests for the `KirchhoffMode` enum + `PlanningOptionsLP::
// kirchhoff_mode()` accessor + `LineLP::add_kirchhoff_rows` mode
// dispatch.  Mirrors the `LineLossesMode` test pattern.
//
// PR 1 of the kirchhoff-strategy refactor only registers the enum and
// extracts the existing B–θ assembly into `kirchhoff::node_angle`.
// The `cycle_basis` strategy is enum-reserved but throws on use; PR 2
// will implement it.  These tests pin both behaviours.

#include <filesystem>
#include <string>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/bus_lp.hpp>
#include <gtopt/json/json_parse_policy.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/line_enums.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>

using namespace gtopt;
// NOLINTBEGIN(bugprone-unchecked-optional-access)

// ── Enum string round-trip ────────────────────────────────────────

TEST_CASE("KirchhoffMode enum parsing")
{
  SUBCASE("round-trip all values")
  {
    for (const auto& entry : kirchhoff_mode_entries) {
      const auto parsed = enum_from_name<KirchhoffMode>(entry.name);
      REQUIRE(parsed.has_value());
      CHECK(*parsed == entry.value);
      CHECK(enum_name(*parsed) == entry.name);
    }
  }

  SUBCASE("invalid name returns nullopt")
  {
    CHECK_FALSE(enum_from_name<KirchhoffMode>("invalid").has_value());
    CHECK_FALSE(enum_from_name<KirchhoffMode>("").has_value());
  }

  SUBCASE("expected entries present")
  {
    CHECK(kirchhoff_mode_entries.size() == 2);
    CHECK(enum_from_name<KirchhoffMode>("node_angle").value()
          == KirchhoffMode::node_angle);
    CHECK(enum_from_name<KirchhoffMode>("cycle_basis").value()
          == KirchhoffMode::cycle_basis);
  }
}

// ── PlanningOptionsLP::kirchhoff_mode() resolution ────────────────

TEST_CASE("PlanningOptionsLP::kirchhoff_mode default + override")
{
  SUBCASE("unset → cycle_basis (post-2026-05-14 default)")
  {
    // Default flipped from `node_angle` to `cycle_basis` so meshed grids
    // get the smaller LP without explicit opt-in (no theta vars / no
    // ref-θ-per-island fixings).  Tests that need the older B–θ form
    // must pin `kirchhoff_mode = node_angle` explicitly.
    PlanningOptionsLP options {};
    CHECK(options.kirchhoff_mode() == KirchhoffMode::cycle_basis);
    CHECK(PlanningOptionsLP::default_kirchhoff_mode
          == KirchhoffMode::cycle_basis);
  }

  SUBCASE("model_options.kirchhoff_mode = 'cycle_basis'")
  {
    PlanningOptions popts;
    popts.model_options.kirchhoff_mode = OptName {"cycle_basis"};
    PlanningOptionsLP options {std::move(popts)};
    CHECK(options.kirchhoff_mode() == KirchhoffMode::cycle_basis);
  }

  SUBCASE("model_options.kirchhoff_mode = 'node_angle' (explicit)")
  {
    PlanningOptions popts;
    popts.model_options.kirchhoff_mode = OptName {"node_angle"};
    PlanningOptionsLP options {std::move(popts)};
    CHECK(options.kirchhoff_mode() == KirchhoffMode::node_angle);
  }

  SUBCASE("invalid string falls back to default")
  {
    PlanningOptions popts;
    popts.model_options.kirchhoff_mode = OptName {"not-a-mode"};
    PlanningOptionsLP options {std::move(popts)};
    CHECK(options.kirchhoff_mode() == KirchhoffMode::cycle_basis);
  }
}

// ── End-to-end equivalence: cycle_basis vs node_angle on a meshed grid ──
//
// Builds a 3-bus triangle (b1—b2—b3—b1) so the network has exactly one
// independent cycle, solves it under both Kirchhoff strategies, and
// confirms that:
//   1. both produce the same physical objective (within solver tol),
//   2. the cycle_basis LP creates **no** theta columns (zero
//      `bus_theta_*` cols), while node_angle does,
//   3. cycle_basis has a smaller column count than node_angle on
//      the same problem (theta vars + reference-θ fixings dropped).
namespace
{

constexpr std::string_view triangle_3bus_json = R"json(
  {
    "options": {
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {
        "use_single_bus": false,
        "use_kirchhoff": true,
        "scale_objective": 1,
        "demand_fail_cost": 1000
      }
    },
    "simulation": {
      "block_array": [
        {
          "uid": 1,
          "duration": 1
        }
      ],
      "stage_array": [
        {
          "uid": 1,
          "first_block": 0,
          "count_block": 1
        }
      ],
      "scenario_array": [
        {
          "uid": 1,
          "probability_factor": 1
        }
      ]
    },
    "system": {
      "name": "tri3",
      "bus_array": [
        {
          "uid": 1,
          "name": "b1"
        },
        {
          "uid": 2,
          "name": "b2"
        },
        {
          "uid": 3,
          "name": "b3"
        }
      ],
      "generator_array": [
        {
          "uid": 1,
          "name": "g1",
          "bus": "b1",
          "pmin": 0,
          "pmax": 300,
          "gcost": 10,
          "capacity": 300
        }
      ],
      "demand_array": [
        {
          "uid": 1,
          "name": "d3",
          "bus": "b3",
          "lmax": [
            [
              150.0
            ]
          ]
        }
      ],
      "line_array": [
        {
          "uid": 1,
          "name": "l12",
          "bus_a": "b1",
          "bus_b": "b2",
          "reactance": 0.05,
          "tmax_ab": 200,
          "tmax_ba": 200
        },
        {
          "uid": 2,
          "name": "l23",
          "bus_a": "b2",
          "bus_b": "b3",
          "reactance": 0.05,
          "tmax_ab": 200,
          "tmax_ba": 200
        },
        {
          "uid": 3,
          "name": "l13",
          "bus_a": "b1",
          "bus_b": "b3",
          "reactance": 0.05,
          "tmax_ab": 200,
          "tmax_ba": 200
        }
      ]
    }
  }
)json";

struct SolveResult
{
  double obj;
  int n_cols;
  int n_theta_cols;  ///< unused for now; col-name introspection is fragile
};

[[nodiscard]] auto solve_with_mode(std::string_view mode) -> SolveResult
{
  auto planning =
      daw::json::from_json<Planning>(triangle_3bus_json, StrictParsePolicy);
  planning.options.model_options.kirchhoff_mode = OptName {std::string {mode}};

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& li = systems.front().front().linear_interface();

  return {li.get_obj_value_raw(), li.get_numcols(), 0};
}

}  // namespace

TEST_CASE(  // NOLINT
    "kirchhoff_mode — cycle_basis and node_angle agree on a meshed grid")
{
  // Demand of 150 MW at b3, single 10$/MWh generator at b1.  Three lines
  // form a triangle with identical reactance, so each Kirchhoff
  // strategy must choose the same dispatch — KVL forces a 2:1 split of
  // the b1→b3 power across the two paths regardless of formulation.
  // Physical cost = 150 MW × 10 $/MWh = 1500 $.
  constexpr double expected = 150.0 * 10.0;

  const auto na = solve_with_mode("node_angle");
  const auto cb = solve_with_mode("cycle_basis");

  SUBCASE("objective matches between formulations")
  {
    CHECK(na.obj == doctest::Approx(expected).epsilon(1e-3));
    CHECK(cb.obj == doctest::Approx(expected).epsilon(1e-3));
    CHECK(cb.obj == doctest::Approx(na.obj).epsilon(1e-6));
  }

  SUBCASE("cycle_basis has strictly fewer columns than node_angle")
  {
    // node_angle adds one θ col per bus + one reference fixing per
    // island; cycle_basis adds neither.  On this 3-bus triangle the
    // expected delta is exactly 3 cols (one θ per bus).
    CHECK(cb.n_cols < na.n_cols);
    CHECK(na.n_cols - cb.n_cols == 3);
  }
}

// ── cycle_basis must NOT add or use theta anywhere ──────────────────────
//
// Companion test for `kirchhoff_mode — cycle_basis and node_angle agree`:
// asserts the *negative* invariants of the loop-flow formulation.
//   - `BusLP::needs_kirchhoff(sc)` is `false` ⇒ `lazy_add_theta` is a
//     no-op ⇒ no `theta_col` ever materialised in the LP.
//   - `SimulationLP::find_ampl_suppression("bus", "theta")` returns the
//     `kirchhoff_mode=cycle_basis` reason ⇒ user constraints touching
//     `bus.theta` are silently dropped (mode-driven suppression),
//     instead of throwing "cannot resolve" or — worse — silently
//     resolving to an absent column.
TEST_CASE(  // NOLINT
    "kirchhoff_mode — cycle_basis suppresses bus.theta in AMPL resolver")
{
  // Default options ⇒ kirchhoff_mode=cycle_basis (post-2026-05-14).
  const auto planning =
      daw::json::from_json<Planning>(triangle_3bus_json, StrictParsePolicy);

  PlanningLP planning_lp(Planning {planning});
  REQUIRE(planning_lp.resolve().has_value());

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& sys_lp = systems.front().front();
  const auto& sim_lp = sys_lp.system_context().simulation();

  SUBCASE("bus.theta is suppressed with reason 'kirchhoff_mode=cycle_basis'")
  {
    const auto reason = sim_lp.find_ampl_suppression(
        Bus::class_name.snake_case(), BusLP::ThetaName);
    REQUIRE(reason.has_value());
    CHECK(reason.value() == "kirchhoff_mode=cycle_basis");
  }

  SUBCASE("no theta col was ever added to the LP")
  {
    // BusLP::theta_cols is mutable + lazy.  If `needs_kirchhoff` ever
    // returned true (it must not under cycle_basis), `lookup_theta_col`
    // would yield a populated col for at least one (scen, stage, block)
    // triple.  Sweep all (scen × stage × block × bus) tuples; every
    // lookup must return nullopt.
    auto&& scenarios = sim_lp.scenarios();
    auto&& stages = sim_lp.stages();
    int populated = 0;
    for (const auto& bus_lp : sys_lp.elements<BusLP>()) {
      for (const auto& scenario : scenarios) {
        for (const auto& stage : stages) {
          for (const auto& block : stage.blocks()) {
            if (bus_lp.lookup_theta_col(scenario, stage, block.uid())
                    .has_value())
            {
              ++populated;
            }
          }
        }
      }
    }
    CHECK(populated == 0);
  }
}

// ── Same invariants under explicit node_angle pin: theta IS materialised ──
TEST_CASE(  // NOLINT
    "kirchhoff_mode — node_angle creates theta and does NOT suppress bus.theta")
{
  auto planning =
      daw::json::from_json<Planning>(triangle_3bus_json, StrictParsePolicy);
  planning.options.model_options.kirchhoff_mode = OptName {"node_angle"};

  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  const auto& sys_lp = systems.front().front();
  const auto& sim_lp = sys_lp.system_context().simulation();

  SUBCASE("bus.theta is NOT suppressed under node_angle")
  {
    const auto reason = sim_lp.find_ampl_suppression(
        Bus::class_name.snake_case(), BusLP::ThetaName);
    CHECK_FALSE(reason.has_value());
  }

  SUBCASE("at least one bus has a theta col populated for some (s, t, b)")
  {
    auto&& scenarios = sim_lp.scenarios();
    auto&& stages = sim_lp.stages();
    bool any_populated = false;
    for (const auto& bus_lp : sys_lp.elements<BusLP>()) {
      for (const auto& scenario : scenarios) {
        for (const auto& stage : stages) {
          for (const auto& block : stage.blocks()) {
            if (bus_lp.lookup_theta_col(scenario, stage, block.uid())
                    .has_value())
            {
              any_populated = true;
            }
          }
        }
      }
    }
    CHECK(any_populated);
  }
}

// ── write_out: cycle_basis emits line flows but NOT theta artefacts ────
//
// Confirms the post-solve write path is mode-aware.  Under cycle_basis:
//   - `LineLP::add_to_output` still writes `flowp` / `flown` (the
//     primary line flow solution) and `capacityp` / `capacityn` duals,
//   - the trailing `add_row_dual(..., ThetaName, ..., theta_rows)` is
//     a no-op (empty holder ⇒ `add_field` early-return),
//   - `BusLP::add_to_output` skips `theta` col_sol/col_cost (empty
//     `theta_cols` ⇒ same early-return),
// so the output directory contains a `Line/flowp/` artefact but no
// `Bus/theta/` or `Line/theta/` artefacts.
TEST_CASE(  // NOLINT
    "kirchhoff_mode — cycle_basis write_out emits flow but not theta")
{
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_cycle_basis_writeout";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  auto planning =
      daw::json::from_json<Planning>(triangle_3bus_json, StrictParsePolicy);
  // Default kirchhoff_mode is cycle_basis; pin it explicitly so the
  // test reads as a contract.
  planning.options.model_options.kirchhoff_mode = OptName {"cycle_basis"};
  planning.options.output_directory = tmpdir.string();

  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
  CHECK_NOTHROW(planning_lp.write_out());

  // Walk the output directory and collect every entry whose path
  // contains "theta" (case-sensitive — the writer uses literal
  // class/field names).  None should be present under cycle_basis.
  // Search for the consolidated signed-flow stream (`Line/flow_sol`);
  // `flowp` was retired when the signed-flow refactor folded both
  // directions into a single sol stream.  The unsigned `flow_sol`
  // file is what the operationally-important line-flow output is now.
  bool flow_present = false;
  std::vector<std::string> theta_artefacts;
  for (auto const& dir_entry :
       std::filesystem::recursive_directory_iterator(tmpdir))
  {
    const auto p = dir_entry.path().string();
    if (p.contains("flow_sol")) {
      flow_present = true;
    }
    if (p.contains("theta")) {
      theta_artefacts.push_back(p);
    }
  }
  // The line flow solution is the operationally important output.
  CHECK(flow_present);
  // No theta col_sol / col_cost / row_dual leaks through.
  CHECK(theta_artefacts.empty());

  std::filesystem::remove_all(tmpdir);
}

// NOLINTEND(bugprone-unchecked-optional-access)