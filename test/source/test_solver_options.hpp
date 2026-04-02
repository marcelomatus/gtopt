/**
 * @file      test_solver_options.hpp
 * @brief     Unit tests for the SolverOptions class
 * @date      Sun May  5 11:30:00 2025
 * @author    Claude
 * @copyright BSD-3-Clause
 *
 * This module contains the unit tests for the SolverOptions class and related
 * functionality.
 */

#include <filesystem>
#include <fstream>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/json/json_solver_options.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("SolverOptions - Default construction")
{
  // Test default construction of SolverOptions
  const SolverOptions options {};

  // Non-optional fields keep their sentinel defaults
  CHECK(options.algorithm == LPAlgo::default_algo);
  CHECK(options.threads == 0);
  CHECK(options.presolve == true);
  CHECK(options.log_level == 0);
  CHECK_FALSE(options.log_mode.has_value());

  // Tolerance fields are nullopt by default — solver uses its own defaults
  CHECK_FALSE(options.optimal_eps.has_value());
  CHECK_FALSE(options.feasible_eps.has_value());
  CHECK_FALSE(options.barrier_eps.has_value());
}

TEST_CASE("SolverLogMode - enumeration values and names")
{
  CHECK(std::to_underlying(SolverLogMode::nolog) == 0);
  CHECK(std::to_underlying(SolverLogMode::detailed) == 1);

  CHECK(enum_name(SolverLogMode::nolog) == "nolog");
  CHECK(enum_name(SolverLogMode::detailed) == "detailed");

  CHECK(enum_from_name<SolverLogMode>("nolog") == SolverLogMode::nolog);
  CHECK(enum_from_name<SolverLogMode>("detailed") == SolverLogMode::detailed);
  CHECK_FALSE(enum_from_name<SolverLogMode>("invalid").has_value());
}

TEST_CASE("SolverScaling - enumeration values and names")
{
  CHECK(std::to_underlying(SolverScaling::none) == 0);
  CHECK(std::to_underlying(SolverScaling::automatic) == 1);
  CHECK(std::to_underlying(SolverScaling::aggressive) == 2);

  CHECK(enum_name(SolverScaling::none) == "none");
  CHECK(enum_name(SolverScaling::automatic) == "automatic");
  CHECK(enum_name(SolverScaling::aggressive) == "aggressive");

  CHECK(enum_from_name<SolverScaling>("none") == SolverScaling::none);
  CHECK(enum_from_name<SolverScaling>("automatic") == SolverScaling::automatic);
  CHECK(enum_from_name<SolverScaling>("aggressive")
        == SolverScaling::aggressive);
  CHECK_FALSE(enum_from_name<SolverScaling>("invalid").has_value());
}

TEST_CASE("SolverOptions - Custom construction")
{
  // Test constructing SolverOptions with custom values
  const SolverOptions options {
      .algorithm = LPAlgo::barrier,
      .threads = 4,
      .presolve = false,
      .optimal_eps = 1e-6,
      .feasible_eps = 1e-5,
      .barrier_eps = 1e-7,
      .log_level = 2,
  };

  // Verify custom values
  CHECK(options.algorithm == LPAlgo::barrier);
  CHECK(options.threads == 4);
  CHECK(options.presolve == false);
  CHECK((options.optimal_eps && *options.optimal_eps == doctest::Approx(1e-6)));
  CHECK(
      (options.feasible_eps && *options.feasible_eps == doctest::Approx(1e-5)));
  CHECK((options.barrier_eps && *options.barrier_eps == doctest::Approx(1e-7)));
  CHECK(options.log_level == 2);
}

TEST_CASE("SolverOptions - LPAlgo enumeration values")
{
  // Test the LPAlgo enumeration values
  CHECK(std::to_underlying(LPAlgo::default_algo) == 0);
  CHECK(std::to_underlying(LPAlgo::primal) == 1);
  CHECK(std::to_underlying(LPAlgo::dual) == 2);
  CHECK(std::to_underlying(LPAlgo::barrier) == 3);
  CHECK(std::to_underlying(LPAlgo::last_algo) == 4);
}

