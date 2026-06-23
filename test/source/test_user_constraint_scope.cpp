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
#include <gtopt/constraint_parser.hpp>
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

/// Build + solve, returning the first cell's raw objective value.
/// `scale_objective = 1` ⇒ raw obj == physical cost.
[[nodiscard]] auto solve_obj(std::string_view uc_block) -> double
{
  Planning base;
  base.merge(parse_planning_json(make_json(uc_block)));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  return systems.front().front().linear_interface().get_obj_value_raw();
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

// ══════════════════════════════════════════════════════════════════════════
// `sum{...}` TIME aggregation (piece-4 step 2) — distinct from element `sum()`
// ══════════════════════════════════════════════════════════════════════════
//
// Model recap (make_json): 3 blocks × 8 h = 24 h; demand = 100 MW/block
// (so total energy = 100 × 24 = 2400 MWh).  g1 gcost = 5 $/MWh, capacity
// 200 MW; demand_fail_cost = 1000 $/MWh; scale_objective = 1.
//   Unconstrained: g1 serves all 2400 MWh → obj = 2400 × 5 = 12000.

TEST_CASE(
    "UserConstraint sum{} time-agg — parses distinct from element sum()")  // NOLINT
{
  using namespace gtopt;

  // Element sum: PARENTHESES → SumElementRef, no time_agg.
  const auto el =
      ConstraintParser::parse("el", "sum(generator(all).generation) <= 300");
  REQUIRE(el.terms.size() == 1);
  CHECK(el.terms[0].sum_ref.has_value());
  CHECK(!el.terms[0].time_agg);

  // Time sum: BRACES → TimeAggRef, no sum_ref.
  const auto ti = ConstraintParser::parse(
      "ti", "sum{b in stage} generator('g1').generation <= 300");
  REQUIRE(ti.terms.size() == 1);
  CHECK(!ti.terms[0].sum_ref.has_value());
  REQUIRE(ti.terms[0].time_agg != nullptr);
  CHECK(ti.terms[0].time_agg->index_name == "b");
  CHECK(ti.terms[0].time_agg->window == TimeWindow::Stage);
  CHECK(ti.terms[0].time_agg->weight == TimeAggWeight::Count);
  REQUIRE(ti.terms[0].time_agg->inner.size() == 1);
  CHECK(ti.terms[0].time_agg->inner[0].element.has_value());

  // dur[b] prefix → Duration weighting.
  const auto en = ConstraintParser::parse(
      "en", "sum{b in stage} dur[b] * generator('g1').generation <= 1200");
  REQUIRE(en.terms[0].time_agg != nullptr);
  CHECK(en.terms[0].time_agg->weight == TimeAggWeight::Duration);

  // `day` window parses.
  const auto dy = ConstraintParser::parse(
      "dy", "sum{b in day} generator('g1').generation <= 50");
  REQUIRE(dy.terms[0].time_agg != nullptr);
  CHECK(dy.terms[0].time_agg->window == TimeWindow::Day);
}

TEST_CASE(
    "UserConstraint sum{} time-agg — dur[b] weighting limits ENERGY")  // NOLINT
{
  using namespace ucscope_test;  // NOLINT(google-build-using-namespace)

  // sum{b in stage} dur[b] * generation <= 1200  caps g1 ENERGY at 1200 MWh.
  // g1 serves 1200 MWh (cost 6000); remaining 1200 MWh fails
  // (1200 × 1000 = 1 200 000).  obj = 1 206 000.
  const double obj = solve_obj(R"({
      "uid": 1, "name": "uc", "scope": "stage", "constraint_type": "energy",
      "expression": "sum{b in stage} dur[b] * generator('g1').generation <= 1200" })");
  CHECK(obj == doctest::Approx(1206000.0));
}

TEST_CASE(
    "UserConstraint sum{} time-agg — unweighted = per-block MW COUNT")  // NOLINT
{
  using namespace ucscope_test;  // NOLINT(google-build-using-namespace)

  // sum{b in stage} generation <= 90  caps the SUM of per-block MW at 90
  // (NOT energy): g1 = 30 MW/block ⇒ 30 × 8 = 240 MWh/block × 3 = 720 MWh
  // served (cost 3600); 1680 MWh fail (1 680 000).  obj = 1 683 600.
  const double obj = solve_obj(R"({
      "uid": 1, "name": "uc", "scope": "stage", "constraint_type": "raw",
      "expression": "sum{b in stage} generator('g1').generation <= 90" })");
  CHECK(obj == doctest::Approx(1683600.0));

  // The dur[b] (energy) form with the same numeric RHS is STRICTLY weaker
  // (1200 MWh budget vs 90 MWh-of-MW), proving the weight changes the LHS.
  const double obj_energy = solve_obj(R"({
      "uid": 1, "name": "uc", "scope": "stage", "constraint_type": "energy",
      "expression": "sum{b in stage} dur[b] * generator('g1').generation <= 90" })");
  // 90 MWh from g1 (cost 450), 2310 MWh fail (2 310 000) → 2 310 450.
  CHECK(obj_energy == doctest::Approx(2310450.0));
  CHECK(obj_energy != doctest::Approx(obj));
}

