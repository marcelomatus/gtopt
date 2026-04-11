/**
 * @file      test_cascade_method.hpp
 * @brief     Unit tests for the multi-level CascadePlanningMethod
 * @date      2026-03-22
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/cascade_method.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_options_lp.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ─── Data structure tests ───────────────────────────────────────────────────

TEST_CASE("ModelOptions defaults are all nullopt")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const ModelOptions opts;
  CHECK(!opts.use_single_bus.has_value());
  CHECK(!opts.use_kirchhoff.has_value());
  CHECK(!opts.use_line_losses.has_value());
  CHECK(!opts.kirchhoff_threshold.has_value());
  CHECK(!opts.loss_segments.has_value());
  CHECK(!opts.scale_objective.has_value());
  CHECK(!opts.scale_theta.has_value());
  CHECK(!opts.demand_fail_cost.has_value());
  CHECK(!opts.reserve_fail_cost.has_value());
}

TEST_CASE("CascadeTransition defaults are all nullopt")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const CascadeTransition trans;
  CHECK(!trans.inherit_optimality_cuts.has_value());
  CHECK(!trans.inherit_feasibility_cuts.has_value());
  CHECK(!trans.inherit_targets.has_value());
  CHECK(!trans.target_rtol.has_value());
  CHECK(!trans.target_min_atol.has_value());
  CHECK(!trans.target_penalty.has_value());
  CHECK(!trans.optimality_dual_threshold.has_value());
}

TEST_CASE("CascadeLevelMethod defaults are all nullopt")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const CascadeLevelMethod solver;
  CHECK(!solver.max_iterations.has_value());
  CHECK(!solver.apertures.has_value());
  CHECK(!solver.convergence_tol.has_value());
}

TEST_CASE("CascadeLevel defaults")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const CascadeLevel level;
  CHECK(!level.name.has_value());
  CHECK(!level.model_options.has_value());
  CHECK(!level.sddp_options.has_value());
  CHECK(!level.transition.has_value());
}

TEST_CASE("CascadeOptions empty level_array by default")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const CascadeOptions opts;
  CHECK(opts.level_array.empty());
  CHECK(!opts.sddp_options.max_iterations.has_value());
  CHECK(!opts.sddp_options.convergence_tol.has_value());
}

TEST_CASE("StateTarget default initialization")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const StateTarget t;
  CHECK(t.class_name.empty());
  CHECK(t.col_name.empty());
  CHECK(t.uid == Uid {unknown_uid});
  CHECK(t.target_value == 0.0);
  CHECK(t.var_scale == 1.0);
  CHECK(std::holds_alternative<std::monostate>(t.context));
}

TEST_CASE("StateTarget with structured fields")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto ctx = make_stage_context(ScenarioUid {0}, StageUid {3});
  const StateTarget t {
      .class_name = "Reservoir",
      .col_name = "efin",
      .uid = Uid {42},
      .context = ctx,
      .scene_index = first_scene_index(),
      .phase_index = PhaseIndex {1},
      .target_value = 100.5,
      .var_scale = 1000.0,
  };

  CHECK(t.class_name == "Reservoir");
  CHECK(t.col_name == "efin");
  CHECK(t.uid == Uid {42});
  CHECK(t.target_value == doctest::Approx(100.5));
  CHECK(t.var_scale == doctest::Approx(1000.0));
  CHECK(t.scene_index == first_scene_index());
  CHECK(t.phase_index == PhaseIndex {1});
  CHECK(std::holds_alternative<StageContext>(t.context));
  const auto& stg = std::get<StageContext>(t.context);
  CHECK(std::get<0>(stg) == ScenarioUid {0});
  CHECK(std::get<1>(stg) == StageUid {3});
}

TEST_CASE(  // NOLINT
    "StateTarget matching by identity — different contexts still match")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Two targets for the same physical variable but from different
  // cascade levels (different stage UIDs in context).
  const StateTarget source {
      .class_name = "Reservoir",
      .col_name = "efin",
      .uid = Uid {5},
      .context = make_stage_context(ScenarioUid {0}, StageUid {2}),
      .target_value = 50.0,
  };

  // Simulate matching logic from add_elastic_targets:
  // match by (class_name, col_name, uid), ignore context differences.
  struct MockSVar
  {
    std::string_view class_name;
    std::string_view col_name;
    Uid uid;
    ColIndex col;
  };

  // Next level has the same reservoir but at a different stage
  const std::vector<MockSVar> next_level_vars = {
      {"Generator", "pgen", Uid {1}, ColIndex {0}},
      {"Reservoir", "efin", Uid {5}, ColIndex {7}},
      {"Battery", "efin", Uid {3}, ColIndex {12}},
  };

  ColIndex matched {unknown_index};
  for (const auto& sv : next_level_vars) {
    if (sv.class_name == source.class_name && sv.col_name == source.col_name
        && sv.uid == source.uid)
    {
      matched = sv.col;
      break;
    }
  }

  CHECK(matched == ColIndex {7});
}

TEST_CASE(  // NOLINT
    "StateTarget matching — no match when uid differs")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const StateTarget source {
      .class_name = "Reservoir",
      .col_name = "efin",
      .uid = Uid {99},
      .target_value = 50.0,
  };

  struct MockSVar
  {
    std::string_view class_name;
    std::string_view col_name;
    Uid uid;
  };

  const std::vector<MockSVar> next_level_vars = {
      {"Reservoir", "efin", Uid {1}},
      {"Reservoir", "efin", Uid {5}},
  };

  bool found = false;
  for (const auto& sv : next_level_vars) {
    if (sv.class_name == source.class_name && sv.col_name == source.col_name
        && sv.uid == source.uid)
    {
      found = true;
      break;
    }
  }

  CHECK_FALSE(found);
}

TEST_CASE("MethodType::cascade enum")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("cascade value is 2")
  {
    CHECK(static_cast<int>(MethodType::cascade) == 2);
  }

  SUBCASE("enum_from_name recognizes cascade")
  {
    CHECK(enum_from_name<MethodType>("cascade") == MethodType::cascade);
  }
}

// ─── PlanningOptionsLP accessor tests
// ───────────────────────────────────────────────

TEST_CASE("PlanningOptionsLP cascade_levels empty by default")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const PlanningOptionsLP options_lp;
  CHECK(!options_lp.has_cascade_levels());
  CHECK(options_lp.cascade_levels().empty());
}

// ─── JSON parsing tests ────────────────────────────────────────────────────

TEST_CASE("JSON parsing of cascade method")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  constexpr std::string_view json_str = R"json(
  {
    "options": {
      "method": "cascade"
    }
  }
  )json";

  const auto planning =
      daw::json::from_json<Planning>(json_str);  // NOLINT(misc-include-cleaner)
  const PlanningOptionsLP options_lp(planning.options);
  CHECK(options_lp.method_type_enum() == MethodType::cascade);
}

TEST_CASE("JSON parsing of cascade levels")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  constexpr std::string_view json_str = R"json(
  {
    "options": {
      "method": "cascade",
      "cascade_options": {
        "sddp_options": {
          "max_iterations": 50,
          "convergence_tol": 0.005
        },
        "level_array": [
          {
            "name": "fast_benders",
            "model_options": {
              "use_single_bus": true,
              "use_kirchhoff": false
            },
            "sddp_options": {
              "max_iterations": 10,
              "apertures": [],
              "convergence_tol": 0.01
            }
          },
          {
            "name": "full_sddp",
            "model_options": {
              "use_kirchhoff": true,
              "use_line_losses": true
            },
            "sddp_options": {
              "max_iterations": 30,
              "apertures": [1, 2, 3, 4, 5]
            },
            "transition": {
              "inherit_optimality_cuts": -1,
              "inherit_feasibility_cuts": 0,
              "inherit_targets": -1,
              "target_rtol": 0.08,
              "target_min_atol": 2.0,
              "target_penalty": 1000.0,
              "optimality_dual_threshold": 0.001
            }
          }
        ]
      }
    }
  }
  )json";

  const auto planning =
      daw::json::from_json<Planning>(json_str);  // NOLINT(misc-include-cleaner)
  const PlanningOptionsLP options_lp(planning.options);

  REQUIRE(options_lp.has_cascade_levels());
  const auto& levels = options_lp.cascade_levels();
  REQUIRE(levels.size() == 2);

  SUBCASE("cascade global options parsed")
  {
    const auto& csddp = options_lp.cascade_sddp_options();
    CHECK(csddp.max_iterations.value_or(0) == 50);
    CHECK(csddp.convergence_tol.value_or(0.0) == doctest::Approx(0.005));
  }

  SUBCASE("level 0 parsed correctly")
  {
    CHECK(levels[0].name.value_or("") == "fast_benders");
    CHECK(levels[0].model_options.has_value());
    const auto m0 = levels[0].model_options.value_or(ModelOptions {});
    CHECK(m0.use_single_bus.value_or(false) == true);
    CHECK(m0.use_kirchhoff.value_or(true) == false);
    CHECK(levels[0].sddp_options.has_value());
    const auto s0 = levels[0].sddp_options.value_or(CascadeLevelMethod {});
    CHECK(s0.max_iterations.value_or(0) == 10);
    CHECK((s0.apertures && s0.apertures->empty()));
  }

  SUBCASE("level 1 transition parsed correctly")
  {
    CHECK(levels[1].transition.has_value());
    const auto trans = levels[1].transition.value_or(CascadeTransition {});
    CHECK(trans.inherit_optimality_cuts.value_or(0) == -1);
    CHECK(trans.inherit_feasibility_cuts.value_or(-1) == 0);
    CHECK(trans.inherit_targets.value_or(0) == -1);
    CHECK(trans.target_rtol.value_or(0.0) == doctest::Approx(0.08));
    CHECK(trans.target_min_atol.value_or(0.0) == doctest::Approx(2.0));
    CHECK(trans.target_penalty.value_or(0.0) == doctest::Approx(1000.0));
    CHECK(trans.optimality_dual_threshold.value_or(0.0)
          == doctest::Approx(0.001));
  }
}

TEST_CASE("JSON cascade options with empty levels uses defaults")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  constexpr std::string_view json_str = R"json(
  {
    "options": {
      "method": "cascade"
    }
  }
  )json";

  const auto planning =
      daw::json::from_json<Planning>(json_str);  // NOLINT(misc-include-cleaner)
  const PlanningOptionsLP options_lp(planning.options);
  CHECK(!options_lp.has_cascade_levels());
}

// ─── Factory tests ──────────────────────────────────────────────────────────

TEST_CASE("make_planning_method factory - cascade")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  PlanningOptions opts;
  opts.method = MethodType::cascade;
  opts.sddp_options.max_iterations = OptInt {20};

  const PlanningOptionsLP options_lp(std::move(opts));
  auto solver = make_planning_method(options_lp);
  REQUIRE(solver != nullptr);
}

TEST_CASE("make_planning_method factory - cascade single phase falls back")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  PlanningOptions opts;
  opts.method = MethodType::cascade;
  const PlanningOptionsLP options_lp(std::move(opts));

  auto solver = make_planning_method(options_lp, 1);
  REQUIRE(solver != nullptr);
}

// ─── Solver integration tests ───────────────────────────────────────────────

TEST_CASE("CascadePlanningMethod basic 3-phase hydro")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};  // no apertures

  // Use a simple 2-level cascade for the test
  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"benders"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .apertures = Array<Uid> {},  // no apertures
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"sddp"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .apertures = Array<Uid> {},  // no apertures
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_targets = OptInt {-1},
                  .target_rtol = OptReal {0.05},
                  .target_min_atol = OptReal {1.0},
                  .target_penalty = OptReal {500.0},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));

  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  SUBCASE("solve succeeds")
  {
    REQUIRE(result.has_value());
  }

  SUBCASE("has iteration results")
  {
    CHECK(!solver.all_results().empty());
  }

  SUBCASE("iteration count within budget")
  {
    // 2 levels: (5 iters + 1 final fwd) + (10 iters + 1 final fwd) = 17
    CHECK(solver.all_results().size() <= 17);
  }
}

TEST_CASE("CascadePlanningMethod with empty options = single level")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Empty CascadeOptions should be equivalent to one level with
  // the base SDDP solver and default model options.
  auto planning1 = make_3phase_hydro_planning();
  PlanningLP planning_lp1(std::move(planning1));

  SDDPOptions sddp_opts1;
  sddp_opts1.max_iterations = 10;
  sddp_opts1.convergence_tol = 0.01;
  sddp_opts1.apertures = std::vector<Uid> {};

  // Direct SDDP
  SDDPMethod direct_solver(planning_lp1, sddp_opts1);
  const SolverOptions lp_opts;
  auto direct_result = direct_solver.solve(lp_opts);

  // Empty cascade
  auto planning2 = make_3phase_hydro_planning();
  PlanningLP planning_lp2(std::move(planning2));

  SDDPOptions sddp_opts2;
  sddp_opts2.max_iterations = 10;
  sddp_opts2.convergence_tol = 0.01;
  sddp_opts2.apertures = std::vector<Uid> {};

  CascadePlanningMethod cascade_solver(std::move(sddp_opts2),
                                       CascadeOptions {});
  auto cascade_result = cascade_solver.solve(planning_lp2, lp_opts);

  REQUIRE(direct_result.has_value());
  REQUIRE(cascade_result.has_value());
  CHECK(direct_result->size() == cascade_solver.all_results().size());
}

TEST_CASE("CascadePlanningMethod 5-phase reservoir")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_5phase_reservoir_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 25;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};  // no apertures

  // Simple 2-level for the test
  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"benders"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {8},
                  .apertures = Array<Uid> {},  // no apertures
              },
      },
      CascadeLevel {
          .name = OptName {"sddp"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {15},
                  .apertures = Array<Uid> {},  // no apertures
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
                  .inherit_targets = OptInt {-1},
                  .target_penalty = OptReal {500.0},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));

  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  SUBCASE("solve succeeds")
  {
    REQUIRE(result.has_value());
  }

  SUBCASE("has results")
  {
    CHECK(solver.all_results().size() > 1);
  }
}

// ─── CascadeLevelMethod merge tests ────────────────────────────────────────

TEST_CASE("CascadeLevelMethod merge overwrites set fields only")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CascadeLevelMethod base;
  base.max_iterations = OptInt {10};
  base.convergence_tol = OptReal {0.01};

  SUBCASE("merge overwrites max_iterations when set")
  {
    CascadeLevelMethod override_opts;
    override_opts.max_iterations = OptInt {20};
    base.merge(override_opts);
    CHECK(base.max_iterations.value_or(0) == 20);
    CHECK(base.convergence_tol.value_or(0.0) == doctest::Approx(0.01));
    CHECK(!base.apertures.has_value());
  }

  SUBCASE("merge does not overwrite unset fields")
  {
    const CascadeLevelMethod empty;
    base.merge(empty);
    CHECK(base.max_iterations.value_or(0) == 10);
    CHECK(base.convergence_tol.value_or(0.0) == doctest::Approx(0.01));
  }

  SUBCASE("merge overwrites apertures when set")
  {
    CascadeLevelMethod override_opts;
    override_opts.apertures = Array<Uid> {
        1,
        2,
        3,
    };
    base.merge(override_opts);
    REQUIRE(base.apertures.has_value());
    CHECK(base.apertures->size() == 3);
  }

  SUBCASE("merge with empty apertures replaces nullopt")
  {
    CascadeLevelMethod override_opts;
    override_opts.apertures = Array<Uid> {};
    base.merge(override_opts);
    REQUIRE(base.apertures.has_value());
    CHECK(base.apertures->empty());
  }

  SUBCASE("merge does not clear apertures when override is nullopt")
  {
    base.apertures = Array<Uid> {
        5,
        6,
    };
    const CascadeLevelMethod empty;
    base.merge(empty);
    REQUIRE(base.apertures.has_value());
    CHECK(base.apertures->size() == 2);
  }
}

// ─── CascadeTransition merge tests ─────────────────────────────────────────

TEST_CASE("CascadeTransition merge overwrites set fields only")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CascadeTransition base;
  base.inherit_targets = OptInt {-1};
  base.target_penalty = OptReal {500.0};

  CascadeTransition override_opts;
  override_opts.target_penalty = OptReal {1000.0};
  override_opts.inherit_optimality_cuts = OptInt {-1};

  base.merge(override_opts);
  CHECK(base.inherit_targets.value_or(0) == -1);
  CHECK(base.target_penalty.value_or(0.0) == doctest::Approx(1000.0));
  CHECK(base.inherit_optimality_cuts.value_or(0) == -1);
  CHECK(!base.target_rtol.has_value());
}

// ─── CascadeOptions merge tests ────────────────────────────────────────────

TEST_CASE("CascadeOptions merge overwrites global fields and level_array")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  CascadeOptions base;
  base.sddp_options.max_iterations = OptInt {50};
  base.sddp_options.convergence_tol = OptReal {0.01};

  SUBCASE("merge overwrites max_iterations via sddp_options")
  {
    CascadeOptions override_opts;
    override_opts.sddp_options.max_iterations = OptInt {100};
    base.merge(std::move(override_opts));
    CHECK(base.sddp_options.max_iterations.value_or(0) == 100);
    CHECK(base.sddp_options.convergence_tol.value_or(0.0)
          == doctest::Approx(0.01));
  }

  SUBCASE("merge replaces level_array when non-empty")
  {
    CascadeOptions override_opts;
    override_opts.level_array = {
        CascadeLevel {
            .name = OptName {"new_level"},
        },
    };
    base.merge(std::move(override_opts));
    REQUIRE(base.level_array.size() == 1);
    CHECK(base.level_array[0].name.value_or("") == "new_level");
  }

  SUBCASE("merge does not replace level_array when empty")
  {
    base.level_array = {
        CascadeLevel {
            .name = OptName {"existing"},
        },
    };
    CascadeOptions empty;
    base.merge(std::move(empty));
    REQUIRE(base.level_array.size() == 1);
    CHECK(base.level_array[0].name.value_or("") == "existing");
  }
}

// ─── ModelOptions merge tests ──────────────────────────────────────────────

TEST_CASE("ModelOptions merge overwrites set fields only")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  ModelOptions base;
  base.use_single_bus = OptBool {true};
  base.demand_fail_cost = 1000.0;

  ModelOptions override_opts;
  override_opts.use_kirchhoff = OptBool {true};
  override_opts.demand_fail_cost = 2000.0;

  base.merge(override_opts);
  CHECK(base.use_single_bus.value_or(false) == true);
  CHECK(base.use_kirchhoff.value_or(false) == true);
  CHECK(base.demand_fail_cost.value_or(0.0) == doctest::Approx(2000.0));
  CHECK(!base.use_line_losses.has_value());
}

// ─── SDDP option resolution: base → cascade global → per-level ────────────

TEST_CASE("CascadePlanningMethod SDDP option priority chain")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // This test verifies the 3-layer priority:
  // base SDDPOptions → cascade global → per-level CascadeLevelMethod.
  // Since build_level_sddp_opts is private, we test through solve()
  // and verify the solver's effective options via all_results().

  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SUBCASE("cascade global overrides base max_iterations")
  {
    SDDPOptions base;
    base.max_iterations = 100;
    base.convergence_tol = 0.01;
    base.apertures = std::vector<Uid> {};

    CascadeOptions cascade;
    cascade.sddp_options.max_iterations = OptInt {3};  // global budget
    cascade.level_array = {
        CascadeLevel {
            .name = OptName {"only_level"},
            .model_options =
                ModelOptions {
                    .use_single_bus = OptBool {true},
                },
            .sddp_options =
                CascadeLevelMethod {
                    .apertures = Array<Uid> {},
                },
        },
    };

    CascadePlanningMethod solver(std::move(base), std::move(cascade));
    const SolverOptions lp_opts;
    auto result = solver.solve(planning_lp, lp_opts);
    REQUIRE(result.has_value());
    // Cascade global max_iterations=3 + 1 final forward pass per level
    CHECK(solver.all_results().size() <= 4);
  }

  SUBCASE("per-level overrides cascade global max_iterations")
  {
    SDDPOptions base;
    base.max_iterations = 100;
    base.convergence_tol = 0.01;
    base.apertures = std::vector<Uid> {};

    CascadeOptions cascade;
    cascade.sddp_options.max_iterations = OptInt {50};  // global budget
    cascade.level_array = {
        CascadeLevel {
            .name = OptName {"limited_level"},
            .model_options =
                ModelOptions {
                    .use_single_bus = OptBool {true},
                },
            .sddp_options =
                CascadeLevelMethod {
                    .max_iterations = OptInt {2},  // per-level override
                    .apertures = Array<Uid> {},
                },
        },
    };

    CascadePlanningMethod solver(std::move(base), std::move(cascade));
    const SolverOptions lp_opts;
    auto result = solver.solve(planning_lp, lp_opts);
    REQUIRE(result.has_value());
    // Per-level max_iterations=2 + 1 final forward pass per level
    CHECK(solver.all_results().size() <= 3);
  }

  SUBCASE("base convergence_tol used when no overrides")
  {
    SDDPOptions base;
    base.max_iterations = 20;
    base.convergence_tol = 0.5;  // very loose → should converge quickly
    base.apertures = std::vector<Uid> {};

    CascadeOptions cascade;
    cascade.level_array = {
        CascadeLevel {
            .name = OptName {"loose_tol"},
            .model_options =
                ModelOptions {
                    .use_single_bus = OptBool {true},
                },
            .sddp_options =
                CascadeLevelMethod {
                    .apertures = Array<Uid> {},
                },
        },
    };

    CascadePlanningMethod solver(std::move(base), std::move(cascade));
    const SolverOptions lp_opts;
    auto result = solver.solve(planning_lp, lp_opts);
    REQUIRE(result.has_value());
    // Loose tolerance should converge in fewer iterations than max
    CHECK(solver.all_results().size() < 20);
  }
}

// ─── Empty cascade_opts uses default 4 levels ──────────────────────────────

TEST_CASE("CascadePlanningMethod empty CascadeOptions = single level")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Empty cascade options should behave as a single level with
  // the base SDDP solver and default model options.
  auto planning1 = make_3phase_hydro_planning();
  PlanningLP planning_lp1(std::move(planning1));

  SDDPOptions sddp_opts1;
  sddp_opts1.max_iterations = 5;
  sddp_opts1.convergence_tol = 0.01;
  sddp_opts1.apertures = std::vector<Uid> {};

  SDDPMethod direct_solver(planning_lp1, sddp_opts1);
  const SolverOptions lp_opts;
  auto direct_result = direct_solver.solve(lp_opts);

  auto planning2 = make_3phase_hydro_planning();
  PlanningLP planning_lp2(std::move(planning2));

  SDDPOptions sddp_opts2;
  sddp_opts2.max_iterations = 5;
  sddp_opts2.convergence_tol = 0.01;
  sddp_opts2.apertures = std::vector<Uid> {};

  CascadePlanningMethod cascade_solver(std::move(sddp_opts2),
                                       CascadeOptions {});
  auto cascade_result = cascade_solver.solve(planning_lp2, lp_opts);

  REQUIRE(direct_result.has_value());
  REQUIRE(cascade_result.has_value());
  CHECK(direct_result->size() == cascade_solver.all_results().size());
}

}  // anonymous namespace