TEST_CASE("SolverOptions - JSON serialization and deserialization")
{
  SUBCASE("with tolerance values")
  {
    // Create a SolverOptions object with non-default values
    const SolverOptions original {
        .algorithm = LPAlgo::primal,
        .threads = 2,
        .presolve = false,
        .optimal_eps = 1e-6,
        .feasible_eps = 1e-5,
        .barrier_eps = 1e-7,
        .log_level = 1,
    };

    // Serialize to JSON
    const auto json_string = daw::json::to_json(original);

    // Deserialize from JSON
    const auto deserialized = daw::json::from_json<SolverOptions>(json_string);

    // Verify deserialized values match original
    CHECK(deserialized.algorithm == original.algorithm);
    CHECK(deserialized.threads == original.threads);
    CHECK(deserialized.presolve == original.presolve);
    CHECK(deserialized.optimal_eps.value_or(-1.0)
          == doctest::Approx(original.optimal_eps.value_or(-1.0)));
    CHECK(deserialized.feasible_eps.value_or(-1.0)
          == doctest::Approx(original.feasible_eps.value_or(-1.0)));
    CHECK(deserialized.barrier_eps.value_or(-1.0)
          == doctest::Approx(original.barrier_eps.value_or(-1.0)));
    CHECK(deserialized.log_level == original.log_level);
  }

  SUBCASE("without tolerance values – nullopt round-trips as null")
  {
    // Default construction leaves tolerances as nullopt
    const SolverOptions original {};

    const auto json_string = daw::json::to_json(original);

    const auto deserialized = daw::json::from_json<SolverOptions>(json_string);

    CHECK_FALSE(deserialized.optimal_eps.has_value());
    CHECK_FALSE(deserialized.feasible_eps.has_value());
    CHECK_FALSE(deserialized.barrier_eps.has_value());
  }
}

TEST_CASE("SolverOptions - Usage with LinearInterface")
{
  // Create a minimal linear problem for testing
  FlatLinearProblem flat_lp;
  flat_lp.name = "test_problem";
  // Setup a simple 1x1 LP problem: min x s.t. x >= 1
  flat_lp.ncols = 1;
  flat_lp.nrows = 1;
  flat_lp.matbeg = {0, 1};  // Column start indices
  flat_lp.matind = {0};  // Row indices
  flat_lp.matval = {1.0};  // Matrix coefficients
  flat_lp.collb = {1.0};  // Column lower bounds
  flat_lp.colub = {10.0};  // Column upper bounds
  flat_lp.objval = {1.0};  // Objective coefficients
  flat_lp.rowlb = {1.0};  // Row lower bounds
  flat_lp.rowub = {10.0};  // Row upper bounds
  flat_lp.colnm = {"x"};  // Column names
  flat_lp.rownm = {"r1"};  // Row names

  // Create LinearInterface with the default available solver
  const auto& reg = SolverRegistry::instance();
  LinearInterface lp(reg.default_solver(), flat_lp);

  // Create solver options with custom values
  const SolverOptions solver_options {
      .algorithm = LPAlgo::primal,
      .presolve = true,
      .optimal_eps = 1e-6,
      .feasible_eps = 1e-5,
  };

  // Solve with custom options
  const auto result = lp.initial_solve(solver_options);

  // Check that the solve worked
  CHECK(result);
  CHECK(lp.is_optimal() == true);
  CHECK(lp.get_obj_value() == doctest::Approx(1.0));

  // Get solution and check it
  const auto sol = lp.get_col_sol();
  REQUIRE(sol.size() == 1);
  CHECK(sol[0] == doctest::Approx(1.0));
}

TEST_CASE("SolverOptions - Numerical parameters")
{
  // Test with different numerical parameter settings

  SUBCASE(
      "Default – tolerances are nullopt (solver uses its built-in defaults)")
  {
    const SolverOptions options {};
    CHECK_FALSE(options.optimal_eps.has_value());
    CHECK_FALSE(options.feasible_eps.has_value());
    CHECK_FALSE(options.barrier_eps.has_value());
  }

  SUBCASE("Custom tolerances")
  {
    SolverOptions options;
    options.optimal_eps = 1e-8;
    options.feasible_eps = 1e-7;
    options.barrier_eps = 1e-6;

    CHECK(options.optimal_eps.value_or(0.0) == doctest::Approx(1e-8));
    CHECK(options.feasible_eps.value_or(0.0) == doctest::Approx(1e-7));
    CHECK(options.barrier_eps.value_or(0.0) == doctest::Approx(1e-6));
  }

  SUBCASE("Realistic tolerances")
  {
    SolverOptions options;
    options.optimal_eps = 1e-6;  // Typical optimality tolerance
    options.feasible_eps = 1e-6;  // Typical feasibility tolerance
    options.barrier_eps = 1e-8;  // Typical barrier convergence tolerance

    CHECK(options.optimal_eps.value_or(0.0) == doctest::Approx(1e-6));
    CHECK(options.feasible_eps.value_or(0.0) == doctest::Approx(1e-6));
    CHECK(options.barrier_eps.value_or(0.0) == doctest::Approx(1e-8));
  }
}