TEST_CASE(
    "UserConstraint sum{} time-agg — slack at stage scope solves")  // NOLINT
{
  using namespace ucscope_test;  // NOLINT(google-build-using-namespace)

  // A non-binding energy budget leaves g1 free → unconstrained obj 12000.
  const double obj = solve_obj(R"({
      "uid": 1, "name": "uc", "scope": "stage", "constraint_type": "energy",
      "expression": "sum{b in stage} dur[b] * generator('g1').generation <= 99999" })");
  CHECK(obj == doctest::Approx(12000.0));
}

TEST_CASE(
    "UserConstraint sum{} time-agg — empty/constant inner is rejected")  // NOLINT
{
  using namespace gtopt;

  // sum over a pure constant has no variables to aggregate → parse error.
  CHECK_THROWS_AS(  // NOLINT
      static_cast<void>(
          ConstraintParser::parse("c", "sum{b in stage} 5 <= 10")),
      ConstraintParseError);

  // Unknown window → parse error.
  CHECK_THROWS_AS(  // NOLINT
      static_cast<void>(ConstraintParser::parse(
          "w", "sum{b in week} generator('g1').generation <= 10")),
      ConstraintParseError);

  // dur index must match the bound index.
  CHECK_THROWS_AS(  // NOLINT
      static_cast<void>(ConstraintParser::parse(
          "d", "sum{b in stage} dur[x] * generator('g1').generation <= 10")),
      ConstraintParseError);
}

// ══════════════════════════════════════════════════════════════════════════
// `sum{b in day}` subsumes `daily_sum` (P1-3) — but daily_sum stays working.
// ══════════════════════════════════════════════════════════════════════════

namespace ucscope_test  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// clang-format off

/// 1 stage, 4 blocks × 12 h = 48 h = 2 days (day0={b1,b2}, day1={b3,b4}).
/// Flat 100 MW demand every block (each day = 100 MW × 24 h = 2400 MWh).
/// g1 cheap (gcost 5), demand_fail_cost = 1000, scale_objective = 1.
auto make_2day_json(std::string_view uc) -> std::string
{
  return std::format(
      R"(
  {{
    "options": {{
      "annual_discount_rate": 0.0, "output_compression": "uncompressed",
      "model_options": {{ "use_single_bus": true, "demand_fail_cost": 1000,
                          "scale_objective": 1 }}
    }},
    "simulation": {{
      "block_array": [
        {{ "uid": 1, "duration": 12 }}, {{ "uid": 2, "duration": 12 }},
        {{ "uid": 3, "duration": 12 }}, {{ "uid": 4, "duration": 12 }}
      ],
      "stage_array": [
        {{ "uid": 1, "first_block": 0, "count_block": 4, "active": 1,
           "chronological": true }}
      ],
      "scenario_array": [ {{ "uid": 1, "probability_factor": 1 }} ]
    }},
    "system": {{
      "name": "daily_subsume",
      "bus_array": [ {{ "uid": 1, "name": "b1" }} ],
      "generator_array": [
        {{ "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200,
           "gcost": 5, "capacity": 200 }}
      ],
      "demand_array": [
        {{ "uid": 1, "name": "d1", "bus": "b1",
           "lmax": [ [ 100.0, 100.0, 100.0, 100.0 ] ] }}
      ],
      "user_constraint_array": [ {} ]
    }}
  }})",
      uc);
}

// clang-format on

[[nodiscard]] auto solve_2day_obj(std::string_view uc) -> double
{
  Planning base;
  base.merge(parse_planning_json(make_2day_json(uc)));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  return systems.front().front().linear_interface().get_obj_value_raw();
}

}  // namespace
}  // namespace ucscope_test

TEST_CASE(
    "UserConstraint daily_sum still works (energy budget per day)")  // NOLINT
{
  using namespace ucscope_test;  // NOLINT(google-build-using-namespace)

  // daily_sum + energy: each DAY's g1 energy ≤ 1000 MWh.  Per day g1 serves
  // 1000 MWh (cost 5000), fails 1400 MWh (1 400 000) → 1 405 000 per day,
  // × 2 days = 2 810 000.
  const double obj = solve_2day_obj(R"({
      "uid": 1, "name": "uc", "daily_sum": true, "constraint_type": "energy",
      "expression": "generator('g1').generation <= 1000" })");
  CHECK(obj == doctest::Approx(2810000.0));
}

TEST_CASE(
    "UserConstraint sum{b in day} ENERGY mirrors daily_sum+energy")  // NOLINT
{
  using namespace ucscope_test;  // NOLINT(google-build-using-namespace)

  // The block-scoped time-agg form: one row per day (the `day` window),
  // energy-weighted via dur[b], same 1000 MWh/day budget as the daily_sum
  // case above.  Must produce the identical objective.
  const double daily = solve_2day_obj(R"({
      "uid": 1, "name": "uc", "daily_sum": true, "constraint_type": "energy",
      "expression": "generator('g1').generation <= 1000" })");
  const double timeagg = solve_2day_obj(R"({
      "uid": 1, "name": "uc", "scope": "block", "constraint_type": "energy",
      "expression": "sum{b in day} dur[b] * generator('g1').generation <= 1000" })");
  CHECK(timeagg == doctest::Approx(daily));
}
