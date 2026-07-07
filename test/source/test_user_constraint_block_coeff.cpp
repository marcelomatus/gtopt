/**
 * @file      test_user_constraint_block_coeff.cpp
 * @brief     Tests for per-block (F9) coefficients on UserConstraint LHS terms
 * @date      2026-05-27
 * @copyright BSD-3-Clause
 *
 * A UserConstraint LHS term may carry a per-block coefficient *profile*
 * (`[v0, v1, ...] * element.attr`) instead of a single scalar.  At LP
 * assembly the term's coefficient for block ordinal `b` (0-based within the
 * stage) is `profile[min(b, size-1)]`, the symmetric LHS analog of the
 * per-block `rhs [...]` schedule.  These tests pin:
 *
 *   (a) parser round-trip of a per-block-coefficient term (also exercised in
 *       test_constraint_parser.cpp);
 *   (b) LP assembly — a non-uniform profile produces a per-block-distinct LP
 *       that solves to a different objective than the uniform scalar form;
 *   (c) backward-compat — a scalar coefficient and an equivalent uniform
 *       profile assemble to the identical objective;
 *   (d) JSON round-trip of a bracketed-profile expression.
 */

#include <string>
#include <string_view>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/constraint_parser.hpp>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/json/json_user_constraint.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/user_constraint.hpp>

using namespace gtopt;

// Unique-named outer namespace avoids unity-build helper-name collisions.
namespace ucbc_test
{
namespace
{

// clang-format off

/// 1-stage, 3-block, single-bus system (each block 1 h).  Flat 100 MW demand
/// every block.  g1 cheap (gcost 5, pmax 100), g2 expensive (gcost 50, pmax
/// 100).  scale_objective = 1 keeps the raw objective equal to physical cost.
/// `uc` is spliced into user_constraint_array verbatim.
auto make_json(std::string_view uc) -> std::string
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
        "demand_fail_cost": 100000,
        "scale_objective": 1
      }}
    }},
    "simulation": {{
      "block_array": [
        {{ "uid": 1, "duration": 1 }},
        {{ "uid": 2, "duration": 1 }},
        {{ "uid": 3, "duration": 1 }}
      ],
      "stage_array": [
        {{ "uid": 1, "first_block": 0, "count_block": 3, "active": 1,
           "chronological": true }}
      ],
      "scenario_array": [ {{ "uid": 1, "probability_factor": 1 }} ]
    }},
    "system": {{
      "name": "block_coeff_uc_test",
      "bus_array": [ {{ "uid": 1, "name": "b1" }} ],
      "generator_array": [
        {{ "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 100,
           "gcost": 5,  "capacity": 100 }},
        {{ "uid": 2, "name": "g2", "bus": "b1", "pmin": 0, "pmax": 100,
           "gcost": 50, "capacity": 100 }}
      ],
      "demand_array": [
        {{ "uid": 1, "name": "d1", "bus": "b1",
           "lmax": [[100.0, 100.0, 100.0]] }}
      ],
      "user_constraint_array": [{}]
    }}
  }}
)",
      uc);
}

// clang-format on

[[nodiscard]] auto solve(std::string_view uc) -> double
{
  Planning base;
  base.merge(parse_planning_json(make_json(uc)));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  return systems.front().front().linear_interface().get_obj_value_raw();
}

}  // namespace
}  // namespace ucbc_test

TEST_CASE(
    "UserConstraint block-coeff — baseline (no binding constraint)")  // NOLINT
{
  using namespace ucbc_test;
  // Loose constraint; g1 serves all 300 MWh: 300 × 5 = 1500.
  const double obj = solve(R"({ "uid": 1, "name": "noop",
      "expression": "generator('g1').generation >= 0" })");
  CHECK(obj == doctest::Approx(1500.0));
}

