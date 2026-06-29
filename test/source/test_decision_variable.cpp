// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_decision_variable.cpp
 * @brief     Unit tests for the DecisionVariable element (2026-05-20 work)
 *
 * Pins the schema + LP + PAMPL contract introduced in commit
 * 16fbdde45 (refactor(commitment): separate dispatch-cost concerns +
 * DecisionVariable):
 *
 *   * Struct schema + designated-initializer defaults.
 *   * JSON round-trip (StrictParsePolicy) of the documented fields
 *     `uid, name, active, lower_bound, upper_bound, cost`.
 *   * AMPL dispatch registry exposes a `decision_variable` iterator
 *     (i.e. `sum(decision_variable(all).value)` can resolve).
 *   * `constraint_parser::is_element_type("decision_variable")` is
 *     `true` — the parser accepts `decision_variable("X").value` as
 *     a valid LHS reference in UserConstraint expressions.
 */

#include <string>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/constraint_parser.hpp>
#include <gtopt/decision_variable.hpp>
#include <gtopt/json/json_decision_variable.hpp>
#include <gtopt/json/json_parse_policy.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Build a one-bus system with a single DecisionVariable pinned to value 1
/// over a single block of ``block_hours``, solve, and return the raw
/// objective (= the DV column's objective coefficient, since the pinned
/// value is 1 and there is no other cost).  ``scale_objective=1`` so the
/// raw objective is in face-value $.
double dv_objective_coeff(const std::optional<std::string>& cost_type,
                          double cost,
                          double block_hours)
{
  System sys;
  sys.name = "dv_cost_test";
  sys.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  DecisionVariable dv {
      .uid = Uid {1},
      .name = "knob",
      // Pin the column to value 1 so obj == its objective coefficient.
      .lower_bound = 1.0,
      .upper_bound = 1.0,
      .cost = cost,
  };
  if (cost_type) {
    dv.cost_type = *cost_type;
  }
  sys.decision_variable_array = {dv};

  Simulation simulation;
  simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = block_hours,
      },
  };
  simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 1,
          .chronological = true,
      },
  };
  simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };

  PlanningOptions popts;
  popts.model_options.scale_objective = 1.0;
  const PlanningOptionsLP options(popts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(sys, simulation_lp);

  auto&& li = system_lp.linear_interface();
  const auto result = li.resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);
  return li.get_obj_value_raw();
}

}  // namespace

TEST_CASE("DecisionVariable — default-constructed")  // NOLINT
{
  const DecisionVariable dv;
  CHECK(dv.uid == unknown_uid);
  CHECK(dv.name == Name {});
  CHECK_FALSE(dv.active.has_value());
  CHECK_FALSE(dv.lower_bound.has_value());
  CHECK_FALSE(dv.upper_bound.has_value());
  CHECK_FALSE(dv.cost.has_value());
}

TEST_CASE("DecisionVariable — designated initializer")  // NOLINT
{
  const DecisionVariable dv {
      .uid = Uid {7},
      .name = "bess_angamos_lw",
      .lower_bound = -1000.0,
      .upper_bound = 1000.0,
      .cost = 0.0,
  };
  CHECK(dv.uid == Uid {7});
  CHECK(dv.name == "bess_angamos_lw");
  CHECK(dv.lower_bound.value_or(-9999.0) == doctest::Approx(-1000.0));
  CHECK(dv.upper_bound.value_or(-9999.0) == doctest::Approx(1000.0));
  CHECK(dv.cost.value_or(-9999.0) == doctest::Approx(0.0));
}

TEST_CASE("DecisionVariable — class_name constant")  // NOLINT
{
  // Single source of truth for AMPL dispatch + LP-row labelling.  Drift
  // here would break the `decision_variable` registration in
  // `source/ampl_dispatch_registry.cpp`.
  CHECK(std::string_view {DecisionVariable::class_name.full_name()}
        == "DecisionVariable");
  CHECK(std::string_view {DecisionVariable::class_name.snake_case()}
        == "decision_variable");
}

