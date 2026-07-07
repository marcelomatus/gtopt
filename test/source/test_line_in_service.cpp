// SPDX-License-Identifier: BSD-3-Clause
//
// Unit tests for `Line.in_service` — the per-(stage, block) IntBool
// schedule that mirrors PLEXOS `Line.Units` (`Lin_Units.csv`).  A block
// resolving to 0 takes the line OUT for that block: LineLP emits no flow
// column / capacity row / loss segment / balance contribution, and the
// Kirchhoff layer (both `node_angle` and `cycle_basis`) emits no KVL
// coupling — a true open circuit, NOT a `tmax=0` short (which would pin
// θ_a = θ_b).  Distinct from the per-stage `active` element flag, which
// cannot represent an intra-stage maintenance window.

#include <string>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/json/json_parse_policy.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>

using namespace gtopt;
// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace
{

// Solve a Planning JSON under an explicit kirchhoff_mode; return the raw
// objective.  `in_service` patches are baked into the JSON by the caller.
[[nodiscard]] double solve_obj(std::string_view json, std::string_view mode)
{
  auto planning = daw::json::from_json<Planning>(json, StrictParsePolicy);
  planning.options.model_options.kirchhoff_mode = OptName {std::string {mode}};

  PlanningLP planning_lp(std::move(planning));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());

  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  return systems.front().front().linear_interface().get_obj_value_raw();
}

// ── Radial 2-bus: gen@b1 → line → demand@b2, two equal 1 h blocks ──────
// `in_service` patched into l12 by the caller.  demand_fail_cost = 1000,
// gcost = 10, demand = 150 MW.
constexpr std::string_view radial_2bus_tmpl = R"json(
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
    "block_array": [ {"uid": 1, "duration": 1}, {"uid": 2, "duration": 1} ],
    "stage_array": [ {"uid": 1, "first_block": 0, "count_block": 2} ],
    "scenario_array": [ {"uid": 1, "probability_factor": 1} ]
  },
  "system": {
    "name": "radial2",
    "bus_array": [ {"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"} ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1",
       "pmin": 0, "pmax": 300, "gcost": 10, "capacity": 300}
    ],
    "demand_array": [
      {"uid": 1, "name": "d2", "bus": "b2", "lmax": [[150.0, 150.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l12", "bus_a": "b1", "bus_b": "b2",
       "reactance": 0.05, "tmax_ab": 200, "tmax_ba": 200__INSERVICE__}
    ]
  }
}
)json";

// ── Meshed 3-bus triangle: gen@b1, demand@b3, lines l12/l23/l13 ─────────
// l13 (the direct b1→b3 path) is patched with `in_service`.  Reroute via
// l12+l23 must keep b3 served when l13 is open.
constexpr std::string_view triangle_3bus_tmpl = R"json(
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
    "block_array": [ {"uid": 1, "duration": 1}, {"uid": 2, "duration": 1} ],
    "stage_array": [ {"uid": 1, "first_block": 0, "count_block": 2} ],
    "scenario_array": [ {"uid": 1, "probability_factor": 1} ]
  },
  "system": {
    "name": "tri3",
    "bus_array": [
      {"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"}, {"uid": 3, "name": "b3"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1",
       "pmin": 0, "pmax": 300, "gcost": 10, "capacity": 300}
    ],
    "demand_array": [
      {"uid": 1, "name": "d3", "bus": "b3", "lmax": [[150.0, 150.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l12", "bus_a": "b1", "bus_b": "b2",
       "reactance": 0.05, "tmax_ab": 200, "tmax_ba": 200},
      {"uid": 2, "name": "l23", "bus_a": "b2", "bus_b": "b3",
       "reactance": 0.05, "tmax_ab": 200, "tmax_ba": 200},
      {"uid": 3, "name": "l13", "bus_a": "b1", "bus_b": "b3",
       "reactance": 0.05, "tmax_ab": 200, "tmax_ba": 200__INSERVICE__}
    ]
  }
}
)json";

// Splice an `in_service` member onto the marked line (replaces the
// `__INSERVICE__` token).  Empty patch ⇒ token removed (line always in).
[[nodiscard]] std::string patch(std::string_view tmpl, std::string_view ins)
{
  std::string s {tmpl};
  const auto pos = s.find("__INSERVICE__");
  REQUIRE(pos != std::string::npos);
  s.replace(pos, std::string_view {"__INSERVICE__"}.size(), ins);
  return s;
}

constexpr double kCheap = 10.0;  // $/MWh
constexpr double kFail = 1000.0;  // $/MWh
constexpr double kDemand = 150.0;  // MW

}  // namespace

TEST_CASE("Line.in_service — out block carries zero flow (radial)")  // NOLINT
{
  // Baseline: line in service both blocks ⇒ both served at gen cost.
  const auto baseline = patch(radial_2bus_tmpl, R"(, "in_service": [[1, 1]])");
  // Out in block 2 ⇒ b2 islanded ⇒ block-2 demand fails.
  const auto out_b2 = patch(radial_2bus_tmpl, R"(, "in_service": [[1, 0]])");
  // Absent schedule ⇒ default in service everywhere (== baseline).
  const auto absent = patch(radial_2bus_tmpl, "");

  const double served_both = 2.0 * kDemand * kCheap;  // 3000
  const double one_fails = (kDemand * kCheap) + (kDemand * kFail);  // 151500

  for (const auto* const mode : {"node_angle", "cycle_basis"}) {
    CAPTURE(mode);
    CHECK(solve_obj(baseline, mode) == doctest::Approx(served_both));
    CHECK(solve_obj(absent, mode) == doctest::Approx(served_both));
    // The +148500 jump proves the line delivers ZERO in block 2 — an open
    // circuit, not a derated/short line.
    CHECK(solve_obj(out_b2, mode) == doctest::Approx(one_fails));
  }
}

TEST_CASE(
    "Line.in_service — open line reroutes, not shorted (meshed)")  // NOLINT
{
  // Triangle b1-b2-b3-b1.  Open the direct l13 in block 2.  Power to b3
  // must reroute b1→b2→b3 (l12, l23 each carry 150 ≤ 200 cap), so demand
  // stays served at gen cost.  A buggy implementation that kept the KVL
  // coupling on the open line would force θ_b1 = θ_b3 (node_angle) or a
  // spurious f12 = −f23 loop constraint (cycle_basis), blocking the
  // series reroute and forcing demand failure → a far larger objective.
  const auto absent = patch(triangle_3bus_tmpl, "");
  const auto l13_out_b2 =
      patch(triangle_3bus_tmpl, R"(, "in_service": [[1, 0]])");

  const double served_both = 2.0 * kDemand * kCheap;  // 3000

  for (const auto* const mode : {"node_angle", "cycle_basis"}) {
    CAPTURE(mode);
    CHECK(solve_obj(absent, mode) == doctest::Approx(served_both));
    // Correct open-circuit handling ⇒ reroute succeeds ⇒ still 3000.
    // (Short/spurious-KVL bug ⇒ block-2 failure ⇒ ~151500.)
    CHECK(solve_obj(l13_out_b2, mode) == doctest::Approx(served_both));
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)
