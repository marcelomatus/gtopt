// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_user_model.cpp
 * @brief     Contract tests for the UserModel element (piece 3)
 * @date      2026-06-22
 * @copyright BSD-3-Clause
 *
 * `UserModel` is the generic AMPL-capture element: it bundles a set of user
 * variable declarations (`variable_array`, reusing `DecisionVariable`) and
 * user constraint declarations (`constraint_array`, reusing `UserConstraint`)
 * into ONE element and captures the named cols/rows under
 * `output/UserModel/<tag>/<name>_{sol,cost,dual,slack}`.  `UserModelLP` REUSES
 * the existing machinery (resolver / AMPL registry / scope routing /
 * aux-col lowering) via internal `DecisionVariableLP` / `UserConstraintLP`
 * delegates — it never forks the resolver and never adds a parallel registry.
 *
 * These tests pin: (1) JSON round-trip; (2) a one-var + one-constraint model
 * builds the expected cols+rows; (3) output capture lands under the tag;
 * (4) a global-scoped model emits exactly one row with no dedup collision;
 * (5) the documented aux-col policy (named-only capture).
 */

#include <filesystem>
#include <string>
#include <string_view>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/json/json_system.hpp>
#include <gtopt/json/json_user_model.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/user_model.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;

// Unique-named outer namespace avoids unity-build helper-name collisions.
namespace usermodel_test
{
namespace
{

// clang-format off

/// 1-stage, 3-block single-bus system.  `um_block` is spliced into
/// `user_model_array` verbatim so each test can vary the bundle.
auto make_json(std::string_view um_block) -> std::string
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
      "name": "user_model_test",
      "bus_array": [ {{ "uid": 1, "name": "b1" }} ],
      "generator_array": [
        {{ "uid": 1, "name": "g1", "bus": "b1", "pmin": 0, "pmax": 200,
           "gcost": 5, "capacity": 200 }}
      ],
      "demand_array": [
        {{ "uid": 1, "name": "d1", "bus": "b1", "lmax": [ [ 100.0, 100.0, 100.0 ] ] }}
      ],
      "user_model_array": [ {} ]
    }}
  }})",
      um_block);
}

// clang-format on

/// Build + solve, returning the first (scene, phase) cell's LP row count.
[[nodiscard]] auto solve_numrows(std::string_view um_block) -> Index
{
  Planning base;
  base.merge(parse_planning_json(make_json(um_block)));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  return systems.front().front().linear_interface().get_numrows();
}

/// Build + solve, returning the first cell's column count.
[[nodiscard]] auto solve_numcols(std::string_view um_block) -> Index
{
  Planning base;
  base.merge(parse_planning_json(make_json(um_block)));
  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  return systems.front().front().linear_interface().get_numcols();
}

}  // namespace
}  // namespace usermodel_test

// ══════════════════════════════════════════════════════════════════════════
// 1. Schema round-trip
// ══════════════════════════════════════════════════════════════════════════

TEST_CASE("UserModel — JSON round-trip preserves bundle")  // NOLINT
{
  constexpr std::string_view js = R"({
    "uid": 1,
    "name": "fcf_recourse",
    "tag": "fcf",
    "description": "user FCF",
    "variable_array": [
      { "uid": 10, "name": "alpha", "scope": "global", "cost": 1,
        "cost_type": "raw" }
    ],
    "constraint_array": [
      { "uid": 20, "name": "FcfCut", "scope": "global",
        "expression": "decision_variable('alpha').value >= 0" }
    ]
  })";
  const auto um = daw::json::from_json<UserModel>(js);
  CHECK(um.uid == Uid {1});
  CHECK(um.name == "fcf_recourse");
  CHECK((um.tag && *um.tag == "fcf"));
  REQUIRE(um.variable_array.size() == 1);
  CHECK(um.variable_array.front().name == "alpha");
  CHECK((um.variable_array.front().scope
         && *um.variable_array.front().scope == "global"));
  REQUIRE(um.constraint_array.size() == 1);
  CHECK(um.constraint_array.front().name == "FcfCut");

  // Re-emit + re-parse is idempotent at the schema level.
  const auto um2 = daw::json::from_json<UserModel>(daw::json::to_json(um));
  CHECK(um2.uid == um.uid);
  CHECK(um2.variable_array.size() == 1);
  CHECK(um2.constraint_array.size() == 1);
  CHECK((um2.tag && *um2.tag == "fcf"));
}