TEST_CASE(  // NOLINT
    "SolverOptions - merge() uses merge_opt (source wins) on optional fields")
{
  SUBCASE("merge sets nullopt field from source")
  {
    SolverOptions dest {};
    const SolverOptions src {
        .optimal_eps = 1e-8,
        .feasible_eps = 1e-7,
    };
    dest.merge(src);

    CHECK(dest.optimal_eps.value_or(0.0) == doctest::Approx(1e-8));
    CHECK(dest.feasible_eps.value_or(0.0) == doctest::Approx(1e-7));
    CHECK_FALSE(dest.barrier_eps.has_value());
  }

  SUBCASE("merge source wins: source overwrites existing value")
  {
    SolverOptions dest {
        .optimal_eps = 1e-6,
    };
    const SolverOptions src {
        .optimal_eps = 1e-10,
    };
    dest.merge(src);

    // Source value should win (merge_opt semantics)
    CHECK(dest.optimal_eps.value_or(0.0) == doctest::Approx(1e-10));
  }

  SUBCASE("merge leaves dest when source is nullopt")
  {
    SolverOptions dest {
        .optimal_eps = 1e-6,
    };
    const SolverOptions src {};
    dest.merge(src);

    // Dest keeps its value when source is nullopt
    CHECK(dest.optimal_eps.value_or(0.0) == doctest::Approx(1e-6));
  }

  SUBCASE("merge leaves both nullopt when neither is set")
  {
    SolverOptions dest {};
    const SolverOptions src {};
    dest.merge(src);

    CHECK_FALSE(dest.optimal_eps.has_value());
    CHECK_FALSE(dest.feasible_eps.has_value());
    CHECK_FALSE(dest.barrier_eps.has_value());
  }

  SUBCASE("merge sets barrier_eps and time_limit from source")
  {
    SolverOptions dest {};
    const SolverOptions src {
        .barrier_eps = 1e-9,
        .time_limit = 300.0,
    };
    dest.merge(src);

    CHECK(dest.barrier_eps.value_or(0.0) == doctest::Approx(1e-9));
    CHECK(dest.time_limit.value_or(0.0) == doctest::Approx(300.0));
  }

  SUBCASE("merge source overwrites existing barrier_eps and time_limit")
  {
    SolverOptions dest {
        .barrier_eps = 1e-6,
        .time_limit = 60.0,
    };
    const SolverOptions src {
        .barrier_eps = 1e-12,
        .time_limit = 600.0,
    };
    dest.merge(src);

    CHECK(dest.barrier_eps.value_or(0.0) == doctest::Approx(1e-12));
    CHECK(dest.time_limit.value_or(0.0) == doctest::Approx(600.0));
  }

  SUBCASE("merge sets log_mode from source when dest is nullopt")
  {
    SolverOptions dest {};
    const SolverOptions src {
        .log_mode = SolverLogMode::detailed,
    };
    dest.merge(src);

    REQUIRE(dest.log_mode.has_value());
    CHECK(*dest.log_mode == SolverLogMode::detailed);
  }

  SUBCASE("merge source overwrites existing log_mode")
  {
    SolverOptions dest {
        .log_mode = SolverLogMode::nolog,
    };
    const SolverOptions src {
        .log_mode = SolverLogMode::detailed,
    };
    dest.merge(src);

    REQUIRE(dest.log_mode.has_value());
    CHECK(*dest.log_mode == SolverLogMode::detailed);
  }

  SUBCASE("merge preserves dest log_mode when source is nullopt")
  {
    SolverOptions dest {
        .log_mode = SolverLogMode::nolog,
    };
    const SolverOptions src {};
    dest.merge(src);

    REQUIRE(dest.log_mode.has_value());
    CHECK(*dest.log_mode == SolverLogMode::nolog);
  }
}

TEST_CASE(  // NOLINT
    "SolverOptions - PlanningOptionsLP merge: method-specific overrides global")
{
  // Verify that method-specific solver options (e.g. sddp forward)
  // override global solver_options, and global fills in the rest.
  PlanningOptions planning;
  planning.solver_options.optimal_eps = 1e-6;
  planning.solver_options.feasible_eps = 1e-7;
  planning.solver_options.log_mode = SolverLogMode::detailed;

  // SDDP forward overrides optimal_eps but not feasible_eps or log_mode
  SolverOptions fwd_opts;
  fwd_opts.optimal_eps = 1e-10;
  planning.sddp_options.forward_solver_options = fwd_opts;

  PlanningOptionsLP plp(planning);
  const auto resolved = plp.sddp_forward_solver_options();

  // Method-specific wins for optimal_eps
  CHECK(resolved.optimal_eps.value_or(0.0) == doctest::Approx(1e-10));
  // Global fills in feasible_eps (not set in method-specific)
  CHECK(resolved.feasible_eps.value_or(0.0) == doctest::Approx(1e-7));
  // Global fills in log_mode (not set in method-specific)
  REQUIRE(resolved.log_mode.has_value());
  CHECK(*resolved.log_mode == SolverLogMode::detailed);
}

