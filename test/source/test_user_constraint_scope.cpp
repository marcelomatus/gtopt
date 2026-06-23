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

#include <stdexcept>
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
// [G10] multi-scene phase/global scope — no cross-scene dedup-collapse
// ══════════════════════════════════════════════════════════════════════════
//
// `phase`/`global` scoped constraints emit ONE row per (scene, phase) cell.
// The worry is the metadata-based duplicate detector in
// `LinearProblem::add_row`: two DIFFERENT scenes building the SAME phase-scope
// row could collide if the row's identity does not discriminate on scene.  The
// `PhaseContext` (scene_uid, phase_uid, element_uid) discriminator prevents
// that — here we BUILD a 2-scene, 2-phase model and assert EACH scene's cell
// carries its OWN phase/global row (so the second scene's row is not silently
// dropped).

namespace ucscope_test  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// clang-format off

/// A 2-scene × 2-phase single-bus model: 2 stages (one per phase), 2
/// scenarios each folded into its own scene.  `uc_block` varies the scope.
auto make_2scene_2phase_json(std::string_view uc_block) -> std::string
{
  return std::format(
      R"(
  {{
    "options": {{
      "annual_discount_rate": 0.0, "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {{ "use_single_bus": true, "demand_fail_cost": 1000,
                          "scale_objective": 1 }}
    }},
    "simulation": {{
      "block_array": [
        {{ "uid": 1, "duration": 8 }}, {{ "uid": 2, "duration": 8 }},
        {{ "uid": 3, "duration": 8 }}, {{ "uid": 4, "duration": 8 }}
      ],
      "stage_array": [
        {{ "uid": 1, "first_block": 0, "count_block": 2, "active": 1 }},
        {{ "uid": 2, "first_block": 2, "count_block": 2, "active": 1 }}
      ],
      "phase_array": [
        {{ "uid": 1, "first_stage": 0, "count_stage": 1 }},
        {{ "uid": 2, "first_stage": 1, "count_stage": 1 }}
      ],
      "scenario_array": [
        {{ "uid": 1, "probability_factor": 0.5 }},
        {{ "uid": 2, "probability_factor": 0.5 }}
      ],
      "scene_array": [
        {{ "uid": 1, "name": "s1", "active": 1, "first_scenario": 0,
           "count_scenario": 1 }},
        {{ "uid": 2, "name": "s2", "active": 1, "first_scenario": 1,
           "count_scenario": 1 }}
      ]
    }},
    "system": {{
      "name": "uc_multiscene_test",
      "bus_array": [ {{ "uid": 1, "name": "b1" }} ],
      "generator_array": [
        {{ "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200,
           "gcost": 5, "capacity": 200 }}
      ],
      "demand_array": [
        {{ "uid": 1, "name": "d1", "bus": "b1", "capacity": 100.0 }}
      ],
      "user_constraint_array": [ {} ]
    }}
  }})",
      uc_block);
}

// clang-format on

/// Resolve the 2-scene 2-phase model and return the row count of cell
/// (scene, phase).
[[nodiscard]] auto solve_cell_numrows(std::string_view uc_block,
                                      SceneIndex scene,
                                      PhaseIndex phase) -> Index
{
  Planning base;
  base.merge(parse_planning_json(make_2scene_2phase_json(uc_block)));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  auto&& systems = planning_lp.systems();
  REQUIRE(systems.size() >= 2);  // 2 scenes
  REQUIRE(systems[scene].size() >= 2);  // 2 phases
  return systems[scene][phase].linear_interface().get_numrows();
}

}  // namespace
}  // namespace ucscope_test

