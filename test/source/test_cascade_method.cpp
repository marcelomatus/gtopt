/**
 * @file      test_cascade_method.hpp
 * @brief     Unit tests for the multi-level CascadePlanningMethod
 * @date      2026-03-22
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/cascade_method.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_options_lp.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;

namespace
{

// ─── Data structure tests ───────────────────────────────────────────────────
//
// Note: Default-construction and merge tests for ModelOptions,
// CascadeTransition, CascadeLevelMethod, CascadeLevel, and CascadeOptions
// are covered authoritatively in test_model_options.cpp and
// test_cascade_options.cpp.  Only tests specific to this file's subject
// (CascadePlanningMethod, JSON/factory/integration) live here.

TEST_CASE("MethodType::cascade enum")  // NOLINT
{
  using namespace gtopt;

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
  using namespace gtopt;

  const PlanningOptionsLP options_lp;
  CHECK(!options_lp.has_cascade_levels());
  CHECK(options_lp.cascade_levels().empty());
}

// ─── JSON parsing tests ────────────────────────────────────────────────────

TEST_CASE("JSON parsing of cascade method")  // NOLINT
{
  using namespace gtopt;

  constexpr std::string_view json_str = R"json(
  {
    "options": {
      "method": "cascade"
    }
  }
  )json";

  const auto planning = parse_planning_json(json_str);
  const PlanningOptionsLP options_lp(planning.options);
  CHECK(options_lp.method_type_enum() == MethodType::cascade);
}

TEST_CASE("JSON parsing of cascade levels")  // NOLINT
{
  using namespace gtopt;

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
              "inherit_optimality_cuts": -1
            }
          }
        ]
      }
    }
  }
  )json";

  const auto planning = parse_planning_json(json_str);
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
  }
}

TEST_CASE("JSON parsing of cascade level active field")  // NOLINT
{
  using namespace gtopt;

  // Exercises the CascadeLevel `active` OptBool wired through
  // json_cascade_options.hpp and consumed by cascade_method.cpp:
  //   `if (!level.active.value_or(true)) continue;`
  //
  // Verifies three modes: active=false (skip), active=true (explicit
  // run), and omitted (defaults to run).  This test guards the JSON
  // binding from silently dropping the field.
  constexpr std::string_view json_str = R"json(
  {
    "options": {
      "method": "cascade",
      "cascade_options": {
        "level_array": [
          {"name": "disabled", "active": false},
          {"name": "enabled", "active": true},
          {"name": "default"}
        ]
      }
    }
  }
  )json";

  const auto planning = parse_planning_json(json_str);
  const PlanningOptionsLP options_lp(planning.options);

  REQUIRE(options_lp.has_cascade_levels());
  const auto& levels = options_lp.cascade_levels();
  REQUIRE(levels.size() == 3);

  SUBCASE("active=false parsed")
  {
    CHECK(levels[0].name.value_or("") == "disabled");
    REQUIRE(levels[0].active.has_value());
    CHECK(*levels[0].active == false);
    CHECK(levels[0].active.value_or(true) == false);
  }

  SUBCASE("active=true parsed")
  {
    CHECK(levels[1].name.value_or("") == "enabled");
    REQUIRE(levels[1].active.has_value());
    CHECK(*levels[1].active == true);
    CHECK(levels[1].active.value_or(true) == true);
  }

  SUBCASE("active omitted defaults to active")
  {
    CHECK(levels[2].name.value_or("") == "default");
    CHECK_FALSE(levels[2].active.has_value());
    // cascade_method.cpp uses `value_or(true)` so absent → active.
    CHECK(levels[2].active.value_or(true) == true);
  }
}

TEST_CASE("JSON cascade options with empty levels uses defaults")  // NOLINT
{
  using namespace gtopt;

  constexpr std::string_view json_str = R"json(
  {
    "options": {
      "method": "cascade"
    }
  }
  )json";

  const auto planning = parse_planning_json(json_str);
  const PlanningOptionsLP options_lp(planning.options);
  CHECK(!options_lp.has_cascade_levels());
}

// ─── Factory tests ──────────────────────────────────────────────────────────

TEST_CASE("make_planning_method factory - cascade")  // NOLINT
{
  using namespace gtopt;

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
  using namespace gtopt;

  PlanningOptions opts;
  opts.method = MethodType::cascade;
  const PlanningOptionsLP options_lp(std::move(opts));

  auto solver = make_planning_method(options_lp, 1);
  REQUIRE(solver != nullptr);
}

// ─── Solver integration tests ───────────────────────────────────────────────

TEST_CASE("CascadePlanningMethod basic 3-phase hydro")  // NOLINT
{
  using namespace gtopt;

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
                  .inherit_optimality_cuts = OptInt {-1},
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

  SUBCASE("iteration indices are strictly monotonic across levels")
  {
    // Regression test for the cascade iteration_offset_hint wiring:
    // each level's SDDPMethod must receive a global iteration offset so
    // its training indices start past the previous level's range.
    // Without the wiring, every level's first iteration_index is 0 and
    // save_cuts_for_iteration stacks cuts from different levels under
    // the same index in m_cut_store_.
    REQUIRE(result.has_value());
    const auto& all = solver.all_results();
    REQUIRE(!all.empty());
    for (std::size_t i = 1; i < all.size(); ++i) {
      const auto prev = static_cast<Index>(all[i - 1].iteration_index);
      const auto curr = static_cast<Index>(all[i].iteration_index);
      CHECK(curr > prev);
    }
  }
}

TEST_CASE("CascadePlanningMethod with empty options = single level")  // NOLINT
{
  using namespace gtopt;

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
  using namespace gtopt;

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

// Note: merge-semantics tests for CascadeLevelMethod, CascadeTransition,
// CascadeOptions, and ModelOptions are covered in test_cascade_options.cpp
// and test_model_options.cpp.

// ─── SDDP option resolution: base → cascade global → per-level ────────────

TEST_CASE("CascadePlanningMethod SDDP option priority chain")  // NOLINT
{
  using namespace gtopt;

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
  using namespace gtopt;
  // NOLINTBEGIN(bugprone-unchecked-optional-access,
  // modernize-use-designated-initializers)

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

// ─── build_level_sddp_opts priority chain: stationary_window, elastic_mode,
//     elastic_penalty ────────────────────────────────────────────────────────
//
// ``build_level_sddp_opts`` is private on ``CascadePlanningMethod``.  We
// expose it through a thin testable subclass declared in this anonymous
// namespace so we can pin the 3-layer priority chain:
//   base SDDPOptions → cascade-global CascadeLevelMethod defaults →
//   per-level CascadeLevelMethod overrides.

class TestableCascade : public CascadePlanningMethod
{
public:
  using CascadePlanningMethod::CascadePlanningMethod;

  /// Expose the private helper for white-box unit testing.
  [[nodiscard]] auto test_build_level_sddp_opts(
      const std::optional<CascadeLevelMethod>& level_solver,
      int remaining_budget = -1) const -> SDDPOptions
  {
    return build_level_sddp_opts(level_solver, remaining_budget);
  }
};

TEST_CASE(  // NOLINT
    "CascadePlanningMethod build_level_sddp_opts new fields")
{
  SUBCASE("per-level overrides all 5 new fields")
  {
    // Base: all defaults (stationary_tol=0.005, stationary_window=4,
    // elastic_filter_mode=single_cut, elastic_penalty=1000).
    SDDPOptions base;
    CascadeOptions cascade;

    TestableCascade solver(std::move(base), std::move(cascade));

    CascadeLevelMethod level;
    level.stationary_tol = OptReal {0.08};
    level.stationary_gap_ceiling = OptReal {0.50};
    level.stationary_window = OptInt {2};
    level.elastic_mode = OptName {"multi_cut"};
    level.elastic_penalty = OptReal {750.0};

    const auto opts = solver.test_build_level_sddp_opts(level);

    CHECK(opts.stationary_tol == doctest::Approx(0.08));
    CHECK(opts.stationary_gap_ceiling == doctest::Approx(0.50));
    CHECK(opts.stationary_window == 2);
    // ``opts`` is the internal solver ``SDDPOptions`` (sddp_types.hpp),
    // where the field is ``elastic_filter_mode`` (non-optional enum),
    // NOT the JSON-binding ``elastic_mode`` (OptName) used at the
    // ``CascadeLevelMethod`` overlay surface.  ``build_level_sddp_opts``
    // is the boundary that parses the string into the enum.
    CHECK(opts.elastic_filter_mode == ElasticFilterMode::multi_cut);
    CHECK(opts.elastic_penalty == doctest::Approx(750.0));
  }

  SUBCASE("cascade-global wins when per-level stationary_tol is absent")
  {
    SDDPOptions base;
    base.stationary_tol = 0.005;  // base default

    CascadeOptions cascade;
    cascade.sddp_options.stationary_tol = OptReal {0.04};  // cascade global

    TestableCascade solver(std::move(base), std::move(cascade));

    // Per-level leaves stationary_tol unset → cascade global should win.
    CascadeLevelMethod level;
    level.convergence_tol = OptReal {0.01};  // set something else

    const auto opts = solver.test_build_level_sddp_opts(level);

    CHECK(opts.stationary_tol == doctest::Approx(0.04));
  }

  SUBCASE("base wins when per-level and cascade-global stationary_tol absent")
  {
    SDDPOptions base;
    base.stationary_tol = 0.005;  // base is the only source

    CascadeOptions cascade;
    // cascade.sddp_options.stationary_tol is absent

    TestableCascade solver(std::move(base), std::move(cascade));

    // Per-level also absent → base should be preserved.
    const auto opts = solver.test_build_level_sddp_opts(std::nullopt);

    CHECK(opts.stationary_tol == doctest::Approx(0.005));
  }

  SUBCASE("remaining_budget caps max_iterations")
  {
    SDDPOptions base;
    base.max_iterations = 100;

    CascadeOptions cascade;
    TestableCascade solver(std::move(base), std::move(cascade));

    CascadeLevelMethod level;
    level.max_iterations = OptInt {50};

    // remaining_budget = 10 must clamp the 50 per-level value.
    const auto opts = solver.test_build_level_sddp_opts(level, 10);
    CHECK(opts.max_iterations == 10);
  }

  SUBCASE("remaining_budget = -1 does not clamp")
  {
    SDDPOptions base;
    base.max_iterations = 100;

    CascadeOptions cascade;
    TestableCascade solver(std::move(base), std::move(cascade));

    CascadeLevelMethod level;
    level.max_iterations = OptInt {30};

    const auto opts = solver.test_build_level_sddp_opts(level, -1);
    CHECK(opts.max_iterations == 30);
  }
}

}  // anonymous namespace

// NOLINTEND(bugprone-unchecked-optional-access,
// modernize-use-designated-initializers)
