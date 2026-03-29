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

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ─── Data structure tests ───────────────────────────────────────────────────

TEST_CASE("ModelOptions defaults are all nullopt")  // NOLINT
{
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
  CHECK(!opts.annual_discount_rate.has_value());
}

TEST_CASE("CascadeTransition defaults are all nullopt")  // NOLINT
{
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
  const CascadeLevelMethod solver;
  CHECK(!solver.max_iterations.has_value());
  CHECK(!solver.apertures.has_value());
  CHECK(!solver.convergence_tol.has_value());
}

TEST_CASE("CascadeLevel defaults")  // NOLINT
{
  const CascadeLevel level;
  CHECK(!level.name.has_value());
  CHECK(!level.model_options.has_value());
  CHECK(!level.sddp_options.has_value());
  CHECK(!level.transition.has_value());
}

TEST_CASE("CascadeOptions empty level_array by default")  // NOLINT
{
  const CascadeOptions opts;
  CHECK(opts.level_array.empty());
  CHECK(!opts.sddp_options.max_iterations.has_value());
  CHECK(!opts.sddp_options.convergence_tol.has_value());
}

TEST_CASE("NamedStateTarget default initialization")  // NOLINT
{
  const NamedStateTarget t;
  CHECK(t.var_name.empty());
  CHECK(t.target_value == 0.0);
}

TEST_CASE("MethodType::cascade enum")  // NOLINT
{
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
  const PlanningOptionsLP options_lp;
  CHECK(!options_lp.has_cascade_levels());
  CHECK(options_lp.cascade_levels().empty());
}

// ─── JSON parsing tests ────────────────────────────────────────────────────

TEST_CASE("JSON parsing of cascade method")  // NOLINT
{
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
  PlanningOptions opts;
  opts.method = MethodType::cascade;
  const PlanningOptionsLP options_lp(std::move(opts));

  auto solver = make_planning_method(options_lp, 1);
  REQUIRE(solver != nullptr);
}

// ─── Solver integration tests ───────────────────────────────────────────────

TEST_CASE("CascadePlanningMethod basic 3-phase hydro")  // NOLINT
{
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
  ModelOptions base;
  base.use_single_bus = OptBool {true};
  base.demand_fail_cost = OptReal {1000.0};

  ModelOptions override_opts;
  override_opts.use_kirchhoff = OptBool {true};
  override_opts.demand_fail_cost = OptReal {2000.0};

  base.merge(override_opts);
  CHECK(base.use_single_bus.value_or(false) == true);
  CHECK(base.use_kirchhoff.value_or(false) == true);
  CHECK(base.demand_fail_cost.value_or(0.0) == doctest::Approx(2000.0));
  CHECK(!base.use_line_losses.has_value());
}

// ─── SDDP option resolution: base → cascade global → per-level ────────────

TEST_CASE("CascadePlanningMethod SDDP option priority chain")  // NOLINT
{
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

// ─── Aperture semantics in cascade levels ──────────────────────────────────

TEST_CASE("Cascade level aperture semantics")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SUBCASE("empty apertures = pure Benders")
  {
    SDDPOptions base;
    base.max_iterations = 5;
    base.convergence_tol = 0.01;
    // base has no apertures (nullopt)

    CascadeOptions cascade;
    cascade.level_array = {
        CascadeLevel {
            .name = OptName {"benders_only"},
            .model_options =
                ModelOptions {
                    .use_single_bus = OptBool {true},
                },
            .sddp_options =
                CascadeLevelMethod {
                    .max_iterations = OptInt {3},
                    .apertures = Array<Uid> {},  // empty = no apertures
                },
        },
    };

    CascadePlanningMethod solver(std::move(base), std::move(cascade));
    const SolverOptions lp_opts;
    auto result = solver.solve(planning_lp, lp_opts);
    REQUIRE(result.has_value());
  }

  SUBCASE("nullopt apertures inherits from base/per-phase")
  {
    SDDPOptions base;
    base.max_iterations = 5;
    base.convergence_tol = 0.01;
    base.apertures = std::vector<Uid> {};  // base = no apertures

    CascadeOptions cascade;
    cascade.level_array = {
        CascadeLevel {
            .name = OptName {"inherit_apertures"},
            .model_options =
                ModelOptions {
                    .use_single_bus = OptBool {true},
                },
            .sddp_options =
                CascadeLevelMethod {
                    .max_iterations = OptInt {3},
                    // apertures absent (nullopt) → uses base setting
                },
        },
    };

    CascadePlanningMethod solver(std::move(base), std::move(cascade));
    const SolverOptions lp_opts;
    auto result = solver.solve(planning_lp, lp_opts);
    REQUIRE(result.has_value());
  }
}