TEST_CASE(  // NOLINT
    "SolverOptions - PlanningOptionsLP merge: monolithic overrides global")
{
  PlanningOptions planning;
  planning.solver_options.barrier_eps = 1e-8;
  planning.solver_options.log_mode = SolverLogMode::nolog;

  // Monolithic overrides log_mode but not barrier_eps
  SolverOptions mono_opts;
  mono_opts.log_mode = SolverLogMode::detailed;
  planning.monolithic_options.solver_options = mono_opts;

  PlanningOptionsLP plp(planning);
  const auto resolved = plp.monolithic_solver_options();

  // Monolithic wins for log_mode
  REQUIRE(resolved.log_mode.has_value());
  CHECK(*resolved.log_mode == SolverLogMode::detailed);
  // Global fills in barrier_eps
  CHECK(resolved.barrier_eps.value_or(0.0) == doctest::Approx(1e-8));
}

TEST_CASE(  // NOLINT
    "SolverOptions - PlanningOptionsLP: no method-specific falls back to "
    "global")
{
  PlanningOptions planning;
  planning.solver_options.optimal_eps = 1e-6;
  planning.solver_options.log_mode = SolverLogMode::detailed;

  // No method-specific options set
  PlanningOptionsLP plp(planning);

  const auto mono = plp.monolithic_solver_options();
  CHECK(mono.optimal_eps.value_or(0.0) == doctest::Approx(1e-6));
  REQUIRE(mono.log_mode.has_value());
  CHECK(*mono.log_mode == SolverLogMode::detailed);

  const auto fwd = plp.sddp_forward_solver_options();
  CHECK(fwd.optimal_eps.value_or(0.0) == doctest::Approx(1e-6));
  REQUIRE(fwd.log_mode.has_value());
  CHECK(*fwd.log_mode == SolverLogMode::detailed);
}

TEST_CASE("SolverOptions - Threading options")
{
  // Test with different threading options

  SUBCASE("Default threading (auto)")
  {
    SolverOptions options;
    options.threads = 0;  // 0 means automatic selection

    CHECK(options.threads == 0);
  }

  SUBCASE("Single-threaded")
  {
    SolverOptions options;
    options.threads = 1;

    CHECK(options.threads == 1);
  }

  SUBCASE("Multi-threaded")
  {
    SolverOptions options;
    options.threads = 4;

    CHECK(options.threads == 4);
  }
}

TEST_CASE("SolverOptions - Presolve and logging options")
{
  // Test presolve and logging options

  SUBCASE("Default presolve (enabled)")
  {
    const SolverOptions options;
    // Default is true

    CHECK(options.presolve == true);
  }

  SUBCASE("Disable presolve")
  {
    SolverOptions options;
    options.presolve = false;

    CHECK(options.presolve == false);
  }

  SUBCASE("Default logging (silent)")
  {
    const SolverOptions options;
    // Default is 0

    CHECK(options.log_level == 0);
  }

  SUBCASE("Verbose logging")
  {
    SolverOptions options;
    options.log_level = 3;  // Highly verbose

    CHECK(options.log_level == 3);
  }
}

TEST_CASE("SolverOptions - Algorithm selection with dual simplex")  // NOLINT
{
  // Verify that selecting the dual simplex algorithm still yields the correct
  // solution on the same simple 1x1 problem.
  FlatLinearProblem flat_lp;
  flat_lp.name = "dual_algo_test";
  flat_lp.ncols = 1;
  flat_lp.nrows = 1;
  flat_lp.matbeg = {0, 1};
  flat_lp.matind = {0};
  flat_lp.matval = {1.0};
  flat_lp.collb = {2.0};
  flat_lp.colub = {10.0};
  flat_lp.objval = {1.0};
  flat_lp.rowlb = {2.0};
  flat_lp.rowub = {10.0};
  flat_lp.colnm = {"x"};
  flat_lp.rownm = {"r1"};

  const auto& reg = SolverRegistry::instance();
  LinearInterface lp(reg.default_solver(), flat_lp);

  const SolverOptions solver_options {
      .algorithm = LPAlgo::dual,
  };

  const auto result = lp.initial_solve(solver_options);

  CHECK(result);
  CHECK(lp.is_optimal() == true);
  CHECK(lp.get_obj_value() == doctest::Approx(2.0));
  const auto sol = lp.get_col_sol();
  REQUIRE(sol.size() == 1);
  CHECK(sol[0] == doctest::Approx(2.0));
}