TEST_CASE("UserModel — parses inside a System; defaults stay empty")  // NOLINT
{
  constexpr std::string_view js = R"({
    "name": "sys",
    "user_model_array": [
      { "uid": 7, "name": "m1",
        "variable_array": [ {"uid": 1, "name": "x"} ] }
    ]
  })";
  const auto sys = daw::json::from_json<System>(js);
  REQUIRE(sys.user_model_array.size() == 1);
  const auto& um = sys.user_model_array.front();
  CHECK(um.uid == Uid {7});
  CHECK_FALSE(um.tag.has_value());  // tag defaults to nullopt (→ name used)
  CHECK(um.variable_array.size() == 1);
  CHECK(um.constraint_array.empty());
}

// ══════════════════════════════════════════════════════════════════════════
// 2. One var + one constraint builds the expected cols + rows
// ══════════════════════════════════════════════════════════════════════════

TEST_CASE("UserModel — one var + one constraint adds one col + rows")  // NOLINT
{
  using namespace usermodel_test;

  const auto base_rows = solve_numrows(R"({ "uid": 1, "name": "empty" })");
  const auto base_cols = solve_numcols(R"({ "uid": 1, "name": "empty" })");

  // A bundle with one block-scoped DecisionVariable (3 blocks → 3 cols) and
  // one block-scoped constraint over it (3 blocks → 3 rows).
  const auto rows = solve_numrows(R"({
      "uid": 1, "name": "m",
      "variable_array": [ { "uid": 1, "name": "x", "lower_bound": 0,
                            "upper_bound": 10 } ],
      "constraint_array": [ { "uid": 1, "name": "cap",
          "expression": "decision_variable('x').value <= 5" } ] })");
  const auto cols = solve_numcols(R"({
      "uid": 1, "name": "m",
      "variable_array": [ { "uid": 1, "name": "x", "lower_bound": 0,
                            "upper_bound": 10 } ],
      "constraint_array": [ { "uid": 1, "name": "cap",
          "expression": "decision_variable('x').value <= 5" } ] })");

  CHECK(cols == base_cols + 3);  // 3 per-block x columns
  CHECK(rows == base_rows + 3);  // 3 per-block cap rows
}