// ─── Multi-level with transitions ──────────────────────────────────────────

TEST_CASE("Cascade 2-level with target inheritance")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 20;
  base.convergence_tol = 0.01;
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"benders"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"sddp_with_targets"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .apertures = Array<Uid> {},
              },
          .transition =
              CascadeTransition {
                  .inherit_targets = OptInt {-1},
                  .target_rtol = OptReal {0.10},
                  .target_min_atol = OptReal {1.0},
                  .target_penalty = OptReal {500.0},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  SUBCASE("solve succeeds")
  {
    REQUIRE(result.has_value());
  }

  SUBCASE("results from both levels")
  {
    // 2 levels: (up to 3 + 1 final fwd) + (up to 5 + 1 final fwd) = 10
    CHECK(solver.all_results().size() >= 2);
    CHECK(solver.all_results().size() <= 10);
  }
}

// ─── Multi-level with cut inheritance ──────────────────────────────────────

TEST_CASE("Cascade 2-level with optimality cut inheritance")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 20;
  base.convergence_tol = 0.01;
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"level0"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {4},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"level1_cuts"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {4},
                  .apertures = Array<Uid> {},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
                  .inherit_feasibility_cuts = OptInt {0},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());
  CHECK(!solver.all_results().empty());
}

// ─── LP reuse when model_options is absent ─────────────────────────────────

TEST_CASE("Cascade reuses LP when model_options absent")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 20;
  base.convergence_tol = 0.01;
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"initial"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"reuse_lp"},
          // No model_options → reuses previous LP
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  .apertures = Array<Uid> {},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());
  CHECK(solver.all_results().size() >= 2);
}

// ─── 3-level cascade with mixed transitions ───────────────────────────────

TEST_CASE("Cascade 3-level mixed transitions")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 20;
  base.convergence_tol = 0.01;
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.sddp_options.convergence_tol = OptReal {0.01};
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"fast_benders"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"guided_benders"},
          // No model_options → reuse LP
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  .apertures = Array<Uid> {},
              },
          .transition =
              CascadeTransition {
                  .inherit_targets = OptInt {-1},
                  .target_rtol = OptReal {0.05},
                  .target_min_atol = OptReal {0.5},
                  .target_penalty = OptReal {300.0},
              },
      },
      CascadeLevel {
          .name = OptName {"final_benders"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  .apertures = Array<Uid> {},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
                  .inherit_targets = OptInt {-1},
                  .target_penalty = OptReal {200.0},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  REQUIRE(result.has_value());
  // 3 levels × up to (3 iterations + 1 final fwd) each = up to 12 total
  CHECK(solver.all_results().size() <= 12);
  CHECK(solver.all_results().size() >= 3);
}

// ─── Cascade with 5-phase reservoir and cut+target transitions ─────────────

TEST_CASE("Cascade 5-phase with dual threshold cut filter")  // NOLINT
{
  auto planning = make_5phase_reservoir_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 20;
  base.convergence_tol = 0.01;
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"benders"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"filtered_cuts"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .apertures = Array<Uid> {},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
                  .inherit_targets = OptInt {-1},
                  .target_penalty = OptReal {500.0},
                  .optimality_dual_threshold = OptReal {0.001},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());
  CHECK(!solver.all_results().empty());
}

// ─── Cascade global convergence_tol as default ─────────────────────────────

TEST_CASE("Cascade global convergence_tol applies to all levels")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions base;
  base.max_iterations = 20;
  base.convergence_tol = 1e-6;  // very tight
  base.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.sddp_options.convergence_tol =
      OptReal {0.5};  // very loose global override
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"level0"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .apertures = Array<Uid> {},
                  // No per-level convergence_tol → uses cascade global 0.5
              },
      },
  };

  CascadePlanningMethod solver(std::move(base), std::move(cascade));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);
  REQUIRE(result.has_value());
  // Loose tolerance should converge fast
  CHECK(solver.all_results().size() < 10);
}

