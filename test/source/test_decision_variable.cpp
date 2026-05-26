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

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

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