TEST_CASE("SolverOptions - Algorithm selection with primal simplex")  // NOLINT
{
  FlatLinearProblem flat_lp;
  flat_lp.name = "primal_algo_test";
  flat_lp.ncols = 1;
  flat_lp.nrows = 1;
  flat_lp.matbeg = {0, 1};
  flat_lp.matind = {0};
  flat_lp.matval = {1.0};
  flat_lp.collb = {3.0};
  flat_lp.colub = {10.0};
  flat_lp.objval = {1.0};
  flat_lp.rowlb = {3.0};
  flat_lp.rowub = {10.0};
  flat_lp.colnm = {"x"};
  flat_lp.rownm = {"r1"};

  const auto& reg = SolverRegistry::instance();
  LinearInterface lp(reg.default_solver(), flat_lp);

  const SolverOptions solver_options {
      .algorithm = LPAlgo::primal,
  };

  const auto result = lp.initial_solve(solver_options);

  CHECK(result);
  CHECK(lp.is_optimal() == true);
  CHECK(lp.get_obj_value() == doctest::Approx(3.0));
  const auto sol = lp.get_col_sol();
  REQUIRE(sol.size() == 1);
  CHECK(sol[0] == doctest::Approx(3.0));
}

TEST_CASE("SolverOptions - All algorithms solve correctly on 2x2 LP")  // NOLINT
{
  // Verify all available algorithms solve:
  //   min  x + y
  //   s.t. x + y >= 4
  //        x, y >= 0
  // Optimal solution: any (x,y) with x+y=4, obj=4.

  auto make_lp = []() -> LinearInterface
  {
    FlatLinearProblem flat_lp;
    flat_lp.name = "algo_test_2x2";
    flat_lp.ncols = 2;
    flat_lp.nrows = 1;
    flat_lp.matbeg = {0, 1, 2};
    flat_lp.matind = {0, 0};
    flat_lp.matval = {1.0, 1.0};
    flat_lp.collb = {0.0, 0.0};
    flat_lp.colub = {1e30, 1e30};
    flat_lp.objval = {1.0, 1.0};
    flat_lp.rowlb = {4.0};
    flat_lp.rowub = {1e30};
    flat_lp.colnm = {"x", "y"};
    flat_lp.rownm = {"sum_row"};
    return LinearInterface(SolverRegistry::instance().default_solver(),
                           flat_lp);
  };

  SUBCASE("default algorithm")
  {
    auto lp = make_lp();
    const auto result =
        lp.initial_solve(SolverOptions {.algorithm = LPAlgo::default_algo});
    CHECK(result);
    CHECK(lp.get_obj_value() == doctest::Approx(4.0));
  }

  SUBCASE("primal simplex")
  {
    auto lp = make_lp();
    const auto result =
        lp.initial_solve(SolverOptions {.algorithm = LPAlgo::primal});
    CHECK(result);
    CHECK(lp.get_obj_value() == doctest::Approx(4.0));
  }

  SUBCASE("dual simplex")
  {
    auto lp = make_lp();
    const auto result =
        lp.initial_solve(SolverOptions {.algorithm = LPAlgo::dual});
    CHECK(result);
    CHECK(lp.get_obj_value() == doctest::Approx(4.0));
  }
}

// ---------------------------------------------------------------------------
// Barrier + threads tests (all available solvers including HiGHS)
// ---------------------------------------------------------------------------

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Build a 4-variable LP to exercise barrier:
///   min  x1 + 2*x2 + 3*x3 + 4*x4
///   s.t. x1 + x2          >= 5
///             x2 + x3      >= 3
///                  x3 + x4 >= 4
///        x1, x2, x3, x4 >= 0
/// Optimal: x1=5, x2=0, x3=3, x4=1, obj=18.
auto make_barrier_test_lp(std::string_view solver_name) -> LinearInterface
{
  FlatLinearProblem flat_lp;
  flat_lp.name = "barrier_threads_test";
  flat_lp.ncols = 4;
  flat_lp.nrows = 3;
  flat_lp.matbeg = {0, 1, 3, 5, 6};
  flat_lp.matind = {0, 0, 1, 1, 2, 2};
  flat_lp.matval = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  flat_lp.collb = {0.0, 0.0, 0.0, 0.0};
  flat_lp.colub = {1e30, 1e30, 1e30, 1e30};
  flat_lp.objval = {1.0, 2.0, 3.0, 4.0};
  flat_lp.rowlb = {5.0, 3.0, 4.0};
  flat_lp.rowub = {1e30, 1e30, 1e30};
  flat_lp.colnm = {"x1", "x2", "x3", "x4"};
  flat_lp.rownm = {"r1", "r2", "r3"};
  return LinearInterface(solver_name, flat_lp);
}

}  // namespace