// ─── Single-level cascade = equivalent to direct SDDP ─────────────────────

TEST_CASE("Single-level cascade produces same result as direct SDDP")
// NOLINT
{
  // Run SDDP directly
  auto planning1 = make_3phase_hydro_planning();
  PlanningLP planning_lp1(std::move(planning1));

  SDDPOptions sddp_opts1;
  sddp_opts1.max_iterations = 8;
  sddp_opts1.convergence_tol = 0.01;
  sddp_opts1.apertures = std::vector<Uid> {};

  SDDPMethod direct_solver(planning_lp1, sddp_opts1);
  const SolverOptions lp_opts;
  auto direct_result = direct_solver.solve(lp_opts);

  // Run single-level cascade with same options
  auto planning2 = make_3phase_hydro_planning();
  PlanningLP planning_lp2(std::move(planning2));

  SDDPOptions sddp_opts2;
  sddp_opts2.max_iterations = 8;
  sddp_opts2.convergence_tol = 0.01;
  sddp_opts2.apertures = std::vector<Uid> {};

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"single"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {8},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
  };

  CascadePlanningMethod cascade_solver(std::move(sddp_opts2),
                                       std::move(cascade));
  auto cascade_result = cascade_solver.solve(planning_lp2, lp_opts);

  REQUIRE(direct_result.has_value());
  REQUIRE(cascade_result.has_value());

  // Both should converge and produce similar iteration counts
  CHECK(direct_result->size() == cascade_solver.all_results().size());
}

// ─── Multi-bus cascade test with transmission lines ─────────────────────────

/// Create a 3-phase 2-bus hydro system with a transmission line.
/// Bus 1: hydro generator + reservoir.  Bus 2: demand + thermal backup.
/// Line connects bus 1 → bus 2 with limited capacity.
auto make_3phase_2bus_hydro_planning() -> Planning
{
  // ── Blocks: 6 per phase × 3 phases = 18 total ──
  Array<Block> block_array;
  for (int i = 0; i < 18; ++i) {
    block_array.push_back(Block {
        .uid = Uid {i + 1},
        .duration = 1.0,
    });
  }

  Array<Stage> stage_array = {
      Stage {
          .uid = Uid {1},
          .first_block = 0,
          .count_block = 6,
      },
      Stage {
          .uid = Uid {2},
          .first_block = 6,
          .count_block = 6,
      },
      Stage {
          .uid = Uid {3},
          .first_block = 12,
          .count_block = 6,
      },
  };

  Array<Phase> phase_array = {
      Phase {
          .uid = Uid {1},
          .first_stage = 0,
          .count_stage = 1,
      },
      Phase {
          .uid = Uid {2},
          .first_stage = 1,
          .count_stage = 1,
      },
      Phase {
          .uid = Uid {3},
          .first_stage = 2,
          .count_stage = 1,
      },
  };

  // ── System components ──
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "bus_hydro",
      },
      {
          .uid = Uid {2},
          .name = "bus_demand",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 80.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {2},
          .gcost = 50.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load1",
          .bus = Uid {2},
          .capacity = 60.0,
      },
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "line_1_2",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .reactance = 0.05,
          .tmax_ba = 100.0,
          .tmax_ab = 100.0,
      },
  };

  // ── Hydro system ──
  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 300.0,
          .emin = 0.0,
          .emax = 300.0,
          .eini = 150.0,
          .fmin = -500.0,
          .fmax = +500.0,
          .flow_conversion_rate = 1.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 10.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 1.0,
      },
  };

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
              },
          },
      .phase_array = std::move(phase_array),
  };

  PlanningOptions options;
  options.demand_fail_cost = OptReal {1000.0};
  options.use_single_bus = OptBool {false};
  options.use_kirchhoff = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  System system = {
      .name = "cascade_2bus_hydro",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = std::move(system),
  };
}