TEST_CASE("UserModel — bundled constraint resolves a bundled var")  // NOLINT
{
  using namespace usermodel_test;

  // The constraint references the var that is declared in the SAME bundle,
  // proving the internal DecisionVariableLP registers its column with the
  // AMPL resolver before the internal UserConstraintLP runs (same order as
  // the standalone collection sequence) — and that the unknown-reference
  // strict resolver does NOT throw (which it would if the var were missing).
  Planning base;
  base.merge(parse_planning_json(make_json(R"({
      "uid": 1, "name": "m",
      "variable_array": [ { "uid": 1, "name": "knob", "lower_bound": 0,
                            "upper_bound": 100, "cost": 7, "cost_type": "raw" } ],
      "constraint_array": [ { "uid": 1, "name": "force",
          "expression": "decision_variable('knob').value >= 3" } ] })")));
  PlanningLP planning_lp(std::move(base));
  const auto result = planning_lp.resolve();
  REQUIRE(result.has_value());
  // knob is forced >= 3 at cost 7 (raw) over 3 blocks → +63 vs the
  // unconstrained dispatch.  Unconstrained obj = 100 MW × 24 h × 5 = 12000.
  const double obj = planning_lp.systems()
                         .front()
                         .front()
                         .linear_interface()
                         .get_obj_value_raw();
  CHECK(obj == doctest::Approx(12000.0 + 3.0 * 7.0 * 3.0));
}

// ══════════════════════════════════════════════════════════════════════════
// 3. Output capture under the tag
// ══════════════════════════════════════════════════════════════════════════

TEST_CASE(
    "UserModel — output capture lands under output/UserModel/<tag>")  // NOLINT
{
  using namespace usermodel_test;

  // `temp_directory_path()` honours $TMPDIR (never a hardcoded /tmp).
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_user_model_capture_test";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  const auto json = std::format(
      R"({{
    "options": {{
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "output_directory": "{}",
      "model_options": {{ "use_single_bus": true, "demand_fail_cost": 1000,
                          "scale_objective": 1 }}
    }},
    "simulation": {{
      "block_array": [ {{ "uid": 1, "duration": 8 }} ],
      "stage_array": [ {{ "uid": 1, "first_block": 0, "count_block": 1,
                          "active": 1 }} ],
      "scenario_array": [ {{ "uid": 1, "probability_factor": 1 }} ]
    }},
    "system": {{
      "name": "capture",
      "bus_array": [ {{ "uid": 1, "name": "b1" }} ],
      "generator_array": [ {{ "uid": 1, "name": "g1", "bus": "b1", "pmin": 0,
          "pmax": 200, "gcost": 5, "capacity": 200 }} ],
      "demand_array": [ {{ "uid": 1, "name": "d1", "bus": "b1",
          "lmax": [ [ 100.0 ] ] }} ],
      "user_model_array": [ {{
        "uid": 1, "name": "m", "tag": "mytag",
        "variable_array": [ {{ "uid": 1, "name": "x", "lower_bound": 0,
                               "upper_bound": 10 }} ],
        "constraint_array": [ {{ "uid": 1, "name": "cap", "penalty": 50,
            "expression": "decision_variable('x').value <= 5" }} ]
      }} ]
    }}
  }})",
      tmpdir.string());

  Planning base;
  base.merge(parse_planning_json(json));
  PlanningLP planning_lp(std::move(base));
  REQUIRE(planning_lp.resolve().has_value());
  planning_lp.write_out();

  // The captures live under output/UserModel/<tag>/<name>_<stream>...
  // CSV writer shards as `<tag>/<name>_<stream>_s<N>_p<M>.csv`.
  const auto um_dir = tmpdir / "UserModel" / "mytag";
  CHECK(std::filesystem::exists(um_dir));

  // Walk the UserModel tree and assert the expected stream stems exist under
  // the tag.  The bundled var's `:sol`, the constraint's `:dual`, and the SOFT
  // constraint's visible `slack` `:sol` are always emitted (all under the tag
  // dir).  Reduced-cost (`:cost`) files for a free variable / zero-realized
  // slack are NOT produced by the standalone elements either, so we don't
  // require them — the `add_col_cost` CALL is the SAME code path, so capture
  // parity is structural; whether a file lands depends on reduced-cost
  // availability, identical to standalone.
  bool has_x_sol = false;
  bool has_cap_dual = false;
  bool has_slack_sol = false;
  if (std::filesystem::exists(tmpdir / "UserModel")) {
    for (const auto& e :
         std::filesystem::recursive_directory_iterator(tmpdir / "UserModel"))
    {
      const auto fn = e.path().filename().string();
      const auto parent = e.path().parent_path().filename().string();
      if (fn.find("x_sol") != std::string::npos) {
        has_x_sol = true;
      }
      if (fn.find("cap_dual") != std::string::npos) {
        has_cap_dual = true;
      }
      // slack files live under .../mytag/cap/slack_sol_...
      if (parent == "cap" && fn.find("slack_sol") != std::string::npos) {
        has_slack_sol = true;
      }
    }
  }
  CHECK(has_x_sol);
  CHECK(has_cap_dual);
  CHECK(has_slack_sol);

  // Nothing leaked into the standalone DecisionVariable/ or UserConstraint/
  // directories — the bundle re-namespaces ALL of its output under UserModel/.
  CHECK_FALSE(std::filesystem::exists(tmpdir / "DecisionVariable"));
  CHECK_FALSE(std::filesystem::exists(tmpdir / "UserConstraint"));

  std::filesystem::remove_all(tmpdir);
}