TEST_CASE("SolverOptions - barrier with threads on all solvers")  // NOLINT
{
  const auto& reg = SolverRegistry::instance();
  const auto solvers = reg.available_solvers();
  REQUIRE(!solvers.empty());

  for (const auto& solver_name : solvers) {
    CAPTURE(solver_name);

    SUBCASE(std::string(solver_name).c_str())
    {
      auto lp = make_barrier_test_lp(solver_name);

      const SolverOptions opts {
          .algorithm = LPAlgo::barrier,
          .threads = 4,
          .presolve = true,
      };

      const auto result = lp.initial_solve(opts);

      CHECK(result.has_value());
      CHECK(lp.is_optimal());
      CHECK(lp.get_obj_value() == doctest::Approx(17.0));

      const auto sol = lp.get_col_sol();
      REQUIRE(sol.size() == 4);
      CHECK(sol[0] + sol[1] >= doctest::Approx(5.0));
      CHECK(sol[1] + sol[2] >= doctest::Approx(3.0));
      CHECK(sol[2] + sol[3] >= doctest::Approx(4.0));
    }
  }
}

TEST_CASE(
    "SolverOptions - barrier then resolve with dual on all solvers")  // NOLINT
{
  // Test the SDDP workflow: initial solve with barrier, then resolve with
  // dual simplex (reuse_basis).  Validates the CPLEX resolve() fix.
  const auto& reg = SolverRegistry::instance();
  const auto solvers = reg.available_solvers();
  REQUIRE(!solvers.empty());

  for (const auto& solver_name : solvers) {
    CAPTURE(solver_name);

    SUBCASE(std::string(solver_name).c_str())
    {
      auto lp = make_barrier_test_lp(solver_name);

      // Step 1: initial solve with barrier + threads
      const SolverOptions barrier_opts {
          .algorithm = LPAlgo::barrier,
          .threads = 4,
      };
      auto r1 = lp.initial_solve(barrier_opts);
      CHECK(r1.has_value());
      CHECK(lp.get_obj_value() == doctest::Approx(17.0));

      // Step 2: modify a bound and resolve with dual simplex (warm start)
      lp.set_row_low(RowIndex {0}, 6.0);

      const SolverOptions resolve_opts {
          .algorithm = LPAlgo::dual,
          .reuse_basis = true,
      };
      auto r2 = lp.resolve(resolve_opts);
      CHECK(r2.has_value());
      CHECK(lp.is_optimal());
      // With x1+x2 >= 6 instead of >= 5, optimal obj increases by 1
      CHECK(lp.get_obj_value() == doctest::Approx(18.0));
    }
  }
}