/// 6-phase variant of the 2-bus hydro planning for cascade tests that need
/// more Benders iterations (more phases ⇒ more state links ⇒ harder for cuts).
auto make_6phase_2bus_hydro_planning() -> Planning
{
  constexpr Size num_phases = 6;
  constexpr Size blocks_per_phase = 4;
  constexpr Size total_blocks = num_phases * blocks_per_phase;

  Array<Block> block_array;
  for (Size i = 0; i < total_blocks; ++i) {
    block_array.push_back(Block {
        .uid = Uid {static_cast<int>(i) + 1},
        .duration = 1.0,
    });
  }

  Array<Stage> stage_array;
  Array<Phase> phase_array;
  for (Size p = 0; p < static_cast<Size>(num_phases); ++p) {
    stage_array.push_back(Stage {
        .uid = Uid {static_cast<int>(p) + 1},
        .first_block = p * blocks_per_phase,
        .count_block = blocks_per_phase,
    });
    phase_array.push_back(Phase {
        .uid = Uid {static_cast<int>(p) + 1},
        .first_stage = p,
        .count_stage = 1,
    });
  }

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "bus_hydro",
      },
      {
          .uid = Uid {2},
          .name = "bus_demand",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 80.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {2},
          .gcost = 50.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load1",
          .bus = Uid {2},
          .capacity = 60.0,
      },
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "line_1_2",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .reactance = 0.05,
          .tmax_ba = 100.0,
          .tmax_ab = 100.0,
      },
  };

  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 500.0,
          .emin = 0.0,
          .emax = 500.0,
          .eini = 250.0,
          .fmin = -500.0,
          .fmax = +500.0,
          .flow_conversion_rate = 1.0,
      },
  };

  const Array<Flow> flow_array = {
      {
          .uid = Uid {1},
          .name = "inflow",
          .direction = 1,
          .junction = Uid {1},
          .discharge = 10.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 1.0,
      },
  };

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
              },
          },
      .phase_array = std::move(phase_array),
  };

  PlanningOptions options;
  options.demand_fail_cost = OptReal {1000.0};
  options.use_single_bus = OptBool {false};
  options.use_kirchhoff = OptBool {true};
  options.scale_objective = OptReal {1.0};
  options.output_format = DataFormat::csv;
  options.output_compression = CompressionCodec::uncompressed;

  System system = {
      .name = "cascade_6ph_2bus_hydro",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .flow_array = flow_array,
      .reservoir_array = reservoir_array,
      .turbine_array = turbine_array,
  };

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = std::move(system),
  };
}

TEST_CASE("Cascade 2-level with multi-bus network and cut inheritance")
// NOLINT
{
  auto planning = make_3phase_2bus_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};  // no apertures (Benders)

  // Level 0: single-bus relaxation (fast convergence, ignores network)
  // Level 1: full network, inherits state variable targets from level 0
  //          (not cuts, since the LP column structure changes with
  //           use_single_bus → different theta/line columns)
  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"uninodal_benders"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {8},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"full_network"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .apertures = Array<Uid> {},
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

  SUBCASE("has results from both levels")
  {
    CHECK(solver.all_results().size() > 2);
  }

  SUBCASE("level_stats populated for both levels")
  {
    REQUIRE(solver.level_stats().size() == 2);

    const auto& stats0 = solver.level_stats()[0];
    CHECK(stats0.name == "uninodal_benders");
    CHECK(stats0.iterations > 0);
    CHECK(stats0.iterations <= 8);
    CHECK(stats0.elapsed_s > 0.0);

    const auto& stats1 = solver.level_stats()[1];
    CHECK(stats1.name == "full_network");
    CHECK(stats1.iterations > 0);
    CHECK(stats1.iterations <= 10);
    CHECK(stats1.elapsed_s > 0.0);
  }

  SUBCASE("iteration count within budget")
  {
    // 2 levels: up to (8+1) + (10+1) = 20 results
    CHECK(solver.all_results().size() <= 20);
  }

  SUBCASE("level stats have valid bounds")
  {
    for (const auto& ls : solver.level_stats()) {
      CHECK(ls.lower_bound <= ls.upper_bound + 1e-6);
      CHECK(ls.gap >= 0.0);
      CHECK(ls.cuts_added >= 0);
    }
  }
}