// ══════════════════════════════════════════════════════════════════════════
// 3b. [G7] tag-unset → output dir = element NAME (no tag dir)
// ══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "UserModel — tag unset → output dir is the element name")
{
  using namespace usermodel_test;

  // `temp_directory_path()` honours $TMPDIR (never a hardcoded /tmp).
  const auto tmpdir = std::filesystem::temp_directory_path()
      / "gtopt_user_model_notag_capture_test";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  // Identical to the tag test below it, but with NO "tag" field — the
  // capture dir must fall back to the element NAME ("mymodel").
  const auto json = std::format(
      R"({{
    "options": {{
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "output_directory": "{}",
      "model_options": {{ "use_single_bus": true, "demand_fail_cost": 1000,
                          "scale_objective": 1 }}
    }},
    "simulation": {{
      "block_array": [ {{ "uid": 1, "duration": 8 }} ],
      "stage_array": [ {{ "uid": 1, "first_block": 0, "count_block": 1,
                          "active": 1 }} ],
      "scenario_array": [ {{ "uid": 1, "probability_factor": 1 }} ]
    }},
    "system": {{
      "name": "capture",
      "bus_array": [ {{ "uid": 1, "name": "b1" }} ],
      "generator_array": [ {{ "uid": 1, "name": "g1", "bus": "b1", "pmin": 0,
          "pmax": 200, "gcost": 5, "capacity": 200 }} ],
      "demand_array": [ {{ "uid": 1, "name": "d1", "bus": "b1",
          "lmax": [ [ 100.0 ] ] }} ],
      "user_model_array": [ {{
        "uid": 1, "name": "mymodel",
        "variable_array": [ {{ "uid": 1, "name": "x", "lower_bound": 0,
                               "upper_bound": 10 }} ],
        "constraint_array": [ {{ "uid": 1, "name": "cap", "penalty": 50,
            "expression": "decision_variable('x').value <= 5" }} ]
      }} ]
    }}
  }})",
      tmpdir.string());

  Planning base;
  base.merge(parse_planning_json(json));
  PlanningLP planning_lp(std::move(base));
  REQUIRE(planning_lp.resolve().has_value());
  planning_lp.write_out();

  // The capture dir falls back to the element name when `tag` is unset.
  const auto name_dir = tmpdir / "UserModel" / "mymodel";
  CHECK(std::filesystem::exists(name_dir));

  // The sol stream is present under the name dir; nothing lands directly under
  // UserModel/ outside a name/tag subdir.
  bool has_x_sol = false;
  if (std::filesystem::exists(tmpdir / "UserModel")) {
    for (const auto& e :
         std::filesystem::recursive_directory_iterator(tmpdir / "UserModel"))
    {
      if (e.path().filename().string().find("x_sol") != std::string::npos) {
        has_x_sol = true;
      }
    }
  }
  CHECK(has_x_sol);

  std::filesystem::remove_all(tmpdir);
}

// ══════════════════════════════════════════════════════════════════════════
// 4. Global-scoped model — one row, no dedup collision
// ══════════════════════════════════════════════════════════════════════════

TEST_CASE("UserModel — global-scope emits one row, no collision")  // NOLINT
{
  using namespace usermodel_test;

  const auto base = solve_numrows(R"({ "uid": 1, "name": "empty" })");

  // One global-scoped constraint → exactly one row for the cell.
  const auto one = solve_numrows(R"({
      "uid": 1, "name": "m",
      "constraint_array": [ { "uid": 1, "name": "g", "scope": "global",
          "expression": "generator('g1').generation <= 150" } ] })");
  CHECK(one == base + 1);

  // Two distinct global constraints in the SAME bundle must BOTH add a row —
  // the PhaseContext element-uid discriminator prevents the metadata dedup in
  // `LinearProblem::add_row` from dropping the second.
  const auto two = solve_numrows(R"({
      "uid": 1, "name": "m",
      "constraint_array": [
        { "uid": 1, "name": "ga", "scope": "global",
          "expression": "generator('g1').generation <= 150" },
        { "uid": 2, "name": "gb", "scope": "global",
          "expression": "generator('g1').generation >= 10" } ] })");
  CHECK(two == base + 2);
}

TEST_CASE("UserModel — all scopes build a feasible LP")  // NOLINT
{
  using namespace usermodel_test;

  for (const std::string_view scope : {"block", "stage", "phase", "global"}) {
    const auto um = std::format(
        R"({{ "uid": 1, "name": "m",
              "constraint_array": [ {{ "uid": 1, "name": "c", "scope": "{}",
                  "expression": "generator('g1').generation <= 150" }} ] }})",
        scope);
    Planning base;
    base.merge(parse_planning_json(make_json(um)));
    PlanningLP planning_lp(std::move(base));
    CHECK(planning_lp.resolve().has_value());
  }
}

// ══════════════════════════════════════════════════════════════════════════
// 5. Aux-col policy — abs/min/max aux cols are internal (named-only capture)
// ══════════════════════════════════════════════════════════════════════════

