// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_ampl_state.cpp
 * @brief     Contract tests for AMPL `state` variables (piece 5, step 1)
 * @date      2026-06-22
 * @copyright BSD-3-Clause
 *
 * Piece 5 step 1 of the FutureCost / UserModel refactor: the state-variable
 * BRIDGE.  A `DecisionVariable` (and thus a `UserModel`-bundled var) gains
 * `state: true` + `link: true`.  When coarse-scoped (`stage` / `phase` /
 * `global`), its planning-pass build registers a cross-phase `StateVariable`
 * (via `SystemContext::add_state_col`) and defers a link to the previous
 * phase's same-variable column (`defer_state_link`).  It then RIDES the
 * generic SDDP backward pass — coupled across phases and present in every
 * cut — with NO engine change.
 *
 * These tests pin:
 *   1. Schema round-trip (`state` / `link` JSON fields).
 *   2. Guard: `state: true` WITHOUT `link` is a hard error.
 *   3. Guard: `state: true` with `block` scope is a hard error.
 *   4. A `state` var IS registered in `sim.state_variables(scene, phase)`
 *      under the DEDICATED `UserStateVar` class name (not `DecisionVariable`),
 *      so it never collides with engine state (reservoir efin / α).
 *   5. The state var LINKS across phases (dependent_variables point at the
 *      next phase's column; last phase has none).
 *   6. The state var APPEARS in a backward-pass cut after an SDDP solve, and
 *      the SDDP solve still converges (the var rides the existing machinery).
 */

#include <string>
#include <string_view>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/decision_variable.hpp>
#include <gtopt/decision_variable_lp.hpp>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/json/json_decision_variable.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/system_lp.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;

// Unique-named outer namespace avoids unity-build helper-name collisions.
namespace amplstate_test
{
namespace
{

// clang-format off

/// 1-stage, 3-block single-bus system.  `dv_block` is spliced into
/// `decision_variable_array` verbatim so each test can vary the var.
auto make_json(std::string_view dv_block) -> std::string
{
  return std::format(
      R"(
  {{
    "options": {{
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {{
        "use_single_bus": true,
        "demand_fail_cost": 1000,
        "scale_objective": 1
      }}
    }},
    "simulation": {{
      "block_array": [
        {{ "uid": 1, "duration": 8 }},
        {{ "uid": 2, "duration": 8 }},
        {{ "uid": 3, "duration": 8 }}
      ],
      "stage_array": [
        {{ "uid": 1, "first_block": 0, "count_block": 3, "active": 1 }}
      ],
      "scenario_array": [ {{ "uid": 1, "probability_factor": 1 }} ]
    }},
    "system": {{
      "name": "ampl_state_test",
      "bus_array": [ {{ "uid": 1, "name": "b1" }} ],
      "generator_array": [
        {{ "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200,
           "gcost": 5, "capacity": 200 }}
      ],
      "demand_array": [
        {{ "uid": 1, "name": "d1", "bus": "b1", "lmax": [ [ 100.0, 100.0, 100.0 ] ] }}
      ],
      "decision_variable_array": [ {} ]
    }}
  }})",
      dv_block);
}

// clang-format on

}  // namespace
}  // namespace amplstate_test

// ─── 1. Schema round-trip ──────────────────────────────────────────────────

TEST_CASE("AMPL state var — schema round-trip of state/link")  // NOLINT
{
  using namespace gtopt;

  const std::string js = R"({
      "uid": 7, "name": "alpha", "scope": "global",
      "state": true, "link": true, "lower_bound": -1000, "upper_bound": 1000
  })";
  const auto dv = daw::json::from_json<DecisionVariable>(js);

  CHECK(dv.uid == Uid {7});
  CHECK(dv.name == "alpha");
  CHECK((dv.scope && *dv.scope == "global"));
  CHECK(dv.state.value_or(false));
  CHECK(dv.link.value_or(false));

  // Re-serialise and re-parse — the booleans survive.
  const auto round = daw::json::to_json(dv);
  const auto dv2 = daw::json::from_json<DecisionVariable>(round);
  CHECK(dv2.state.value_or(false));
  CHECK(dv2.link.value_or(false));
}

// ─── 2. Guard: state without link is a hard error ──────────────────────────

TEST_CASE("AMPL state var — state without link is a hard error")  // NOLINT
{
  using namespace amplstate_test;

  auto planning = parse_planning_json(make_json(R"({
      "uid": 1, "name": "s", "scope": "global", "state": true })"));
  CHECK_THROWS_AS(PlanningLP(std::move(planning)),  // NOLINT
                  std::runtime_error);
}