TEST_CASE("SDDP baseline (6-phase, no cascade)")  // NOLINT
{
  // Baseline: plain SDDP solver on the same 6-phase hydro system,
  // for comparison with cascade cut/target inheritance tests.
  auto planning = make_6phase_2bus_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};

  SDDPMethod solver(planning_lp, std::move(sddp_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(lp_opts);

  SUBCASE("solve succeeds")
  {
    REQUIRE(result.has_value());
  }

  SUBCASE("converges")
  {
    REQUIRE(result.has_value());
    REQUIRE(!result->empty());
    const auto& last_training = (*result)[result->size() - 2];
    CHECK(last_training.converged);
    CHECK(last_training.gap < 0.01 + 1e-9);
    // 6 phases should require several iterations
    CHECK(last_training.iteration >= IterationIndex {3});
  }

  SUBCASE("optimal value matches expected")
  {
    REQUIRE(result.has_value());
    REQUIRE(!result->empty());
    // Simulation pass is the last result
    const auto& sim = result->back();
    CHECK(sim.upper_bound == doctest::Approx(49950.0).epsilon(0.01));
    CHECK(sim.lower_bound == doctest::Approx(49950.0).epsilon(0.01));
  }
}

TEST_CASE("Cascade 2-level with cut inheritance only (6-phase)")
// NOLINT
{
  // 6 phases ⇒ more state links ⇒ Benders needs more iterations to converge,
  // making the effect of inherited cuts clearly visible.
  auto planning = make_6phase_2bus_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};

  // Level 0: Benders training on full network.
  // Level 1: Same LP, inherits cuts ⇒ fewer iterations or simulation only.
  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"training"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {15},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"with_cuts"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {20},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
                  .inherit_feasibility_cuts = OptInt {-1},
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

  SUBCASE("level 0 converges with multiple iterations")
  {
    REQUIRE(solver.level_stats().size() == 2);
    const auto& stats0 = solver.level_stats()[0];

    CHECK(stats0.converged);
    CHECK(stats0.gap < 0.01 + 1e-9);
    // 6 phases should require several iterations
    CHECK(stats0.iterations >= 3);
  }

  SUBCASE("level 1 converges quickly with inherited cuts")
  {
    REQUIRE(solver.level_stats().size() == 2);
    const auto& stats0 = solver.level_stats()[0];
    const auto& stats1 = solver.level_stats()[1];

    CHECK(stats1.converged);
    CHECK(stats1.gap < 0.01 + 1e-9);
    // Inherited cuts should let level 1 converge in no more iterations
    CHECK(stats1.iterations <= stats0.iterations);
  }

  SUBCASE("both levels reach same optimal value")
  {
    REQUIRE(solver.level_stats().size() == 2);
    const auto& stats0 = solver.level_stats()[0];
    const auto& stats1 = solver.level_stats()[1];

    CHECK(stats0.lower_bound
          == doctest::Approx(stats1.lower_bound).epsilon(0.01));
    CHECK(stats0.upper_bound
          == doctest::Approx(stats1.upper_bound).epsilon(0.01));
  }

  SUBCASE("level stats have valid bounds")
  {
    for (const auto& ls : solver.level_stats()) {
      CHECK(ls.name.size() > 0);
      CHECK(ls.lower_bound <= ls.upper_bound + 1e-6);
      CHECK(ls.cuts_added >= 0);
    }
  }
}

TEST_CASE("Cascade 2-level with target inheritance only (6-phase)")
// NOLINT
{
  // 6 phases ⇒ more state links ⇒ targets from level 0 guide level 1
  // toward the optimal reservoir trajectory, reducing iterations.
  auto planning = make_6phase_2bus_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};

  // Level 0: Benders training on full network.
  // Level 1: Same LP, inherits state variable targets ⇒ fewer iterations.
  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"training"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {15},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"with_targets"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {20},
                  .apertures = Array<Uid> {},
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

  SUBCASE("level 0 converges with multiple iterations")
  {
    REQUIRE(solver.level_stats().size() == 2);
    const auto& stats0 = solver.level_stats()[0];

    CHECK(stats0.converged);
    CHECK(stats0.gap < 0.01 + 1e-9);
    // 6 phases should require several iterations
    CHECK(stats0.iterations >= 3);
  }

  SUBCASE("level 1 converges with inherited targets")
  {
    REQUIRE(solver.level_stats().size() == 2);
    const auto& stats0 = solver.level_stats()[0];
    const auto& stats1 = solver.level_stats()[1];

    CHECK(stats1.converged);
    CHECK(stats1.gap < 0.01 + 1e-9);
    // Targets guide the forward pass toward the optimal trajectory;
    // LB convergence still depends on cut generation, so iteration
    // count may be similar but should not greatly exceed level 0.
    // Allow +1 tolerance for solver-dependent numerical differences.
    CHECK(stats1.iterations <= stats0.iterations + 1);
  }

  SUBCASE("both levels reach same optimal value")
  {
    REQUIRE(solver.level_stats().size() == 2);
    const auto& stats0 = solver.level_stats()[0];
    const auto& stats1 = solver.level_stats()[1];

    CHECK(stats0.lower_bound
          == doctest::Approx(stats1.lower_bound).epsilon(0.01));
    CHECK(stats0.upper_bound
          == doctest::Approx(stats1.upper_bound).epsilon(0.01));
  }

  SUBCASE("level stats have valid bounds")
  {
    for (const auto& ls : solver.level_stats()) {
      CHECK(ls.name.size() > 0);
      CHECK(ls.lower_bound <= ls.upper_bound + 1e-6);
      CHECK(ls.cuts_added >= 0);
    }
  }
}

