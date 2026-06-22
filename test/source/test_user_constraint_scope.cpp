/**
 * @file      test_user_constraint_scope.cpp
 * @brief     Contract tests for UserConstraint `scope`
 * (block/stage/phase/global)
 * @date      2026-06-22
 * @copyright BSD-3-Clause
 *
 * Piece-4 step 1 of the FutureCost / UserModel refactor: the typed
 * `scope` JSON enum field (`block` default, `stage`, `phase`, `global`)
 * that reduces a constraint's instantiation granularity.  `block`/`stage`
 * route through the per-(scenario, stage) operational sweep
 * (`add_to_lp`); `phase`/`global` route through the planning passes
 * (`add_to_phase_lp` / `add_to_global_lp`) and produce ONE row per
 * (scene, phase) cell — anchored at the cell's terminal stage/block.
 *
 * The assertions here are STRUCTURAL: they count LP rows so the
 * granularity reduction is observable, and they prove `global`-scoped
 * rows do NOT collide in `LinearProblem::add_row`'s metadata-based
 * duplicate detector (the `PhaseContext` discriminator's job).
 */

#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/user_constraint_enums.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// Unique-named outer namespace avoids unity-build helper-name collisions;
// the nested anonymous namespace gives the helpers internal linkage.
namespace ucscope_test  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// clang-format off

/// A 1-stage, 3-block single-bus system.  `uc_block` is spliced into
/// `user_constraint_array` verbatim so each test can vary the scope clause.
/// `scale_objective = 1` keeps the raw objective equal to the physical cost.
auto make_json(std::string_view uc_block) -> std::string
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
      "name": "uc_scope_test",
      "bus_array": [ {{ "uid": 1, "name": "b1" }} ],
      "generator_array": [
        {{ "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200,
           "gcost": 5, "capacity": 200 }}
      ],
      "demand_array": [
        {{ "uid": 1, "name": "d1", "bus": "b1", "lmax": [ [ 100.0, 100.0, 100.0 ] ] }}
      ],
      "user_constraint_array": [ {} ]
    }}
  }})",
      uc_block);
}

// clang-format on

/// Build + solve the system, returning the total LP row count of the
/// first (scene, phase) cell.  The granularity reduction of `scope` is
/// observable as a delta in this count.
[[nodiscard]] auto solve_numrows(std::string_view uc_block) -> Index
{
  Planning base;
  base.merge(parse_planning_json(make_json(uc_block)));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  return systems.front().front().linear_interface().get_numrows();
}

}  // namespace
}  // namespace ucscope_test

TEST_CASE("UserConstraint scope — enum parsing")  // NOLINT
{
  CHECK(enum_from_name<ConstraintScope>("block") == ConstraintScope::Block);
  CHECK(enum_from_name<ConstraintScope>("stage") == ConstraintScope::Stage);
  CHECK(enum_from_name<ConstraintScope>("phase") == ConstraintScope::Phase);
  CHECK(enum_from_name<ConstraintScope>("global") == ConstraintScope::Global);
  CHECK(!enum_from_name<ConstraintScope>("").has_value());
  CHECK(!enum_from_name<ConstraintScope>("weekly").has_value());

  CHECK(scope_is_planning(ConstraintScope::Phase));
  CHECK(scope_is_planning(ConstraintScope::Global));
  CHECK_FALSE(scope_is_planning(ConstraintScope::Block));
  CHECK_FALSE(scope_is_planning(ConstraintScope::Stage));
}

TEST_CASE("UserConstraint scope — block default = one row per block")  // NOLINT
{
  using namespace ucscope_test;  // NOLINT(google-build-using-namespace)

  // A stage-level attribute (capainst) shares one LP column across all
  // blocks, so a per-block constraint over it still emits one row PER
  // block in the 3-block stage.
  const auto base = solve_numrows(R"({
      "uid": 1, "name": "noop_far",
      "expression": "generator('g1').generation <= 9999" })");

  // Default (no scope) — one row per block: +3 vs a noop that resolves
  // but is never binding (still 3 rows).  We instead compare default vs
  // explicit "block": they must be identical.
  const auto def = solve_numrows(R"({
      "uid": 1, "name": "uc",
      "expression": "generator('g1').generation <= 150" })");
  const auto blk = solve_numrows(R"({
      "uid": 1, "name": "uc", "scope": "block",
      "expression": "generator('g1').generation <= 150" })");

  CHECK(def == blk);  // explicit "block" == implicit default
  CHECK(def == base);  // both add 3 rows over the 3-block stage
}