TEST_CASE(
    "UserConstraint scope — multi-scene phase/global rows do not collapse")  // NOLINT
{
  using namespace ucscope_test;  // NOLINT(google-build-using-namespace)

  constexpr auto expr = R"("expression": "generator('g1').generation <= 150")";

  // Baseline: a far-from-binding NOOP at each scope — establishes the per-cell
  // row floor WITHOUT the constraint of interest contributing a meaningful
  // delta.  We instead compare block vs phase/global directly per cell.
  const auto blk_s0p0 = solve_cell_numrows(
      std::format(R"({{ "uid": 1, "name": "uc", "scope": "block", {} }})",
                  expr),
      SceneIndex {0},
      PhaseIndex {0});
  const auto ph_s0p0 = solve_cell_numrows(
      std::format(R"({{ "uid": 1, "name": "uc", "scope": "phase", {} }})",
                  expr),
      SceneIndex {0},
      PhaseIndex {0});
  const auto ph_s1p0 = solve_cell_numrows(
      std::format(R"({{ "uid": 1, "name": "uc", "scope": "phase", {} }})",
                  expr),
      SceneIndex {1},
      PhaseIndex {0});
  const auto gl_s0p0 = solve_cell_numrows(
      std::format(R"({{ "uid": 1, "name": "uc", "scope": "global", {} }})",
                  expr),
      SceneIndex {0},
      PhaseIndex {0});
  const auto gl_s1p0 = solve_cell_numrows(
      std::format(R"({{ "uid": 1, "name": "uc", "scope": "global", {} }})",
                  expr),
      SceneIndex {1},
      PhaseIndex {0});

  // Each phase has 2 blocks, so a block-scoped constraint adds 2 rows per cell;
  // phase/global scope collapses that to exactly 1 row per cell → blk − 1.
  CHECK(ph_s0p0 == blk_s0p0 - 1);
  CHECK(gl_s0p0 == blk_s0p0 - 1);

  // The discriminating check: scene 1's cell ALSO carries its own phase/global
  // row (same count as scene 0's cell) — the second scene's row is NOT dropped
  // by the metadata dedup detector.  Summed across the 2 scenes that is 2
  // phase-scope rows / 2 global-scope rows for phase 0.
  CHECK(ph_s1p0 == ph_s0p0);
  CHECK(gl_s1p0 == gl_s0p0);
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

// ─── [G11] FIXED: a time-agg window FINER than a coarse scope is REJECTED.
//
// A coarse scope (`stage`/`phase`/`global`) builds exactly ONE representative
// LP row (`UserConstraintLP::build_coarse_row`), so a `sum{...}` window finer
// than the scope would aggregate only the representative sub-unit and SILENTLY
// drop the rest.  The historical bug: `make_2day_json` is a single 4-block
// stage spanning TWO 24-h days (day0={b1,b2}, day1={b3,b4}); a STAGE-scoped
// `sum{b in day} dur[b]*gen <= 1000` budgeted day0 ALONE and left day1
// unconstrained — measured obj 1 417 000 vs the correct daily_sum golden
// 2 810 000.
//
// The decided fix is NO-SILENT-COLLAPSE: ERROR on the lossy combination at
// `UserConstraintLP` construction.  This case proves the throw fires; the
// "OK cases still build" case below proves the non-lossy combinations and the
// untouched block-scope daily_sum / `sum{b in day}` golden path stay green.
//
// The full lossy CLASS that must throw:
//   scope=stage  + day window     → ERROR (day finer than stage)
//   scope=phase  + day window     → ERROR
//   scope=phase  + stage window   → ERROR (stage finer than phase)
//   scope=global + day window     → ERROR
//   scope=global + stage window   → ERROR
// The non-lossy combinations that must NOT throw:
//   scope=stage  + stage window   → OK (window == scope: whole-stage budget)
//   scope=block  + ANY window     → OK (per-block path is never coarse)

namespace ucscope_test  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Build (and resolve) the 2-day model with @p uc spliced in; return true
/// iff construction/resolve throws `std::runtime_error` (the G11 guard).
[[nodiscard]] auto build_2day_throws(std::string_view uc) -> bool
{
  try {
    Planning base;
    base.merge(parse_planning_json(make_2day_json(uc)));
    PlanningLP planning_lp(std::move(base));
    (void)planning_lp.resolve();
  } catch (const std::runtime_error&) {
    return true;
  }
  return false;
}

}  // namespace
}  // namespace ucscope_test

TEST_CASE(
    "UserConstraint day-window under coarse scope is rejected "
    "(no silent day-drop)")  // NOLINT
{
  using namespace ucscope_test;  // NOLINT(google-build-using-namespace)

  // The exact historical defect: stage scope + `sum{b in day}` dropped day1.
  // Now the construction must throw rather than under-budget.
  CHECK_THROWS_AS(  // NOLINT
      static_cast<void>(solve_2day_obj(R"({
        "uid": 1, "name": "uc", "scope": "stage", "constraint_type": "energy",
        "expression": "sum{b in day} dur[b] * generator('g1').generation <= 1000" })")),
      std::runtime_error);
}

TEST_CASE(
    "UserConstraint time-agg finer-than-scope — full lossy class errors")  // NOLINT
{
  using namespace ucscope_test;  // NOLINT(google-build-using-namespace)

  // ── Lossy combinations: every one must ERROR (no silent sub-unit drop). ──
  // scope=stage + day window
  CHECK(build_2day_throws(R"({
      "uid": 1, "name": "uc", "scope": "stage", "constraint_type": "energy",
      "expression": "sum{b in day} dur[b] * generator('g1').generation <= 1000" })"));
  // scope=phase + day window
  CHECK(build_2day_throws(R"({
      "uid": 1, "name": "uc", "scope": "phase", "constraint_type": "energy",
      "expression": "sum{b in day} dur[b] * generator('g1').generation <= 1000" })"));
  // scope=phase + stage window (stage finer than phase)
  CHECK(build_2day_throws(R"({
      "uid": 1, "name": "uc", "scope": "phase", "constraint_type": "energy",
      "expression": "sum{b in stage} dur[b] * generator('g1').generation <= 1000" })"));
  // scope=global + day window
  CHECK(build_2day_throws(R"({
      "uid": 1, "name": "uc", "scope": "global", "constraint_type": "energy",
      "expression": "sum{b in day} dur[b] * generator('g1').generation <= 1000" })"));
  // scope=global + stage window
  CHECK(build_2day_throws(R"({
      "uid": 1, "name": "uc", "scope": "global", "constraint_type": "energy",
      "expression": "sum{b in stage} dur[b] * generator('g1').generation <= 1000" })"));

  // ── Non-lossy combinations: these must NOT throw. ──
  // scope=stage + stage window — window matches the scope (whole-stage budget).
  CHECK_FALSE(build_2day_throws(R"({
      "uid": 1, "name": "uc", "scope": "stage", "constraint_type": "energy",
      "expression": "sum{b in stage} dur[b] * generator('g1').generation <= 9999999" })"));
  // scope=block + day window — per-block path is never coarse (daily_sum form).
  CHECK_FALSE(build_2day_throws(R"({
      "uid": 1, "name": "uc", "scope": "block", "constraint_type": "energy",
      "expression": "sum{b in day} dur[b] * generator('g1').generation <= 1000" })"));
}