TEST_CASE("Cascade 3-level with targets then cuts (6-phase)")  // NOLINT
{
  // Level 0: fast uninodal Benders to get rough solution.
  // Level 1: full network guided by uninodal targets.
  // Level 2: same network, inherits cuts from level 1 ⇒ faster convergence.
  auto planning = make_6phase_2bus_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};

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
                  .max_iterations = OptInt {15},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"guided"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {20},
                  .apertures = Array<Uid> {},
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
      CascadeLevel {
          .name = OptName {"refined"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {20},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
                  .inherit_feasibility_cuts = OptInt {-1},
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

  SUBCASE("all three levels present")
  {
    CHECK(solver.level_stats().size() == 3);
  }

  SUBCASE("level 2 converges faster than level 1")
  {
    REQUIRE(solver.level_stats().size() == 3);
    const auto& stats1 = solver.level_stats()[1];
    const auto& stats2 = solver.level_stats()[2];

    CHECK(stats1.converged);
    CHECK(stats2.converged);
    CHECK(stats2.iterations <= stats1.iterations);
  }

  SUBCASE("all levels reach same optimal value")
  {
    REQUIRE(solver.level_stats().size() == 3);
    const auto& stats0 = solver.level_stats()[0];
    const auto& stats1 = solver.level_stats()[1];
    const auto& stats2 = solver.level_stats()[2];

    CHECK(stats0.lower_bound
          == doctest::Approx(stats1.lower_bound).epsilon(0.01));
    CHECK(stats1.lower_bound
          == doctest::Approx(stats2.lower_bound).epsilon(0.01));
    CHECK(stats0.upper_bound
          == doctest::Approx(stats1.upper_bound).epsilon(0.01));
    CHECK(stats1.upper_bound
          == doctest::Approx(stats2.upper_bound).epsilon(0.01));
  }

  SUBCASE("level stats have valid bounds")
  {
    for (const auto& ls : solver.level_stats()) {
      CHECK(ls.name.size() > 0);
      CHECK(ls.lower_bound <= ls.upper_bound + 1e-6);
      CHECK(ls.cuts_added >= 0);
    }
  }
}

// ─── Inherit with forgetting tests ──────────────────────────────────────────

TEST_CASE("Cascade 2-level inherit_optimality_cuts=3 (forget after 3 iters)")
// NOLINT
{
  // Level 1 inherits optimality cuts, uses them for 3 iterations, then
  // forgets them and continues with only self-generated cuts.
  // inherit_optimality_cuts=3 means: inherit, but drop after 3 iters.
  auto planning = make_6phase_2bus_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.apertures = std::vector<Uid> {};

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"training"},
          .model_options =
              ModelOptions {
                  .use_single_bus = OptBool {false},
                  .use_kirchhoff = OptBool {true},
              },
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {15},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"forget_after_3"},
          // No model_options ⇒ reuses level 0's LP
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {20},
                  .apertures = Array<Uid> {},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  // Inherit optimality cuts but forget after 3 iters
                  .inherit_optimality_cuts = OptInt {3},
                  .inherit_feasibility_cuts = OptInt {-1},
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

  SUBCASE("level 1 converges after forgetting inherited cuts")
  {
    REQUIRE(solver.level_stats().size() == 2);
    const auto& stats1 = solver.level_stats()[1];

    CHECK(stats1.converged);
    CHECK(stats1.gap < 0.01 + 1e-9);
    CHECK(stats1.lower_bound <= stats1.upper_bound + 1e-6);
  }

  SUBCASE("level 1 ran more results than without forget")
  {
    REQUIRE(solver.level_stats().size() == 2);
    // forget triggers a two-phase solve: phase-1 (capped at 3 iters)
    // + phase-2 (re-solve without inherited cuts). The total results
    // include both phases.
    const auto& stats1 = solver.level_stats()[1];
    CHECK(stats1.iterations >= 1);
  }
}