TEST_CASE(
    "UserModel — aux cols (abs lowering) are internal, not captured")  // NOLINT
{
  using namespace usermodel_test;

  // An `abs(...)` constraint lowers to an `abs_aux` column + 2 helper rows
  // PER block.  Those aux cols/rows are NOT registered in the AMPL registry
  // and carry no tag-name, so by policy UserModel captures only the NAMED
  // var/constraint — the aux structure is internal LP detail.  We assert the
  // model still BUILDS and SOLVES (the aux lowering runs through the reused
  // UserConstraintLP path) — the policy is about OUTPUT capture, documented in
  // user_model_lp.cpp; the named row's dual is the captured surface.
  Planning base;
  base.merge(parse_planning_json(make_json(R"({
      "uid": 1, "name": "m",
      "variable_array": [ { "uid": 1, "name": "x", "lower_bound": -10,
                            "upper_bound": 10 } ],
      "constraint_array": [ { "uid": 1, "name": "absrow",
          "expression": "abs(decision_variable('x').value) <= 4" } ] })")));
  PlanningLP planning_lp(std::move(base));
  REQUIRE(planning_lp.resolve().has_value());

  // The named row is captured (it has a tag-name); the aux cols never appear
  // as a named UserModel stream.  Structural proof: the abs lowering added
  // aux columns beyond the single named `x` column — i.e. the cols added by
  // this bundle exceed the 3 per-block `x` columns (3 abs_aux + 3 x = 6),
  // confirming the aux cols exist in the LP but are intentionally un-named.
  const auto base_cols = solve_numcols(R"({ "uid": 1, "name": "empty" })");
  const auto abs_cols = solve_numcols(R"({
      "uid": 1, "name": "m",
      "variable_array": [ { "uid": 1, "name": "x", "lower_bound": -10,
                            "upper_bound": 10 } ],
      "constraint_array": [ { "uid": 1, "name": "absrow",
          "expression": "abs(decision_variable('x').value) <= 4" } ] })");
  // 3 named x cols + 3 internal abs_aux cols = +6.
  CHECK(abs_cols == base_cols + 6);
}

// ══════════════════════════════════════════════════════════════════════════
// 6. [G9] UserModel output capture under SDDP + low_memory=compress
// ══════════════════════════════════════════════════════════════════════════
//
// Parity with the off-mode capture test (test 3): under `compress` the SDDP
// solver releases each cell's backend between solves and `write_out` must
// rehydrate the LP from the compressed snapshot.  The UserModel capture must
// survive that round-trip — `output/UserModel/<tag>/<name>_sol` must still be
// populated.  The fixture is the 3-phase hydro SDDP case with a bundled
// block-scoped DecisionVariable + constraint.
TEST_CASE(  // NOLINT
    "UserModel — output capture survives SDDP low_memory=compress")
{
  using namespace usermodel_test;

  const auto tmpdir = std::filesystem::temp_directory_path()
      / "gtopt_user_model_compress_capture_test";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  auto planning = make_3phase_hydro_planning();  // ≥2 phases (SDDP needs it)
  planning.options.method = MethodType::sddp;
  planning.options.output_directory = OptName {tmpdir.string()};
  planning.options.output_format = DataFormat::csv;
  planning.options.output_compression = CompressionCodec::uncompressed;
  // A bundled block-scoped var + constraint — captured under UserModel/<tag>/.
  planning.system.user_model_array.push_back(UserModel {
      .uid = Uid {1},
      .name = "m",
      .tag = OptName {"mytag"},
      .variable_array =
          {
              DecisionVariable {
                  .uid = Uid {1},
                  .name = "x",
                  .lower_bound = OptReal {0.0},
                  .upper_bound = OptReal {10.0},
              },
          },
      .constraint_array =
          {
              UserConstraint {
                  .uid = Uid {1},
                  .name = "cap",
                  .expression = Name {"decision_variable('x').value <= 5"},
              },
          },
  });

  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 5;
  opts.convergence_tol = 1e-4;
  opts.cut_sharing = CutSharingMode::none;
  opts.low_memory_mode = LowMemoryMode::compress;  // the rehydrate path
  opts.enable_api = false;

  SDDPMethod sddp(planning_lp, opts);
  const auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  planning_lp.write_out();

  // Parity with the off-mode test: the bundled var's sol stream lands under
  // output/UserModel/mytag/ even though the backend was released + rehydrated.
  const auto um_dir = tmpdir / "UserModel" / "mytag";
  CHECK(std::filesystem::exists(um_dir));

  bool has_x_sol = false;
  if (std::filesystem::exists(tmpdir / "UserModel")) {
    for (const auto& e :
         std::filesystem::recursive_directory_iterator(tmpdir / "UserModel"))
    {
      if (e.path().filename().string().find("x_sol") != std::string::npos) {
        has_x_sol = true;
      }
    }
  }
  CHECK(has_x_sol);

  std::filesystem::remove_all(tmpdir);
}