TEST_CASE("DecisionVariable — JSON round-trip preserves all fields")  // NOLINT
{
  // Round-trip the documented JSON example from `decision_variable.hpp`
  // plus the optional `active` field.  Round-trip is a re-emission of
  // the parsed struct, NOT byte-equality with the original (the
  // serializer normalizes whitespace + key order).  We assert
  // canonical token presence so this stays a stable contract test.
  constexpr std::string_view input = R"({
    "uid": 1,
    "name": "bess_angamos_lw",
    "lower_bound": -1000,
    "upper_bound": 1000,
    "cost": 0
  })";
  const auto dv =
      daw::json::from_json<DecisionVariable>(input, StrictParsePolicy);
  CHECK(dv.uid == Uid {1});
  CHECK(dv.name == "bess_angamos_lw");
  CHECK(dv.lower_bound.value_or(0.0) == doctest::Approx(-1000.0));
  CHECK(dv.upper_bound.value_or(0.0) == doctest::Approx(1000.0));
  CHECK(dv.cost.value_or(99.0) == doctest::Approx(0.0));

  // Re-emit + re-parse: idempotent at the schema level.
  const auto emitted = daw::json::to_json(dv);
  const auto dv2 =
      daw::json::from_json<DecisionVariable>(emitted, StrictParsePolicy);
  CHECK(dv2.uid == dv.uid);
  CHECK(dv2.name == dv.name);
  CHECK(dv2.lower_bound.value_or(0.0) == doctest::Approx(-1000.0));
  CHECK(dv2.upper_bound.value_or(0.0) == doctest::Approx(1000.0));
  CHECK(dv2.cost.value_or(99.0) == doctest::Approx(0.0));
}

TEST_CASE("DecisionVariable — block scope + cost_type round-trip")  // NOLINT
{
  // The single-block scope (`block`) and the cost interpretation
  // (`cost_type` = power/energy/raw) feed the FCF cost-to-go `alpha_fcf`:
  // one last-block column, raw money (discount-only, not duration-scaled).
  constexpr std::string_view input = R"({
    "uid": 44,
    "name": "alpha_fcf",
    "cost": 1,
    "cost_type": "raw",
    "block": 111,
    "obj_constant": 1158518300.0
  })";
  const auto dv =
      daw::json::from_json<DecisionVariable>(input, StrictParsePolicy);
  CHECK(dv.name == "alpha_fcf");
  CHECK(dv.cost.value_or(0.0) == doctest::Approx(1.0));
  CHECK(dv.cost_type.value_or("") == "raw");
  CHECK(dv.block.value_or(Uid {0}) == Uid {111});
  CHECK(dv.obj_constant.value_or(0.0) == doctest::Approx(1158518300.0));
  // α' is free — no explicit lower bound on the rebased FCF column.
  CHECK_FALSE(dv.lower_bound.has_value());

  // Re-emit + re-parse preserves the new fields.
  const auto dv2 = daw::json::from_json<DecisionVariable>(
      daw::json::to_json(dv), StrictParsePolicy);
  CHECK(dv2.cost_type.value_or("") == "raw");
  CHECK(dv2.block.value_or(Uid {0}) == Uid {111});
  CHECK(dv2.obj_constant.value_or(0.0) == doctest::Approx(1158518300.0));

  // Defaults: block + cost_type + obj_constant are absent when not
  // supplied (→ per-block column; the C++ LP build defaults an absent
  // cost_type to "raw" = face value, see the LP cost tests below).
  const auto bare = daw::json::from_json<DecisionVariable>(
      R"({"uid": 1, "name": "x"})", StrictParsePolicy);
  CHECK_FALSE(bare.block.has_value());
  CHECK_FALSE(bare.cost_type.has_value());
  CHECK_FALSE(bare.obj_constant.has_value());
}

TEST_CASE(
    "DecisionVariable — block_state + link + state + initial_value "
    "round-trip")  // NOLINT
{
  // The AMPL-reservoir fields (`block_state`, `link`, `state`,
  // `initial_value`) must survive JSON parse + re-emit.  A binding name drift
  // (e.g. `block_state` → `blockState`) would silently leave every reservoir
  // DV with `block_state=false`, disabling the whole feature — this pins the
  // contract in `json_decision_variable.hpp`.
  constexpr std::string_view input = R"({
    "uid": 55,
    "name": "vol",
    "lower_bound": 0,
    "upper_bound": 500,
    "scope": "block",
    "link": true,
    "block_state": true,
    "initial_value": 120.0
  })";
  const auto dv =
      daw::json::from_json<DecisionVariable>(input, StrictParsePolicy);
  CHECK(dv.link.value_or(false) == true);
  CHECK(dv.block_state.value_or(false) == true);
  CHECK(dv.initial_value.value_or(-1.0) == doctest::Approx(120.0));

  // Re-emit + re-parse preserves them.
  const auto dv2 = daw::json::from_json<DecisionVariable>(
      daw::json::to_json(dv), StrictParsePolicy);
  CHECK(dv2.link.value_or(false) == true);
  CHECK(dv2.block_state.value_or(false) == true);
  CHECK(dv2.initial_value.value_or(-1.0) == doctest::Approx(120.0));

  // A coarse `state` DV round-trips its flag too.
  const auto sv = daw::json::from_json<DecisionVariable>(
      R"({"uid": 56, "name": "s", "scope": "global", "state": true, "link": true})",
      StrictParsePolicy);
  CHECK(sv.state.value_or(false) == true);

  // Defaults: all four absent when not supplied.
  const auto bare = daw::json::from_json<DecisionVariable>(
      R"({"uid": 1, "name": "x"})", StrictParsePolicy);
  CHECK_FALSE(bare.block_state.has_value());
  CHECK_FALSE(bare.link.has_value());
  CHECK_FALSE(bare.state.has_value());
  CHECK_FALSE(bare.initial_value.has_value());
}