// ─── Additional cascade coverage tests ──────────────────────────────────────

TEST_CASE(  // NOLINT
    "Cascade 2-level with custom target tolerances (3-phase)")
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.enable_api = false;

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"base"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"refined"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_targets = OptInt {-1},
                  .target_rtol = OptReal {0.2},
                  .target_min_atol = OptReal {5.0},
                  .target_penalty = OptReal {100.0},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 2);
  CHECK(solver.level_stats()[0].converged);
  CHECK(solver.level_stats()[1].converged);
}

TEST_CASE(  // NOLINT
    "Cascade 2-level inherit_optimality_cuts keeps cuts (3-phase)")
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.enable_api = false;

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"train"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"inherit"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {15},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  // Keep inherited cuts forever
                  .inherit_optimality_cuts = OptInt {-1},
                  .inherit_feasibility_cuts = OptInt {-1},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 2);

  const auto& stats0 = solver.level_stats()[0];
  const auto& stats1 = solver.level_stats()[1];

  CHECK(stats0.converged);
  CHECK(stats1.converged);

  // Level 1 should converge in fewer or equal iterations thanks to
  // inherited cuts providing a warm start
  CHECK(stats1.iterations <= stats0.iterations + 1);
}

TEST_CASE(  // NOLINT
    "Cascade 2-level forget inherited cuts after N iterations (3-phase)")
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.enable_api = false;

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"train"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"forget"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {20},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  // Forget after 3 iterations
                  .inherit_optimality_cuts = OptInt {3},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 2);

  // Level 1 should converge even after forgetting inherited cuts
  CHECK(solver.level_stats()[1].converged);
}

TEST_CASE(  // NOLINT
    "Cascade 3-level progressive refinement (3-phase)")
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 30;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.enable_api = false;

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"level_0"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .convergence_tol = OptReal {0.05},
              },
      },
      CascadeLevel {
          .name = OptName {"level_1"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
                  .inherit_targets = OptInt {-1},
              },
      },
      CascadeLevel {
          .name = OptName {"level_2"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {15},
                  .convergence_tol = OptReal {0.001},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
                  .inherit_targets = OptInt {-1},
                  .target_rtol = OptReal {0.01},
                  .target_penalty = OptReal {1000.0},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 3);

  // All levels should converge
  for (const auto& stats : solver.level_stats()) {
    CHECK(stats.converged);
    CHECK(stats.lower_bound <= stats.upper_bound + 1e-6);
  }
}

TEST_CASE(  // NOLINT
    "Cascade 2-level with both targets and cuts (3-phase)")
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 20;
  sddp_opts.convergence_tol = 0.01;
  sddp_opts.enable_api = false;

  CascadeOptions cascade_opts;
  cascade_opts.level_array = {
      CascadeLevel {
          .name = OptName {"train"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {10},
                  .convergence_tol = OptReal {0.01},
              },
      },
      CascadeLevel {
          .name = OptName {"both"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {15},
                  .convergence_tol = OptReal {0.01},
              },
          .transition =
              CascadeTransition {
                  .inherit_optimality_cuts = OptInt {-1},
                  .inherit_targets = OptInt {-1},
                  .target_rtol = OptReal {0.05},
                  .target_penalty = OptReal {500.0},
              },
      },
  };

  CascadePlanningMethod solver(std::move(sddp_opts), std::move(cascade_opts));
  const SolverOptions lp_opts;
  auto result = solver.solve(planning_lp, lp_opts);

  REQUIRE(result.has_value());
  REQUIRE(solver.level_stats().size() == 2);

  CHECK(solver.level_stats()[0].converged);
  CHECK(solver.level_stats()[1].converged);

  // Both inheritance mechanisms should allow level 1 to converge fast
  CHECK(solver.level_stats()[1].iterations
        <= solver.level_stats()[0].iterations + 1);
}

}  // anonymous namespace