TEST_CASE("UserConstraint scope — stage emits one row per stage")  // NOLINT
{
  using namespace ucscope_test;  // NOLINT(google-build-using-namespace)

  // Same constraint at block vs stage scope over a 3-block stage:
  // block → 3 rows, stage → 1 row, so block has exactly +2 rows.
  const auto blk = solve_numrows(R"({
      "uid": 1, "name": "uc", "scope": "block",
      "expression": "generator('g1').generation <= 150" })");
  const auto stg = solve_numrows(R"({
      "uid": 1, "name": "uc", "scope": "stage",
      "expression": "generator('g1').generation <= 150" })");

  CHECK(stg == blk - 2);
}

TEST_CASE("UserConstraint scope — phase emits one row per cell")  // NOLINT
{
  using namespace ucscope_test;  // NOLINT(google-build-using-namespace)

  // Single (scene, phase) cell here, so phase-scope is also exactly one
  // row — same count as stage-scope in this single-stage case.
  const auto blk = solve_numrows(R"({
      "uid": 1, "name": "uc", "scope": "block",
      "expression": "generator('g1').generation <= 150" })");
  const auto ph = solve_numrows(R"({
      "uid": 1, "name": "uc", "scope": "phase",
      "expression": "generator('g1').generation <= 150" })");

  CHECK(ph == blk - 2);  // 1 row instead of 3
}

TEST_CASE(
    "UserConstraint scope — global emits one row, no dedup collision")  // NOLINT
{
  using namespace ucscope_test;  // NOLINT(google-build-using-namespace)

  // One global constraint → exactly one row for the whole cell.
  const auto blk = solve_numrows(R"({
      "uid": 1, "name": "uc", "scope": "block",
      "expression": "generator('g1').generation <= 150" })");
  const auto glob1 = solve_numrows(R"({
      "uid": 1, "name": "uc", "scope": "global",
      "expression": "generator('g1').generation <= 150" })");
  CHECK(glob1 == blk - 2);  // 1 row instead of 3

  // TWO distinct global constraints in the same cell must BOTH add a row.
  // A naive `monostate`/`ScenePhaseContext` row would collide in
  // `LinearProblem::add_row`'s metadata-based duplicate detector and the
  // second row would be silently dropped — the `PhaseContext` element-uid
  // discriminator prevents that.
  const auto glob2 = solve_numrows(R"({
      "uid": 1, "name": "uc_a", "scope": "global",
      "expression": "generator('g1').generation <= 150" },
    {
      "uid": 2, "name": "uc_b", "scope": "global",
      "expression": "generator('g1').generation >= 10" })");

  // glob2 has exactly one more row than glob1 (two global rows vs one).
  CHECK(glob2 == glob1 + 1);
}

TEST_CASE("UserConstraint scope — unknown value is a hard error")  // NOLINT
{
  using namespace ucscope_test;  // NOLINT(google-build-using-namespace)

  auto planning = parse_planning_json(make_json(R"({
      "uid": 1, "name": "uc", "scope": "fortnight",
      "expression": "generator('g1').generation <= 150" })"));
  CHECK_THROWS_AS(PlanningLP(std::move(planning)),  // NOLINT
                  std::runtime_error);
}

TEST_CASE("UserConstraint scope — coarse scope solves correctly")  // NOLINT
{
  using namespace ucscope_test;  // NOLINT(google-build-using-namespace)

  // Sanity: a stage-scoped bound on a stage-level attribute (capainst is
  // one column across blocks) constrains the LP and still solves.  All
  // four scopes must produce a feasible LP on this trivial model.
  for (const std::string_view scope : {"block", "stage", "phase", "global"}) {
    const auto uc = std::format(
        R"({{ "uid": 1, "name": "uc", "scope": "{}",
              "expression": "generator('g1').generation <= 150" }})",
        scope);
    Planning base;
    base.merge(parse_planning_json(make_json(uc)));
    PlanningLP planning_lp(std::move(base));
    const auto result = planning_lp.resolve();
    CHECK(result.has_value());
  }
}