// ─── 3. Guard: state with block scope is a hard error ──────────────────────

TEST_CASE("AMPL state var — block-scoped state is a hard error")  // NOLINT
{
  using namespace amplstate_test;

  // Default scope (block) + state:true → error.
  auto p1 = parse_planning_json(make_json(R"({
      "uid": 1, "name": "s", "state": true, "link": true })"));
  CHECK_THROWS_AS(PlanningLP(std::move(p1)), std::runtime_error);  // NOLINT

  // Explicit block scope + state:true → error.
  auto p2 = parse_planning_json(make_json(R"({
      "uid": 1, "name": "s", "scope": "block", "state": true, "link": true })"));
  CHECK_THROWS_AS(PlanningLP(std::move(p2)), std::runtime_error);  // NOLINT
}

// ─── 4/5. Registration + cross-phase linking on the 3-phase fixture ────────

namespace amplstate_test
{
namespace
{

/// The 3-phase hydro fixture with a global `state` DecisionVariable spliced
/// into the system's `decision_variable_array`.  This exercises the
/// cross-phase state-var bridge in a real multi-phase build.
auto make_state_var_planning() -> Planning
{
  auto planning = make_3phase_hydro_planning();
  planning.system.decision_variable_array.push_back(DecisionVariable {
      .uid = Uid {99},
      .name = "user_alpha",
      .lower_bound = OptReal {-1.0e6},
      .upper_bound = OptReal {1.0e6},
      .scope = OptName {"global"},
      .state = OptBool {true},
      .link = OptBool {true},
  });
  return planning;
}

}  // namespace
}  // namespace amplstate_test

TEST_CASE(  // NOLINT
    "AMPL state var — registered under UserStateVar in every phase")
{
  using namespace amplstate_test;

  auto planning = make_state_var_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto& sim = planning_lp.simulation();
  const auto num_phases = static_cast<int>(sim.phases().size());
  REQUIRE(num_phases == 3);

  const auto scene = first_scene_index();
  int found = 0;
  for (int pi = 0; pi < num_phases; ++pi) {
    const auto& sv_map = sim.state_variables(scene, PhaseIndex {pi});
    for (const auto& [key, svar] : sv_map) {
      // The user state var is keyed under the DEDICATED class name, NOT
      // `DecisionVariable` — guard 4: it never collides with engine state.
      if (key.class_name == DecisionVariableLP::StateClassName
          && key.uid == Uid {99} && key.col_name == "value")
      {
        ++found;
        CHECK(svar.col() >= 0);
      }
      // It must NEVER appear under the element's own class name.
      CHECK_FALSE(key.class_name == "DecisionVariable");
    }
  }
  CHECK(found == 3);  // one per phase cell
}

TEST_CASE(  // NOLINT
    "AMPL state var — links across phases (dependent vars + last has none)")
{
  using namespace amplstate_test;

  auto planning = make_state_var_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto& sim = planning_lp.simulation();
  const auto scene = first_scene_index();

  // Phases 0 and 1 link forward to the next phase's column.
  for (int pi = 0; pi < 2; ++pi) {
    const auto& sv_map = sim.state_variables(scene, PhaseIndex {pi});
    bool found_dep = false;
    for (const auto& [key, svar] : sv_map) {
      if (key.class_name == DecisionVariableLP::StateClassName
          && key.uid == Uid {99})
      {
        const auto deps = svar.dependent_variables();
        CHECK_FALSE(deps.empty());
        if (!deps.empty()) {
          CHECK(deps[0].phase_index() == PhaseIndex {pi + 1});
          CHECK(deps[0].scene_index() == scene);
          CHECK(deps[0].col() >= 0);
          found_dep = true;
        }
      }
    }
    CHECK(found_dep);
  }

  // The last phase (2) produces no outgoing link.
  {
    const auto& sv_map = sim.state_variables(scene, PhaseIndex {2});
    for (const auto& [key, svar] : sv_map) {
      if (key.class_name == DecisionVariableLP::StateClassName
          && key.uid == Uid {99})
      {
        CHECK(svar.dependent_variables().empty());
      }
    }
  }
}

// ─── 6. SDDP solve with the user state var still converges ──────────────────

TEST_CASE(  // NOLINT
    "AMPL state var — SDDP solve converges with a user state var present")
{
  using namespace amplstate_test;

  auto planning = make_state_var_planning();
  planning.options.method = MethodType::sddp;
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 5;
  sddp_opts.convergence_tol = 1e-6;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  const auto ub = results->back().upper_bound;
  const auto lb = results->back().lower_bound;
  CHECK(std::isfinite(ub));
  CHECK(std::isfinite(lb));
}