TEST_CASE("SolverOptions - log_mode detailed writes log file")  // NOLINT
{
  const auto& reg = SolverRegistry::instance();
  const auto solvers = reg.available_solvers();
  REQUIRE(!solvers.empty());

  for (const auto& solver_name : solvers) {
    CAPTURE(solver_name);

    SUBCASE(std::string(solver_name).c_str())
    {
      const auto log_dir = std::filesystem::temp_directory_path()
          / std::format("gtopt_test_solver_logs_{}", solver_name);
      std::filesystem::remove_all(log_dir);
      std::filesystem::create_directories(log_dir);

      SUBCASE("initial_solve creates log file")
      {
        auto lp = make_barrier_test_lp(solver_name);
        const auto log_stem = (log_dir / "initial_solve").string();
        lp.set_log_file(log_stem);

        const SolverOptions opts {
            .algorithm = LPAlgo::barrier,
            .log_level = 1,
            .log_mode = SolverLogMode::detailed,
        };

        const auto result = lp.initial_solve(opts);
        CHECK(result.has_value());
        CHECK(lp.is_optimal());

        const auto log_path = std::format("{}.log", log_stem);
        REQUIRE(std::filesystem::exists(log_path));
        CHECK(std::filesystem::file_size(log_path) > 0);
      }

      SUBCASE("resolve creates log file")
      {
        auto lp = make_barrier_test_lp(solver_name);
        const auto log_stem = (log_dir / "resolve").string();
        lp.set_log_file(log_stem);

        // First do initial_solve without logging
        const SolverOptions init_opts {
            .algorithm = LPAlgo::barrier,
        };
        auto r1 = lp.initial_solve(init_opts);
        REQUIRE(r1.has_value());

        // Modify a bound and resolve with logging enabled
        lp.set_row_low(RowIndex {0}, 6.0);

        const SolverOptions resolve_opts {
            .algorithm = LPAlgo::dual,
            .log_level = 1,
            .log_mode = SolverLogMode::detailed,
        };

        const auto result = lp.resolve(resolve_opts);
        CHECK(result.has_value());
        CHECK(lp.is_optimal());
        CHECK(lp.get_obj_value() == doctest::Approx(18.0));

        const auto log_path = std::format("{}.log", log_stem);
        REQUIRE(std::filesystem::exists(log_path));
        CHECK(std::filesystem::file_size(log_path) > 0);
      }

      SUBCASE("nolog mode without log file does not create file")
      {
        auto lp = make_barrier_test_lp(solver_name);
        // Do NOT call set_log_file — nolog should produce no file

        const auto log_stem = (log_dir / "nolog").string();

        const SolverOptions opts {
            .algorithm = LPAlgo::barrier,
            .log_level = 1,
            .log_mode = SolverLogMode::nolog,
        };

        const auto result = lp.initial_solve(opts);
        CHECK(result.has_value());
        CHECK(lp.is_optimal());
        CHECK(lp.get_obj_value() == doctest::Approx(17.0));

        const auto log_path = std::format("{}.log", log_stem);
        CHECK_FALSE(std::filesystem::exists(log_path));
      }

      std::filesystem::remove_all(log_dir);
    }
  }
}

TEST_CASE("SolverOptions - query methods reflect applied options")  // NOLINT
{
  const auto& reg = SolverRegistry::instance();
  const auto solvers = reg.available_solvers();
  REQUIRE(!solvers.empty());

  for (const auto& solver_name : solvers) {
    CAPTURE(solver_name);

    SUBCASE(std::string(solver_name).c_str())
    {
      SUBCASE("barrier algorithm with custom settings")
      {
        auto lp = make_barrier_test_lp(solver_name);

        const SolverOptions opts {
            .algorithm = LPAlgo::barrier,
            .threads = 4,
            .presolve = false,
            .log_level = 2,
        };

        const auto result = lp.initial_solve(opts);
        CHECK(result.has_value());
        CHECK(lp.is_optimal());

        CHECK(lp.get_algorithm() == LPAlgo::barrier);
        CHECK(lp.get_threads() == 4);
        CHECK(lp.get_presolve() == false);
        CHECK(lp.get_log_level() == 2);
      }

      SUBCASE("dual algorithm with presolve enabled")
      {
        auto lp = make_barrier_test_lp(solver_name);

        const SolverOptions opts {
            .algorithm = LPAlgo::dual,
            .threads = 1,
            .presolve = true,
            .log_level = 0,
        };

        const auto result = lp.initial_solve(opts);
        CHECK(result.has_value());
        CHECK(lp.is_optimal());

        CHECK(lp.get_algorithm() == LPAlgo::dual);
        CHECK(lp.get_threads() == 1);
        CHECK(lp.get_presolve() == true);
        CHECK(lp.get_log_level() == 0);
      }

      SUBCASE("reuse_basis with dual overrides to dual, no presolve")
      {
        auto lp = make_barrier_test_lp(solver_name);

        // Initial solve with barrier
        const SolverOptions barrier_opts {
            .algorithm = LPAlgo::barrier,
            .threads = 2,
        };
        auto r1 = lp.initial_solve(barrier_opts);
        REQUIRE(r1.has_value());
        CHECK(lp.get_algorithm() == LPAlgo::barrier);

        // Resolve with reuse_basis + dual — should override to dual
        lp.set_row_low(RowIndex {0}, 6.0);

        const SolverOptions reuse_opts {
            .algorithm = LPAlgo::dual,
            .threads = 2,
            .presolve = true,
            .reuse_basis = true,
        };

        const auto result = lp.resolve(reuse_opts);
        CHECK(result.has_value());
        CHECK(lp.is_optimal());
        CHECK(lp.get_obj_value() == doctest::Approx(18.0));

        CHECK(lp.get_algorithm() == LPAlgo::dual);
        CHECK(lp.get_presolve() == false);
        CHECK(lp.get_threads() == 2);
      }

      SUBCASE("reuse_basis with barrier keeps barrier algorithm")
      {
        auto lp = make_barrier_test_lp(solver_name);

        // Initial solve with barrier
        const SolverOptions barrier_opts {
            .algorithm = LPAlgo::barrier,
            .threads = 2,
        };
        auto r1 = lp.initial_solve(barrier_opts);
        REQUIRE(r1.has_value());

        // Resolve with reuse_basis + barrier — should keep barrier
        lp.set_row_low(RowIndex {0}, 6.0);

        const SolverOptions reuse_opts {
            .algorithm = LPAlgo::barrier,
            .threads = 2,
            .presolve = true,
            .reuse_basis = true,
        };

        const auto result = lp.resolve(reuse_opts);
        CHECK(result.has_value());
        CHECK(lp.is_optimal());
        CHECK(lp.get_obj_value() == doctest::Approx(18.0));

        CHECK(lp.get_algorithm() == LPAlgo::barrier);
        CHECK(lp.get_presolve() == true);
        CHECK(lp.get_threads() == 2);
      }

      SUBCASE("options update on successive apply_options calls")
      {
        auto lp = make_barrier_test_lp(solver_name);

        // First solve with barrier
        const SolverOptions opts1 {
            .algorithm = LPAlgo::barrier,
            .threads = 4,
            .presolve = false,
            .log_level = 1,
        };
        auto r1 = lp.initial_solve(opts1);
        CHECK(r1.has_value());

        CHECK(lp.get_algorithm() == LPAlgo::barrier);
        CHECK(lp.get_threads() == 4);
        CHECK(lp.get_presolve() == false);
        CHECK(lp.get_log_level() == 1);

        // Then resolve with dual
        lp.set_row_low(RowIndex {0}, 6.0);

        const SolverOptions opts2 {
            .algorithm = LPAlgo::dual,
            .threads = 1,
            .presolve = true,
            .log_level = 0,
        };
        auto r2 = lp.resolve(opts2);
        CHECK(r2.has_value());

        CHECK(lp.get_algorithm() == LPAlgo::dual);
        CHECK(lp.get_threads() == 1);
        CHECK(lp.get_presolve() == true);
        CHECK(lp.get_log_level() == 0);
      }
    }
  }
}