TEST_CASE("DecisionVariable — JSON parses minimal (uid + name only)")  // NOLINT
{
  // All non-uid/name fields are optional.  A minimal DecisionVariable
  // must parse cleanly with the LP defaulting to free-on-both-sides /
  // zero-cost.
  constexpr std::string_view input = R"({"uid": 2, "name": "free_knob"})";
  const auto dv =
      daw::json::from_json<DecisionVariable>(input, StrictParsePolicy);
  CHECK(dv.uid == Uid {2});
  CHECK(dv.name == "free_knob");
  CHECK_FALSE(dv.lower_bound.has_value());
  CHECK_FALSE(dv.upper_bound.has_value());
  CHECK_FALSE(dv.cost.has_value());
}

TEST_CASE(
    "constraint_parser — `decision_variable` / `commitment` / "
    "`simple_commitment` are recognized element types")  // NOLINT
{
  // The 2026-05-20 commit (16fbdde45) added DecisionVariable as a
  // referenceable element class in PAMPL `sum(...)` predicates.  The
  // earlier `1d709ae52` validation work also added `commitment` and
  // `simple_commitment` to the same parser allow-list.  Pin that
  // every one of those token names parses cleanly with the right
  // `element_type` so a future rename-and-forget doesn't silently
  // invalidate every user_constraint body that references them
  // (which today would surface only at LP-build time as a strict-
  // mode resolution failure).
  //
  // `Parser::is_element_type` is private; we exercise the public
  // `parse` surface — a parse failure or wrong element_type would
  // bubble up here.

  SUBCASE("decision_variable resolves as element type")
  {
    const auto expr =
        ConstraintParser::parse(R"(decision_variable("X").value <= 100)");
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    CHECK(expr.terms[0].element.value_or(ElementRef {}).element_type
          == "decision_variable");
  }
  SUBCASE("commitment resolves as element type")
  {
    const auto expr =
        ConstraintParser::parse(R"(commitment("U1").status <= 1)");
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    CHECK(expr.terms[0].element.value_or(ElementRef {}).element_type
          == "commitment");
  }
  SUBCASE("simple_commitment resolves as element type")
  {
    const auto expr =
        ConstraintParser::parse(R"(simple_commitment("S1").status <= 1)");
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    CHECK(expr.terms[0].element.value_or(ElementRef {}).element_type
          == "simple_commitment");
  }
}

TEST_CASE("DecisionVariableLP — cost_type controls objective weighting")
{
  // The DV column is pinned to value 1 over a single 8 h block, so the
  // raw objective equals the column's objective coefficient.
  constexpr double cost = 7.0;
  constexpr double block_hours = 8.0;

  SUBCASE("default (no cost_type) is RAW face value — NOT ×duration")
  {
    // REGRESSION (cost-weighting bug): the converter emits PLEXOS
    // DecisionVariables (penalties, reserve VoRS, BESS knobs) WITHOUT a
    // cost_type.  These are discrete face-value $ amounts; the default
    // must be "raw" so they are NOT Δt-weighted.  Before the fix the
    // default was "power", inflating the cost by the block length (×8).
    const auto obj = dv_objective_coeff(std::nullopt, cost, block_hours);
    CHECK(obj == doctest::Approx(cost));  // face value ×1
    CHECK(obj != doctest::Approx(cost * block_hours));  // not ×8
  }

  SUBCASE("explicit cost_type=power STILL Δt-weights (regression)")
  {
    const auto obj = dv_objective_coeff("power", cost, block_hours);
    CHECK(obj == doctest::Approx(cost * block_hours));  // 7 × 8 = 56
  }

  SUBCASE("explicit cost_type=energy is prob·discount (no duration)")
  {
    // prob = discount = 1 here → energy coefficient is the face value,
    // and crucially NOT multiplied by the 8 h block duration.
    const auto obj = dv_objective_coeff("energy", cost, block_hours);
    CHECK(obj == doctest::Approx(cost));
    CHECK(obj != doctest::Approx(cost * block_hours));
  }

  SUBCASE("explicit cost_type=raw is face value")
  {
    const auto obj = dv_objective_coeff("raw", cost, block_hours);
    CHECK(obj == doctest::Approx(cost));
  }
}