TEST_CASE(  // NOLINT
    "UserConstraint block-coeff — profile binds only the blocks it selects")
{
  using namespace ucbc_test;

  // Profile [1, 0, 0]: cap g1 at 40 in block 0 only (coeff 0 ⇒ no cap in
  // blocks 1,2 — `0 * g1 <= 40` is vacuous).
  //   block 0: g1=40 (×5) + g2=60 (×50) = 200 + 3000 = 3200
  //   block 1: g1=100 (×5) = 500
  //   block 2: g1=100 (×5) = 500
  //   total = 4200.
  const double obj = solve(R"({ "uid": 1, "name": "g1_block0_cap",
      "expression": "[1, 0, 0] * generator('g1').generation <= 40" })");
  CHECK(obj == doctest::Approx(4200.0));

  // Distinguish from the WRONG expansions:
  //  - uniform 1*g1<=40 every block: 3 × 3200 = 9600.
  //  - no constraint at all:          1500.
  CHECK(obj != doctest::Approx(9600.0));
  CHECK(obj != doctest::Approx(1500.0));
}

TEST_CASE(  // NOLINT
    "UserConstraint block-coeff — distinct per-block caps via profile")
{
  using namespace ucbc_test;

  // Coefficient profile [1, 1, 0]: cap g1<=40 in blocks 0,1; vacuous (0
  // coeff) in block 2.
  //   block 0: g1=40, g2=60  → 200 + 3000 = 3200
  //   block 1: g1=40, g2=60  → 3200
  //   block 2: g1=100        → 500
  //   total = 6900.
  const double obj = solve(R"({ "uid": 1, "name": "g1_block01_cap",
      "expression": "[1, 1, 0] * generator('g1').generation <= 40" })");
  CHECK(obj == doctest::Approx(6900.0));
}

TEST_CASE(  // NOLINT
    "UserConstraint block-coeff — short profile broadcasts its last entry")
{
  using namespace ucbc_test;

  // Profile [1] (length 1) broadcasts to all 3 blocks → uniform 1*g1<=40.
  //   3 × 3200 = 9600.
  const double obj = solve(R"({ "uid": 1, "name": "g1_short_profile",
      "expression": "[1] * generator('g1').generation <= 40" })");
  CHECK(obj == doctest::Approx(9600.0));
}

TEST_CASE(  // NOLINT
    "UserConstraint block-coeff — scalar and uniform profile are identical")
{
  using namespace ucbc_test;

  // Backward-compat: a scalar coefficient and an equivalent uniform profile
  // must assemble to the identical objective.
  const double scalar_obj = solve(R"({ "uid": 1, "name": "scalar_cap",
      "expression": "1 * generator('g1').generation <= 40" })");
  const double profile_obj = solve(R"({ "uid": 1, "name": "uniform_profile",
      "expression": "[1, 1, 1] * generator('g1').generation <= 40" })");

  CHECK(scalar_obj == doctest::Approx(9600.0));
  CHECK(profile_obj == doctest::Approx(scalar_obj));
}

TEST_CASE(
    "UserConstraint block-coeff — bracketed expression round-trips JSON")  // NOLINT
{
  using namespace ucbc_test;

  constexpr std::string_view uc_json = R"({
    "uid": 7,
    "name": "block_coeff_rt",
    "expression": "[1, 2, 3] * generator('g1').generation <= 100"
  })";

  const auto uc = daw::json::from_json<UserConstraint>(uc_json);
  CHECK(uc.uid == Uid {7});
  CHECK(uc.name == "block_coeff_rt");
  CHECK(uc.expression == "[1, 2, 3] * generator('g1').generation <= 100");

  // Serialize then parse again — the bracketed profile survives unchanged
  // (it lives entirely inside the `expression` string).
  const auto out = daw::json::to_json(uc);
  const auto uc2 = daw::json::from_json<UserConstraint>(out);
  CHECK(uc2.expression == uc.expression);

  // And the round-tripped expression still parses to a per-block profile.
  auto expr = ConstraintParser::parse(uc2.name, uc2.expression);
  REQUIRE(expr.terms.size() == 1);
  REQUIRE(expr.terms[0].coeff_profile.has_value());
  REQUIRE(expr.terms[0].coeff_profile->size() == 3);
  CHECK(expr.terms[0].coeff_at(0) == doctest::Approx(1.0));
  CHECK(expr.terms[0].coeff_at(1) == doctest::Approx(2.0));
  CHECK(expr.terms[0].coeff_at(2) == doctest::Approx(3.0));
}