TEST_CASE(
    "SolverOptions - max_fallbacks default and explicit values")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  SUBCASE("default is 2")
  {
    const SolverOptions opts {};
    CHECK(opts.max_fallbacks == 2);
  }

  SUBCASE("explicit zero disables fallback")
  {
    const SolverOptions opts {
        .max_fallbacks = 0,
    };
    CHECK(opts.max_fallbacks == 0);
  }

  SUBCASE("explicit one allows single fallback")
  {
    const SolverOptions opts {
        .max_fallbacks = 1,
    };
    CHECK(opts.max_fallbacks == 1);
  }
}

TEST_CASE(  // NOLINT
    "SolverOptions - PlanningOptionsLP applies SDDP max_fallbacks defaults")
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  SUBCASE("defaults: forward=2, backward=0")
  {
    PlanningOptions planning;
    PlanningOptionsLP plp(planning);

    const auto fwd = plp.sddp_forward_solver_options();
    CHECK(fwd.max_fallbacks == 2);

    const auto bwd = plp.sddp_backward_solver_options();
    CHECK(bwd.max_fallbacks == 0);
  }

  SUBCASE("explicit overrides from sddp_options")
  {
    PlanningOptions planning;
    planning.sddp_options.forward_max_fallbacks = 1;
    planning.sddp_options.backward_max_fallbacks = 2;

    PlanningOptionsLP plp(planning);

    const auto fwd = plp.sddp_forward_solver_options();
    CHECK(fwd.max_fallbacks == 1);

    const auto bwd = plp.sddp_backward_solver_options();
    CHECK(bwd.max_fallbacks == 2);
  }

  SUBCASE("global max_fallbacks inherited by forward when not overridden")
  {
    PlanningOptions planning;
    planning.solver_options.max_fallbacks = 1;
    // No SDDP-specific override for forward

    PlanningOptionsLP plp(planning);

    const auto fwd = plp.sddp_forward_solver_options();
    CHECK(fwd.max_fallbacks == 1);

    // backward still defaults to 0
    const auto bwd = plp.sddp_backward_solver_options();
    CHECK(bwd.max_fallbacks == 0);
  }
}
