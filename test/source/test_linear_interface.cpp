/**
 * @file      test_linear_interface.hpp
 * @brief     Tests for the LinearInterface class
 * @date      Sat Apr 19 12:45:00 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <cmath>
#include <expected>
#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/error.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/low_memory_snapshot.hpp>
#include <gtopt/memory_compress.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("LinearInterface - Constructor and basic operations")
{
  using namespace gtopt;

  // Create a basic interface
  LinearInterface interface;

  // Add some columns and rows
  const auto col1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
  });
  const auto col2 = interface.add_col(SparseCol {
      .uppb = 5.0,
      .cost = 3.0,
  });
  const auto col3 = interface.add_col(SparseCol {}.free());

  // Check dimensions
  REQUIRE(interface.get_numcols() == 3);
  REQUIRE(interface.get_numrows() == 0);

  // Check column bounds
  auto col_low = interface.get_col_low();
  auto col_upp = interface.get_col_upp();

  REQUIRE(col_low[col1] == doctest::Approx(0.0));
  REQUIRE(col_upp[col1] == doctest::Approx(10.0));
  REQUIRE(col_low[col2] == doctest::Approx(0.0));
  REQUIRE(col_upp[col2] == doctest::Approx(5.0));
  REQUIRE(col_low[col3] <= -1.0e20);  // Free columns have large negative bound
  REQUIRE(col_upp[col3] >= 1.0e20);  // Free columns have large positive bound

  // Test objective coefficients set via SparseCol
  REQUIRE(interface.get_obj_coeff()[col1] == doctest::Approx(2.0));
  REQUIRE(interface.get_obj_coeff()[col2] == doctest::Approx(3.0));
  REQUIRE(interface.get_obj_coeff()[col3] == doctest::Approx(0.0));

  // Test adding constraints
  SparseRow row1;
  row1[col1] = 1.0;
  row1[col2] = 2.0;
  row1.lowb = 0.0;
  row1.uppb = 15.0;

  const auto r1 = interface.add_row(row1);
  REQUIRE(interface.get_numrows() == 1);

  // Test getting row bounds
  auto row_low = interface.get_row_low();
  auto row_upp = interface.get_row_upp();

  REQUIRE(row_low[r1] == doctest::Approx(0.0));
  REQUIRE(row_upp[r1] == doctest::Approx(15.0));
}

TEST_CASE("LinearInterface - LP solution")
{
  using namespace gtopt;

  // Create interface with a simple LP problem
  // Maximize 3x1 + 2x2
  // Subject to:
  //   x1 + 2x2 <= 10
  //   x1 <= 4
  //   x2 <= 3
  //   x1, x2 >= 0

  LinearInterface interface;

  // Add variables
  const auto x1 = interface.add_col(SparseCol {
      .uppb = 4.0,
      .cost = -3.0,
  });
  const auto x2 = interface.add_col(SparseCol {
      .uppb = 3.0,
      .cost = -2.0,
  });

  // Add constraint: x1 + 2x2 <= 10
  SparseRow row1;
  row1[x1] = 1.0;
  row1[x2] = 2.0;
  row1.uppb = 10.0;
  (void)interface.add_row(row1);  // NOLINT

  // Solve the problem
  const SolverOptions options;

  auto result = interface.initial_solve(options);
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);  // 0 = optimal

  // Check the solution (should be x1=4, x2=3)
  auto sol = interface.get_col_sol();
  REQUIRE(sol[x1] == doctest::Approx(4.0));
  REQUIRE(sol[x2] == doctest::Approx(3.0));

  // Check objective value: 3*4 + 2*3 = 18
  const double obj_val =
      -interface.get_obj_value_raw();  // Negate back to get maximization value
  REQUIRE(obj_val == doctest::Approx(18.0));
}

TEST_CASE("LinearInterface - Variable types and bounds")
{
  using namespace gtopt;

  LinearInterface interface;

  // Add different types of variables
  const auto continuous_var = interface.add_col(SparseCol {
      .uppb = 10.0,
  });
  const auto integer_var = interface.add_col(SparseCol {
      .uppb = 10.0,
  });
  const auto binary_var = interface.add_col(SparseCol {
      .uppb = 1.0,
  });

  // Set variable types
  interface.set_continuous(continuous_var);
  interface.set_integer(integer_var);
  interface.set_binary(binary_var);

  // Check variable types
  REQUIRE(interface.is_continuous(continuous_var));
  REQUIRE(!interface.is_integer(continuous_var));

  REQUIRE(!interface.is_continuous(integer_var));
  REQUIRE(interface.is_integer(integer_var));

  REQUIRE(!interface.is_continuous(binary_var));
  REQUIRE(interface.is_integer(binary_var));

  // Test changing bounds
  interface.set_col_low(continuous_var, 2.0);
  interface.set_col_upp(continuous_var, 8.0);

  auto col_low = interface.get_col_low();
  auto col_upp = interface.get_col_upp();

  REQUIRE(col_low[continuous_var] == doctest::Approx(2.0));
  REQUIRE(col_upp[continuous_var] == doctest::Approx(8.0));
}

TEST_CASE(  // NOLINT
    "LinearInterface - advanced_basis warm-start after a column-bound change")
{
  using namespace gtopt;

  // This is exactly what an SDDP aperture re-solve does: change only
  // column bounds, then re-optimize off the resident basis with a warm
  // simplex solve (`SolverOptions::advanced_basis`).  Model:
  //   maximize 3 x1 + 2 x2  s.t.  x1 + 2 x2 <= 10,  0<=x1<=U,  0<=x2<=3
  // (stored as a minimization of -3 x1 - 2 x2).
  auto add_problem = [](LinearInterface& iface)
  {
    const auto x1 = iface.add_col(SparseCol {.uppb = 4.0, .cost = -3.0});
    const auto x2 = iface.add_col(SparseCol {.uppb = 3.0, .cost = -2.0});
    SparseRow row1;
    row1[x1] = 1.0;
    row1[x2] = 2.0;
    row1.uppb = 10.0;
    (void)iface.add_row(row1);  // NOLINT
    return std::pair {x1, x2};
  };

  // ── Reference: cold solve of the LP with x1 capped at 2.0 ──
  double cold_obj = 0.0;
  {
    LinearInterface iface;
    const auto [x1, x2] = add_problem(iface);
    iface.set_col_upp(x1, 2.0);
    const auto r = iface.initial_solve(SolverOptions {});
    REQUIRE(r.has_value());
    REQUIRE(r.value() == 0);
    cold_obj = iface.get_obj_value_raw();
  }
  // x1=2 → x2=min((10-2)/2, 3)=3 → obj = 3·2 + 2·3 = 12 (raw -12).
  REQUIRE(cold_obj == doctest::Approx(-12.0));

  // ── Warm path: seed a basis at x1<=4, change the bound, warm-resolve ──
  LinearInterface iface;
  const auto [x1, x2] = add_problem(iface);

  // Cold seed solve — leaves an optimal basis resident on the backend.
  const auto r0 = iface.initial_solve(SolverOptions {});
  REQUIRE(r0.has_value());
  REQUIRE(r0.value() == 0);
  // x1=4, x2=3 → obj 18 (raw -18).
  REQUIRE(iface.get_obj_value_raw() == doctest::Approx(-18.0));

  // The "aperture delta": tighten only a column bound.
  iface.set_col_upp(x1, 2.0);

  // Warm re-solve off the resident basis: ADVIND=1 + primal simplex on
  // CPLEX; on backends without `advanced_basis` it degrades to a normal
  // solve.  Either way the optimum must match the cold reference — a
  // bound change keeps the basis valid (it stays dual-feasible).
  SolverOptions warm;
  warm.advanced_basis = true;
  warm.algorithm = LPAlgo::primal;
  const auto r1 = iface.resolve(warm);
  REQUIRE(r1.has_value());
  REQUIRE(r1.value() == 0);

  CHECK(iface.get_obj_value_raw() == doctest::Approx(cold_obj));
  const auto sol = iface.get_col_sol();
  CHECK(sol[x1] == doctest::Approx(2.0));
  CHECK(sol[x2] == doctest::Approx(3.0));
}

TEST_CASE(  // NOLINT
    "LinearInterface - warm-start across a chunk of aperture-style bound "
    "changes")
{
  using namespace gtopt;

  // Faithful unit-level model of the SDDP aperture chunk loop: ONE LP
  // ("the clone"), solved cold once to seed a basis, then a sequence of
  // "apertures" — each a column-bound delta — re-solved WARM off the
  // resident basis.  Mirrors `solve_apertures_for_phase`: aperture 0 is
  // the cold seed, apertures 1..K-1 warm-start.  Same model as above:
  //   maximize 3 x1 + 2 x2  s.t.  x1 + 2 x2 <= 10,  0<=x1<=cap, 0<=x2<=3.
  // Per unit of the constraint x1 yields 3 vs x2's 1, so the optimum is
  // x1=min(cap,10), x2=min((10-x1)/2, 3).
  LinearInterface iface;
  const auto x1 = iface.add_col(SparseCol {.uppb = 4.0, .cost = -3.0});
  const auto x2 = iface.add_col(SparseCol {.uppb = 3.0, .cost = -2.0});
  SparseRow row1;
  row1[x1] = 1.0;
  row1[x2] = 2.0;
  row1.uppb = 10.0;
  (void)iface.add_row(row1);  // NOLINT

  // The four "apertures" (distinct x1 caps) and their analytic optima
  // (raw = negated maximization objective).
  struct Ap
  {
    double cap;
    double raw_obj;
  };
  const std::array<Ap, 4> apertures {{
      {.cap = 4.0, .raw_obj = -18.0},  // x1=4, x2=3
      {.cap = 2.0, .raw_obj = -12.0},  // x1=2, x2=3
      {.cap = 1.0, .raw_obj = -9.0},  // x1=1, x2=3
      {.cap = 3.0, .raw_obj = -15.0},  // x1=3, x2=3
  }};

  SolverOptions warm;
  warm.advanced_basis = true;
  warm.algorithm = LPAlgo::primal;

  bool seeded = false;
  for (const auto& ap : apertures) {
    iface.set_col_upp(x1, ap.cap);  // apply the aperture bound delta

    // First aperture = cold seed (no resident basis yet); the rest
    // warm-start off the basis left by the previous aperture — exactly
    // the chunk loop's `clone_has_basis` gate.
    const auto r =
        seeded ? iface.resolve(warm) : iface.initial_solve(SolverOptions {});
    seeded = true;
    REQUIRE(r.has_value());
    REQUIRE(r.value() == 0);
    CHECK(iface.get_obj_value_raw() == doctest::Approx(ap.raw_obj));

    // Cross-check against a fresh COLD solve of the same bounded LP: the
    // warm re-solve must reproduce the cold optimum exactly (a bound
    // change keeps the basis valid; the optimum is unchanged).
    LinearInterface cold;
    const auto cx1 = cold.add_col(SparseCol {.uppb = ap.cap, .cost = -3.0});
    const auto cx2 = cold.add_col(SparseCol {.uppb = 3.0, .cost = -2.0});
    SparseRow crow;
    crow[cx1] = 1.0;
    crow[cx2] = 2.0;
    crow.uppb = 10.0;
    (void)cold.add_row(crow);  // NOLINT
    const auto cr = cold.initial_solve(SolverOptions {});
    REQUIRE(cr.has_value());
    CHECK(iface.get_obj_value_raw()
          == doctest::Approx(cold.get_obj_value_raw()));
  }
}

TEST_CASE(  // NOLINT
    "LinearInterface - advanced_basis warm-start across every available solver")
{
  using namespace gtopt;

  // `advanced_basis` is a cross-solver primitive: every backend maps it to
  // its native warm-start (CPLEX ADVIND+simplex, HiGHS simplex+no-presolve,
  // Gurobi Method=simplex, MindOpt Method+no-presolve, OSI/CLP resolve()
  // hint).  Exercise the cold-seed → bound-change → warm-resolve sequence
  // on EVERY loaded solver plugin and assert the warm path reproduces the
  // cold optimum.  Model: maximize 3 x1 + 2 x2  s.t.  x1 + 2 x2 <= 10,
  // x1<=cap, x2<=3 (stored as minimize -3 x1 - 2 x2).
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  const auto solvers = reg.available_solvers();
  REQUIRE_FALSE(solvers.empty());

  int tested = 0;
  for (const auto& name : solvers) {
    CAPTURE(name);

    LinearInterface iface(name);
    const auto x1 = iface.add_col(SparseCol {.uppb = 4.0, .cost = -3.0});
    const auto x2 = iface.add_col(SparseCol {.uppb = 3.0, .cost = -2.0});
    SparseRow row1;
    row1[x1] = 1.0;
    row1[x2] = 2.0;
    row1.uppb = 10.0;
    (void)iface.add_row(row1);  // NOLINT

    // Cold seed.  A solver that is present but not usable here (e.g. an
    // unlicensed commercial backend) fails to reach optimal — skip it.
    const auto r0 = iface.initial_solve(SolverOptions {});
    if (!r0.has_value() || r0.value() != 0) {
      MESSAGE("solver '" << name << "' cold solve unavailable — skipping");
      continue;
    }
    ++tested;
    CHECK(iface.get_obj_value_raw() == doctest::Approx(-18.0));

    // Aperture-style bound delta + warm re-solve via advanced_basis.  The
    // warm path must reproduce the cold optimum on every backend (a bound
    // change keeps the basis valid; backends that cannot warm-start degrade
    // to a normal solve — still correct).
    iface.set_col_upp(x1, 2.0);
    SolverOptions warm;
    warm.advanced_basis = true;
    warm.algorithm = LPAlgo::primal;
    const auto r1 = iface.resolve(warm);
    REQUIRE(r1.has_value());
    CHECK(r1.value() == 0);
    CHECK(iface.get_obj_value_raw() == doctest::Approx(-12.0));
  }
  // At least one real solver must have exercised the warm path.
  REQUIRE(tested >= 1);
}

TEST_CASE("LinearInterface - relax_integers flips MIP columns to continuous")
{
  using namespace gtopt;

  LinearInterface interface;

  // Build a tiny mixed problem: one continuous, one integer, one binary.
  const auto cont_var = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });
  const auto int_var = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });
  const auto bin_var = interface.add_col(SparseCol {
      .uppb = 1.0,
      .cost = 1.0,
  });

  interface.set_continuous(cont_var);
  interface.set_integer(int_var);
  interface.set_binary(bin_var);

  REQUIRE(interface.is_continuous(cont_var));
  REQUIRE(interface.is_integer(int_var));
  REQUIRE(interface.is_integer(bin_var));

  SUBCASE("relaxes every integer column in a single pass")
  {
    const auto relaxed = interface.relax_integers();
    CHECK(relaxed == 2);

    // All columns are now continuous; previously-continuous column
    // is left untouched.
    CHECK(interface.is_continuous(cont_var));
    CHECK(interface.is_continuous(int_var));
    CHECK(interface.is_continuous(bin_var));

    CHECK(!interface.is_integer(int_var));
    CHECK(!interface.is_integer(bin_var));
  }

  SUBCASE("idempotent — second call is a no-op")
  {
    const auto first = interface.relax_integers();
    CHECK(first == 2);

    const auto second = interface.relax_integers();
    CHECK(second == 0);

    CHECK(interface.is_continuous(int_var));
    CHECK(interface.is_continuous(bin_var));
  }
}

TEST_CASE("LinearInterface - restore_integers round-trips relax_integers")
{
  using namespace gtopt;

  // Exercises the mip-start relax→restore path (mip_start.cpp): snapshot
  // the integer columns, relax to a continuous LP, then re-impose
  // integrality on exactly the snapshotted columns.  Pure model
  // manipulation (no solve) so it needs no MIP solver.
  LinearInterface interface;
  const auto cont_var =
      interface.add_col(SparseCol {.uppb = 10.0, .cost = 1.0});
  const auto int_var = interface.add_col(SparseCol {.uppb = 10.0, .cost = 1.0});
  const auto bin_var = interface.add_col(SparseCol {.uppb = 1.0, .cost = 1.0});
  interface.set_continuous(cont_var);
  interface.set_integer(int_var);
  interface.set_binary(bin_var);

  // Snapshot integer columns BEFORE relaxing (relaxation hides them) —
  // mirrors `apply_mip_start`'s int_cols capture.
  std::vector<int> int_cols;
  for (int i = 0; i < static_cast<int>(interface.get_numcols()); ++i) {
    if (interface.is_integer(ColIndex {i})) {
      int_cols.push_back(i);
    }
  }
  REQUIRE(int_cols.size() == 2);

  REQUIRE(interface.relax_integers() == 2);
  REQUIRE(interface.is_continuous(int_var));
  REQUIRE(interface.is_continuous(bin_var));

  const auto restored = interface.restore_integers(int_cols);
  CHECK(restored == 2);
  CHECK(interface.is_integer(int_var));
  CHECK(interface.is_integer(bin_var));
  // The originally-continuous column is left continuous.
  CHECK(interface.is_continuous(cont_var));

  SUBCASE("empty span restores nothing")
  {
    interface.relax_integers();
    const auto none = interface.restore_integers(std::span<const int> {});
    CHECK(none == 0);
    CHECK(interface.is_continuous(int_var));
    CHECK(interface.is_continuous(bin_var));
  }
}

TEST_CASE("LinearInterface - relax_integers on a pure LP is a no-op")
{
  using namespace gtopt;

  LinearInterface interface;

  (void)interface.add_col(SparseCol {
      .uppb = 5.0,
      .cost = 1.0,
  });
  (void)interface.add_col(SparseCol {
      .uppb = 5.0,
      .cost = 1.0,
  });

  // Neither column has been flagged as integer — relaxation finds
  // nothing to do and returns 0 without touching the backend's
  // column-type state.
  const auto relaxed = interface.relax_integers();
  CHECK(relaxed == 0);
}

TEST_CASE(
    "LinearInterface - relax_integers uses backend bulk API on every solver")
{
  // End-to-end check that the active backend's `relax_all_integers()`
  // override (or the default fallback) correctly clears integrality
  // on every column.  This exercises whichever backend the build is
  // linking — the ctest matrix already runs the test binary once per
  // solver plugin via the test_solver_* / test_mip_solvers pattern,
  // so each invocation lands on a different backend and this doctest
  // covers the active one.  We deliberately do NOT iterate solver
  // backends here.
  using namespace gtopt;

  LinearInterface interface;

  const auto a = interface.add_col(SparseCol {
      .uppb = 7.0,
      .cost = 1.0,
  });
  const auto b = interface.add_col(SparseCol {
      .uppb = 4.0,
      .cost = 1.0,
  });
  const auto c = interface.add_col(SparseCol {
      .uppb = 1.0,
      .cost = 1.0,
  });

  // Flag every column as integer/binary so the bulk relaxation path
  // has work to do on whichever backend is linked.
  interface.set_integer(a);
  interface.set_integer(b);
  interface.set_binary(c);

  REQUIRE(interface.is_integer(a));
  REQUIRE(interface.is_integer(b));
  REQUIRE(interface.is_integer(c));

  // One call into the bulk virtual.  Default fallback or native
  // single-call API — both must end in a fully-continuous LP.
  const auto relaxed = interface.relax_integers();
  CHECK(relaxed == 3);

  CHECK(interface.is_continuous(a));
  CHECK(interface.is_continuous(b));
  CHECK(interface.is_continuous(c));

  CHECK(!interface.is_integer(a));
  CHECK(!interface.is_integer(b));
  CHECK(!interface.is_integer(c));
}

TEST_CASE("LinearInterface - LP file output")
{
  using namespace gtopt;

  // Create simple LP with names so both col and row names are tracked.
  LinearInterface interface;
  interface.set_label_maker(LabelMaker {LpNamesLevel::all});

  // Add variables using SparseCol with class_name/variable_name so
  // `LinearInterface::generate_labels_from_maps` can synthesise
  // real labels at write_lp time.  SparseCols added without a
  // `class_name` deliberately throw under the on-demand labelling
  // contract (no generic `c<i>`/`r<i>` fallback).
  (void)interface.add_col(SparseCol {
      // NOLINT
      .lowb = 0.0,
      .uppb = 10.0,
      .class_name = "V",
      .variable_name = "x",
      .variable_uid = Uid {0},
  });
  (void)interface.add_col(SparseCol {
      // NOLINT
      .lowb = 0.0,
      .uppb = 10.0,
      .class_name = "V",
      .variable_name = "x",
      .variable_uid = Uid {1},
  });

  // Add constraint using SparseRow API
  SparseRow row;
  row[ColIndex {0}] = 1.0;
  row[ColIndex {1}] = 1.0;
  row.lowb = 0.0;
  row.uppb = 15.0;
  row.class_name = "C";
  row.constraint_name = "1";
  row.variable_uid = Uid {0};
  row.context = make_stage_context(make_uid<Scenario>(0), make_uid<Stage>(0));
  (void)interface.add_row(row);  // NOLINT

  // Set problem name
  interface.set_prob_name("TestLP");

  // Write LP to file
  const std::string temp_file = "test_interface_lp";
  auto result = interface.write_lp(temp_file);
  REQUIRE(result.has_value());

  // Verify file was created
  const std::string lp_file = temp_file + ".lp";
  const bool file_exists = std::filesystem::exists(lp_file);
  if (file_exists) {
    std::filesystem::remove(lp_file);  // Clean up
  }
  REQUIRE(file_exists);
}

TEST_CASE("LinearInterface - Loading from FlatLinearProblem")
{
  using namespace gtopt;

  // Create linear problem
  LinearProblem lp("TestLP");

  // Add columns
  const auto col1 = lp.add_col({.lowb = 0.0, .uppb = 10.0, .cost = 2.0});
  const auto col2 = lp.add_col({.lowb = 0.0, .uppb = 10.0, .cost = 3.0});

  // Add row
  const auto row1 = lp.add_row({.uppb = 15.0});

  // Set coefficients
  lp.set_coeff(row1, col1, 1.0);
  lp.set_coeff(row1, col2, 1.0);

  // Convert to flat format
  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  auto flat_lp = lp.flatten(flat_opts);

  // Create interface and load the problem
  LinearInterface interface;
  interface.load_flat(flat_lp);

  // Verify dimensions
  REQUIRE(interface.get_numcols() == 2);
  REQUIRE(interface.get_numrows() == 1);

  // Verify objective coefficients
  REQUIRE(interface.get_obj_coeff()[0] == doctest::Approx(2.0));
  REQUIRE(interface.get_obj_coeff()[1] == doctest::Approx(3.0));

  // Solve the problem
  auto result = interface.initial_solve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);  // 0 = optimal

  // Get solution
  const auto sol = interface.get_col_sol();

  // For this simple problem, both variables should be at their lower bounds (0)
  // since we're minimizing and they have positive objective coefficients
  REQUIRE(sol[0] == doctest::Approx(0.0));
  REQUIRE(sol[1] == doctest::Approx(0.0));
}

TEST_CASE("LinearInterface - EPOPT optimality tolerance folds scale_objective")
{
  using namespace gtopt;

  // The reduced-cost optimality tolerance (CPLEX EPOPT) is applied by the
  // backend in RAW LP-objective space.  flatten() divides the objective by
  // scale_objective, so rc_raw = rc_phys / scale_objective.  To keep the
  // PHYSICAL reduced-cost floor invariant (base 1e-6 at scale_objective == 1),
  // initial_solve()/resolve() must set the raw tolerance to
  // 1e-6 / scale_objective.  See scale_optimality_tol_for_objective.
  const auto build_and_solve =
      [](double scale_obj,
         std::optional<double> user_opt_eps) -> std::optional<double>
  {
    LinearProblem lp("EpoptScaleLP");
    const auto col1 = lp.add_col({.lowb = 0.0, .uppb = 10.0, .cost = 2.0});
    const auto col2 = lp.add_col({.lowb = 0.0, .uppb = 10.0, .cost = 3.0});
    const auto row1 = lp.add_row({.lowb = 5.0, .uppb = 15.0});
    lp.set_coeff(row1, col1, 1.0);
    lp.set_coeff(row1, col2, 1.0);

    LpMatrixOptions flat_opts;
    flat_opts.scale_objective = scale_obj;
    auto flat_lp = lp.flatten(flat_opts);

    LinearInterface interface;
    interface.load_flat(flat_lp);
    REQUIRE(interface.scale_objective() == doctest::Approx(scale_obj));

    SolverOptions opts;
    opts.optimal_eps = user_opt_eps;  // physical (scale_obj == 1) tolerance
    auto result = interface.initial_solve(opts);
    REQUIRE(result.has_value());
    return interface.last_solver_options().optimal_eps;
  };

  SUBCASE("scale_objective == 1 leaves the tolerance unscaled")
  {
    // No-op: helper returns early, so optimal_eps stays as the user passed
    // it (here unset → backend default).
    const auto eps = build_and_solve(1.0, std::nullopt);
    CHECK_FALSE(eps.has_value());
  }

  SUBCASE("scale_objective == 1000 → raw EPOPT = 1e-6 / 1000 = 1e-9")
  {
    const auto eps = build_and_solve(1000.0, std::nullopt);
    REQUIRE(eps.has_value());
    CHECK(eps.value_or(-1.0) == doctest::Approx(1e-9));
  }

  SUBCASE("explicit physical optimal_eps is divided by scale_objective")
  {
    // user 1e-4 physical / 100 = 1e-6 raw (inside [1e-9, 1e-1]).
    const auto eps = build_and_solve(100.0, 1e-4);
    CHECK(eps.value_or(-1.0) == doctest::Approx(1e-6));
  }

  SUBCASE("scaled tolerance is clamped to the EPOPT floor 1e-9")
  {
    // user 1e-6 physical / 1e6 = 1e-12 → clamped up to 1e-9.
    const auto eps = build_and_solve(1.0e6, 1e-6);
    CHECK(eps.value_or(-1.0) == doctest::Approx(1e-9));
  }
}

TEST_CASE("LinearInterface - get/set problem name")
{
  using namespace gtopt;

  LinearInterface interface;
  interface.set_prob_name("TestProblem");
  CHECK(interface.get_prob_name() == "TestProblem");
}

TEST_CASE("LinearInterface - set_col and set_rhs")
{
  using namespace gtopt;

  LinearInterface interface;

  const auto col1 = interface.add_col(SparseCol {
      .uppb = 10.0,
  });
  interface.set_col(col1, 5.0);

  auto col_low = interface.get_col_low();
  auto col_upp = interface.get_col_upp();

  CHECK(col_low[col1] == doctest::Approx(5.0));
  CHECK(col_upp[col1] == doctest::Approx(5.0));

  // Add a row and test set_rhs
  SparseRow row;
  row[col1] = 1.0;
  row.lowb = 0.0;
  row.uppb = 10.0;
  const auto r1 = interface.add_row(row);

  interface.set_row_equal_to(r1, 7.0);

  auto row_low = interface.get_row_low();
  auto row_upp = interface.get_row_upp();

  CHECK(row_low[r1] == doctest::Approx(7.0));
  CHECK(row_upp[r1] == doctest::Approx(7.0));
}

TEST_CASE("LinearInterface - add_col without bounds (default)")
{
  using namespace gtopt;

  LinearInterface interface;

  const auto col = interface.add_col(SparseCol {});

  auto col_low = interface.get_col_low();
  auto col_upp = interface.get_col_upp();

  CHECK(col_low[col] == doctest::Approx(0.0));
  CHECK(interface.is_pos_inf(col_upp[col]));
}

TEST_CASE("LinearInterface - row bounds modification")
{
  using namespace gtopt;

  LinearInterface interface;

  const auto col = interface.add_col(SparseCol {
      .uppb = 10.0,
  });

  SparseRow row;
  row[col] = 1.0;
  row.lowb = 0.0;
  row.uppb = 10.0;
  const auto r1 = interface.add_row(row);

  interface.set_row_low(r1, 2.0);
  interface.set_row_upp(r1, 8.0);

  auto row_low = interface.get_row_low();
  auto row_upp = interface.get_row_upp();

  CHECK(row_low[r1] == doctest::Approx(2.0));
  CHECK(row_upp[r1] == doctest::Approx(8.0));
}

TEST_CASE("LinearInterface - write LP to empty filename (no-op)")
{
  using namespace gtopt;

  LinearInterface interface;
  (void)interface.add_col(SparseCol {
      // NOLINT
      .uppb = 10.0,
  });

  // Writing with empty filename should succeed (no-op)
  CHECK(interface.write_lp("").has_value());
}

TEST_CASE("LinearInterface - solve with solver options (dual algorithm)")
{
  using namespace gtopt;

  LinearInterface interface;

  const auto x1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
  });
  const auto x2 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 3.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.uppb = 12.0;
  (void)interface.add_row(row);  // NOLINT

  SolverOptions opts;
  opts.algorithm = LPAlgo::dual;
  opts.presolve = true;

  auto result = interface.initial_solve(opts);
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("LinearInterface - solve with primal algorithm")
{
  using namespace gtopt;

  LinearInterface interface;

  const auto x1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.uppb = 5.0;
  (void)interface.add_row(row);  // NOLINT

  SolverOptions opts;
  opts.algorithm = LPAlgo::primal;

  auto result = interface.initial_solve(opts);
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("LinearInterface - solver options with tolerances")
{
  using namespace gtopt;

  LinearInterface interface;

  const auto x1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.uppb = 5.0;
  (void)interface.add_row(row);  // NOLINT

  SolverOptions opts;
  opts.optimal_eps = 1e-8;
  opts.feasible_eps = 1e-8;

  auto result = interface.initial_solve(opts);
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("LinearInterface - get_status and status checks")
{
  using namespace gtopt;

  LinearInterface interface;

  const auto x1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.uppb = 5.0;
  (void)interface.add_row(row);  // NOLINT

  auto result = interface.resolve();
  REQUIRE(result.has_value());

  CHECK(interface.is_optimal());
  CHECK_FALSE(interface.is_dual_infeasible());
  CHECK_FALSE(interface.is_prim_infeasible());
  CHECK(interface.get_status() == 0);
  // CLP's largestDualError() may return 0 for clean solves.
  // Backends without kappa support return std::nullopt (e.g. MindOpt).
  if (const auto kappa0 = interface.get_kappa(); kappa0.has_value()) {
    CHECK(*kappa0 >= 0.0);
  }
}

TEST_CASE("LinearInterface - get_kappa returns meaningful condition number")
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Build a problem with a non-trivial basis matrix to get kappa > 1.
  // Use coefficients with different magnitudes to create a condition
  // number that is clearly not the default 1.0.
  //
  // min -x1 - 2*x2 - x3
  // s.t. 1000*x1 +    x2 +    x3 <= 5000
  //         x1 + 1000*x2 +    x3 <= 5000
  //         x1 +    x2 + 1000*x3 <= 5000
  //      0 <= x1,x2,x3 <= 100
  LinearInterface interface;

  const auto x1 = interface.add_col(SparseCol {
      .uppb = 100.0,
      .cost = -1.0,
  });
  const auto x2 = interface.add_col(SparseCol {
      .uppb = 100.0,
      .cost = -2.0,
  });
  const auto x3 = interface.add_col(SparseCol {
      .uppb = 100.0,
      .cost = -1.0,
  });

  SparseRow r1;
  r1[x1] = 1000.0;
  r1[x2] = 1.0;
  r1[x3] = 1.0;
  r1.uppb = 5000.0;
  (void)interface.add_row(r1);  // NOLINT

  SparseRow r2;
  r2[x1] = 1.0;
  r2[x2] = 1000.0;
  r2[x3] = 1.0;
  r2.uppb = 5000.0;
  (void)interface.add_row(r2);  // NOLINT

  SparseRow r3;
  r3[x1] = 1.0;
  r3[x2] = 1.0;
  r3[x3] = 1000.0;
  r3.uppb = 5000.0;
  (void)interface.add_row(r3);  // NOLINT

  auto result = interface.initial_solve();
  REQUIRE(result.has_value());
  CHECK(interface.is_optimal());

  const auto kappa = interface.get_kappa();
  // CLP uses largestDualError() which may be 0 for clean solves.
  // Backends without kappa support return std::nullopt (e.g. MindOpt).
  if (kappa.has_value()) {
    CHECK(*kappa >= 0.0);
  }

  // Log the kappa value for diagnostics
  MESSAGE("Solver kappa = ", kappa.value_or(-1.0));
}

TEST_CASE("LinearInterface - get_kappa with explicit solver")
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Test kappa with each available solver backend.
  // HiGHS uses Highs::getKappa(exact=true) which computes the actual
  // condition number. CLP/CBC uses largestDualError() as a proxy
  // (conditionNumber() is unavailable in multi-factorization builds).
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  for (const auto& solver : reg.available_solvers()) {
    CAPTURE(solver);
    try {
      LinearInterface interface(solver);

      const auto x1 = interface.add_col(SparseCol {
          .uppb = 100.0,
          .cost = -1.0,
      });
      const auto x2 = interface.add_col(SparseCol {
          .uppb = 100.0,
          .cost = -2.0,
      });
      const auto x3 = interface.add_col(SparseCol {
          .uppb = 100.0,
          .cost = -1.0,
      });

      SparseRow r1;
      r1[x1] = 1000.0;
      r1[x2] = 1.0;
      r1[x3] = 1.0;
      r1.uppb = 5000.0;
      (void)interface.add_row(r1);  // NOLINT

      SparseRow r2;
      r2[x1] = 1.0;
      r2[x2] = 1000.0;
      r2[x3] = 1.0;
      r2.uppb = 5000.0;
      (void)interface.add_row(r2);  // NOLINT

      SparseRow r3;
      r3[x1] = 1.0;
      r3[x2] = 1.0;
      r3[x3] = 1000.0;
      r3.uppb = 5000.0;
      (void)interface.add_row(r3);  // NOLINT

      auto result = interface.initial_solve();
      REQUIRE(result.has_value());
      CHECK(interface.is_optimal());

      const auto kappa = interface.get_kappa();
      // Backends without kappa support return std::nullopt (e.g. MindOpt).
      if (kappa.has_value()) {
        CHECK(*kappa >= 0.0);
      }
      MESSAGE(solver, " kappa = ", kappa.value_or(-1.0));
    } catch (const std::exception& e) {
      MESSAGE("Solver '", solver, "' not available: ", e.what());
    }
  }
}

// Regression test for the sentinel-mismatch bug (see CLAUDE.md feedback
// "no-nan-values"): when CPXgetdblquality / Highs::getKappa / CLP's proxy
// fail, the backend must propagate std::nullopt and NEVER return a literal
// 1.0 that would silently poison std::max-based aggregation across the
// (scene, phase) grid.  The stat aggregator in LinearInterface must also
// skip nullopt rather than fold a sentinel into max_kappa.
TEST_CASE(  // NOLINT
    "LinearInterface - get_kappa never returns the 1.0 sentinel silently")
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  for (const auto& solver : reg.available_solvers()) {
    CAPTURE(solver);
    try {
      LinearInterface interface(solver);

      // Trivial 1-variable LP: min x s.t. x >= 0, x <= 10.  Any sane
      // solver either produces a real (non-sentinel) kappa or returns
      // nullopt; a literal 1.0 would only come from the old failed-query
      // fallback path, which the fix removes.
      const auto x = interface.add_col(SparseCol {
          .uppb = 10.0,
          .cost = 1.0,
      });
      SparseRow row;
      row[x] = 1.0;
      row.uppb = 8.0;
      (void)interface.add_row(row);  // NOLINT

      auto result = interface.initial_solve();
      REQUIRE(result.has_value());
      REQUIRE(interface.is_optimal());

      const auto kappa = interface.get_kappa();
      if (kappa.has_value()) {
        // A computed kappa must be finite and non-negative.  It MAY
        // coincidentally be ~1.0 for a 1x1 well-conditioned basis, so
        // we don't forbid that value exactly — we only forbid the
        // previously-silent poisoning where a failed query masqueraded
        // as a computed value.
        CHECK(std::isfinite(*kappa));
        CHECK(*kappa >= 0.0);
      }

      // Aggregation check: SolverStats::max_kappa must remain at its
      // -1 sentinel when the backend does not supply a value, never
      // drift up to a fake 1.0.
      const auto& stats = interface.solver_stats();
      if (!kappa.has_value()) {
        CHECK(stats.max_kappa < 0.0);
      } else {
        CHECK(stats.max_kappa == doctest::Approx(*kappa));
      }
    } catch (const std::exception& e) {
      MESSAGE("Solver '", solver, "' not available: ", e.what());
    }
  }
}

TEST_CASE("LinearInterface - set_col_sol and set_row_dual")
{
  using namespace gtopt;

  LinearInterface interface;

  const auto x1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });
  const auto x2 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.uppb = 8.0;
  (void)interface.add_row(row);  // NOLINT

  // Set initial solution
  std::vector<double> sol = {3.0, 4.0};
  interface.set_col_sol(sol);

  // Set row prices
  std::vector<double> dual = {1.0};
  interface.set_row_dual(dual);

  auto result = interface.resolve();
  REQUIRE(result.has_value());
}

TEST_CASE("LinearInterface - set_col_sol with null span (no-op)")
{
  using namespace gtopt;

  LinearInterface interface;
  (void)interface.add_col(SparseCol {
      // NOLINT
      .uppb = 10.0,
  });

  const std::span<const double> null_span;
  CHECK_NOTHROW(interface.set_col_sol(null_span));

  const std::span<const double> null_dual;
  CHECK_NOTHROW(interface.set_row_dual(null_dual));
}

TEST_CASE("LinearInterface - get_obj_value after solve")
{
  using namespace gtopt;

  LinearInterface interface;

  [[maybe_unused]] const auto x1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 5.0,
  });

  auto result = interface.resolve();
  REQUIRE(result.has_value());

  CHECK(interface.get_obj_value_raw() == doctest::Approx(0.0));
}

TEST_CASE("LinearInterface - log file handling")
{
  using namespace gtopt;

  const std::string log_file = "/tmp/gtopt_test_lp_log";

  LinearInterface interface(std::string_view {}, log_file);

  const auto x1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.uppb = 5.0;
  (void)interface.add_row(row);  // NOLINT

  SolverOptions opts;
  opts.log_level = 1;

  auto result = interface.resolve(opts);
  REQUIRE(result.has_value());

  // Clean up log file
  const std::string log_filename = log_file + ".log";
  if (std::filesystem::exists(log_filename)) {
    std::filesystem::remove(log_filename);
  }
}

TEST_CASE("LinearInterface - FlatLinearProblem constructor")
{
  using namespace gtopt;

  LinearProblem lp("DirectLoad");

  const auto col1 = lp.add_col({.lowb = 0.0, .uppb = 10.0, .cost = 1.0});
  const auto col2 = lp.add_col({.lowb = 0.0, .uppb = 10.0, .cost = 2.0});

  auto row1 = lp.add_row({.uppb = 8.0});
  lp.set_coeff(row1, col1, 1.0);
  lp.set_coeff(row1, col2, 1.0);

  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  auto flat_lp = lp.flatten(flat_opts);

  // Construct directly from FlatLinearProblem
  auto& reg = SolverRegistry::instance();
  LinearInterface interface(reg.default_solver(), flat_lp);

  CHECK(interface.get_numcols() == 2);
  CHECK(interface.get_numrows() == 1);

  auto result = interface.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

TEST_CASE("LinearInterface - time limit")
{
  using namespace gtopt;

  LinearInterface interface;

  [[maybe_unused]] const auto x1 = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  interface.set_time_limit(100.0);

  auto result = interface.resolve();
  REQUIRE(result.has_value());
}

TEST_CASE(
    "LinearInterface - duplicate column metadata throws eagerly at add_col")
{
  using namespace gtopt;

  LinearInterface li;

  const auto ctx =
      make_stage_context(make_uid<Scenario>(0), make_uid<Stage>(0));

  // First column with labelled metadata is accepted.
  (void)li.add_col(SparseCol {
      // NOLINT
      .lowb = 0.0,
      .uppb = 1.0,
      .class_name = "X",
      .variable_name = "v",
      .variable_uid = Uid {1},
      .context = ctx,
  });

  // Second column with identical `(class_name, variable_name,
  // variable_uid, context)` metadata is rejected at add_col time
  // — no need to wait for `materialize_labels` or `write_lp`.
  {
    auto try_add = [&]
    {
      (void)li.add_col(SparseCol {
          .lowb = 0.0,
          .uppb = 1.0,
          .class_name = "X",
          .variable_name = "v",
          .variable_uid = Uid {1},
          .context = ctx,
      });
    };
    CHECK_THROWS_AS(try_add(), std::runtime_error);
  }
}

TEST_CASE("LinearInterface - unique metadata passes label synthesis")
{
  using namespace gtopt;

  LinearInterface li;
  li.set_label_maker(LabelMaker {LpNamesLevel::all});

  const auto ctx =
      make_stage_context(make_uid<Scenario>(0), make_uid<Stage>(0));

  (void)li.add_col(SparseCol {
      // NOLINT
      .lowb = 0.0,
      .uppb = 1.0,
      .class_name = "X",
      .variable_name = "v",
      .variable_uid = Uid {1},
      .context = ctx,
  });
  (void)li.add_col(SparseCol {
      // NOLINT
      .lowb = 0.0,
      .uppb = 1.0,
      .class_name = "X",
      .variable_name = "v",
      .variable_uid = Uid {2},
      .context = ctx,
  });
  li.materialize_labels();
  CHECK(li.col_name_map().size() == 2);

  SparseRow row1;
  row1[ColIndex {0}] = 1.0;
  row1.uppb = 1.0;
  row1.class_name = "R";
  row1.constraint_name = "1";
  row1.variable_uid = Uid {0};
  row1.context = ctx;
  (void)li.add_row(row1);  // NOLINT
  li.materialize_labels();
  CHECK(li.row_name_map().size() == 1);
}

TEST_CASE(
    "LinearInterface - empty metadata cols/rows are not tracked for uniqueness")
{
  using namespace gtopt;

  LinearInterface li;

  // Two unlabelled cols (no class_name / no variable_name / unknown
  // uid / monostate context) must both succeed — the dup detector
  // intentionally skips empty labels so structural tests that build
  // anonymous cells don't trip it.
  (void)li.add_col(SparseCol {.lowb = 0.0, .uppb = 1.0});  // NOLINT
  (void)li.add_col(SparseCol {.lowb = 0.0, .uppb = 2.0});  // NOLINT
  CHECK(li.get_numcols() == 2);

  SparseRow r1;
  r1[ColIndex {0}] = 1.0;
  r1.uppb = 5.0;
  SparseRow r2;
  r2[ColIndex {0}] = 1.0;
  r2.uppb = 10.0;
  (void)li.add_row(r1);  // NOLINT
  (void)li.add_row(r2);  // NOLINT
  CHECK(li.get_numrows() == 2);
}

TEST_CASE("LinearInterface - duplicate row metadata throws eagerly at add_row")
{
  using namespace gtopt;

  LinearInterface li;
  const auto ctx =
      make_stage_context(make_uid<Scenario>(0), make_uid<Stage>(0));

  // Column provides a live ColIndex for the SparseRow cmap.
  (void)li.add_col(SparseCol {
      // NOLINT
      .lowb = 0.0,
      .uppb = 1.0,
      .class_name = "X",
      .variable_name = "v",
      .variable_uid = Uid {1},
      .context = ctx,
  });

  SparseRow row;
  row[ColIndex {0}] = 1.0;
  row.uppb = 1.0;
  row.class_name = "R";
  row.constraint_name = "1";
  row.variable_uid = Uid {0};
  row.context = ctx;
  (void)li.add_row(row);  // NOLINT

  // Second row with identical metadata is rejected at add_row time.
  {
    auto try_add = [&] { (void)li.add_row(row); };
    CHECK_THROWS_AS(try_add(), std::runtime_error);
  }
}

// ─── Warm-start clone tests ────────────────────────────────────────────────

TEST_CASE("LinearInterface - clone preserves solution")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Build a simple LP:  min 2x1 + 3x2  s.t.  x1 + x2 >= 5,  x1,x2 in [0,10]
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 3.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.lowb = 5.0;
  row.uppb = LinearProblem::DblMax;
  (void)li.add_row(row);  // NOLINT

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE(li.is_optimal());
  const double orig_obj = li.get_obj_value_raw();

  SUBCASE("clone produces same objective")
  {
    auto cloned = li.clone();
    auto r = cloned.resolve();
    REQUIRE(r.has_value());
    CHECK(cloned.get_obj_value_raw() == doctest::Approx(orig_obj));
  }
}

TEST_CASE("LinearInterface - warm-start clone resolves after bound change")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // LP: min 2x1 + x2  s.t.  x1 + x2 >= 10,  x1 in [0,20], x2 in [0,20]
  // Optimal: x1=0, x2=10 → obj=10
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 20.0,
      .cost = 2.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = 20.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.lowb = 10.0;
  row.uppb = LinearProblem::DblMax;
  (void)li.add_row(row);  // NOLINT

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  CHECK(li.get_obj_value_raw() == doctest::Approx(10.0));

  // Clone with warm-start, tighten x1 >= 6, re-solve
  auto cloned = li.clone();
  cloned.set_col_low(x1, 6.0);

  SolverOptions ws_opts;
  auto r = cloned.resolve(ws_opts);
  REQUIRE(r.has_value());
  REQUIRE(cloned.is_optimal());

  // Optimal: x1=6, x2=4 → obj=12+4=16
  CHECK(cloned.get_obj_value_raw() == doctest::Approx(16.0));
  CHECK(cloned.get_col_sol()[x1] == doctest::Approx(6.0));
  CHECK(cloned.get_col_sol()[x2] == doctest::Approx(4.0));

  // Original is unmodified
  CHECK(li.get_col_low()[x1] == doctest::Approx(0.0));
}

TEST_CASE(
    "LinearInterface - warm-start clone works after barrier initial solve")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // LP: min 3x1 + 2x2  s.t.  x1 + x2 >= 8,  x1 in [0,10], x2 in [0,10]
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 3.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 1.0;
  row.lowb = 8.0;
  row.uppb = LinearProblem::DblMax;
  (void)li.add_row(row);  // NOLINT

  // Solve with barrier (the default algorithm)
  SolverOptions barrier_opts;
  barrier_opts.algorithm = LPAlgo::barrier;
  auto res = li.initial_solve(barrier_opts);
  REQUIRE(res.has_value());
  const double orig_obj = li.get_obj_value_raw();
  // Optimal: x1=0, x2=8 → obj=16
  CHECK(orig_obj == doctest::Approx(16.0));

  // Clone with warm-start and resolve with dual simplex (the warm-start path)
  auto cloned = li.clone();

  // Tighten x1 >= 3, re-solve with warm-start
  cloned.set_col_low(x1, 3.0);

  SolverOptions ws_opts;
  auto r = cloned.resolve(ws_opts);
  REQUIRE(r.has_value());
  REQUIRE(cloned.is_optimal());

  // Optimal: x1=3, x2=5 → obj=9+10=19
  CHECK(cloned.get_obj_value_raw() == doctest::Approx(19.0));
  CHECK(cloned.get_col_sol()[x1] == doctest::Approx(3.0));
  CHECK(cloned.get_col_sol()[x2] == doctest::Approx(5.0));
}

TEST_CASE(  // NOLINT
    "LinearInterface - load_flat with integer columns and name tracking")
{
  using namespace gtopt;

  LinearProblem lp("IntLP");

  const auto col1 = lp.add_col({.lowb = 0.0, .uppb = 10.0, .cost = 1.0});
  const auto col2 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 2.0,
      .is_integer = true,
  });

  auto row1 = lp.add_row({.uppb = 8.0});
  lp.set_coeff(row1, col1, 1.0);
  lp.set_coeff(row1, col2, 1.0);

  // Convert to flat with names
  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  flat_opts.col_with_name_map = true;
  flat_opts.row_with_name_map = true;
  auto flat_lp = lp.flatten(flat_opts);

  // Load with name tracking enabled (LabelMaker travels via flat_lp)
  LinearInterface li;
  li.load_flat(flat_lp);

  CHECK(li.get_numcols() == 2);
  CHECK(li.get_numrows() == 1);

  // Without metadata on SparseCol/SparseRow, names are empty
  CHECK(li.col_name_map().empty());
  CHECK(li.row_name_map().empty());

  // Integer column should be marked
  CHECK(li.is_integer(col2));
  CHECK(li.is_continuous(col1));

  auto result = li.resolve();
  REQUIRE(result.has_value());
}

TEST_CASE("LinearInterface - load_flat without names (minimal)")  // NOLINT
{
  using namespace gtopt;

  LinearProblem lp("NoNames");
  const auto col = lp.add_col({.lowb = 0.0, .uppb = 5.0, .cost = 1.0});

  // Add a row so flatten() does not early-return (needs ncols>0 AND nrows>0).
  auto row = SparseRow {.uppb = 10.0};
  row[col] = 1.0;
  [[maybe_unused]] const auto row_idx = lp.add_row(row);

  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  flat_opts.col_with_name_map = true;
  auto flat_lp = lp.flatten(flat_opts);

  LinearInterface li;
  li.load_flat(flat_lp);

  // Without metadata on SparseCol/SparseRow, names are empty at any level
  CHECK(li.col_name_map().empty());
  CHECK(li.row_name_map().empty());
}

TEST_CASE("LinearInterface - initial_solve returns error on infeasible LP")
// NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Create an infeasible LP: x >= 10 AND x <= 5
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 5.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.lowb = 10.0;
  row.uppb = LinearProblem::DblMax;
  (void)li.add_row(row);  // NOLINT

  auto result = li.initial_solve();
  REQUIRE_FALSE(result.has_value());

  const auto& err = result.error();
  CHECK(err.code == ErrorCode::SolverError);
  CHECK_FALSE(err.message.empty());
  CHECK(err.status == 2);  // infeasible status

  // Verify status queries
  CHECK_FALSE(li.is_optimal());
  CHECK(li.is_prim_infeasible());
  CHECK(li.get_status() == 2);
}

TEST_CASE("LinearInterface - resolve returns error on infeasible LP")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Create an infeasible LP: x >= 10 AND x <= 5
  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 5.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.lowb = 10.0;
  row.uppb = LinearProblem::DblMax;
  (void)li.add_row(row);  // NOLINT

  auto result = li.resolve();
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == ErrorCode::SolverError);
  CHECK_FALSE(li.is_optimal());
}

TEST_CASE("LinearInterface - get_coeff on empty LP returns zero")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearInterface li;
  const auto col = li.add_col(SparseCol {
      .uppb = 10.0,
  });

  // No rows added, matrix should return 0.0 for any coefficient
  CHECK(li.get_coeff(RowIndex {0}, col) == doctest::Approx(0.0));
}

TEST_CASE("LinearInterface - set_coeff and get_coeff round trip")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = 10.0,
  });

  SparseRow row;
  row[x1] = 2.0;
  row[x2] = 3.0;
  row.uppb = 20.0;
  const auto r1 = li.add_row(row);

  CHECK(li.get_coeff(r1, x1) == doctest::Approx(2.0));
  CHECK(li.get_coeff(r1, x2) == doctest::Approx(3.0));

  // Modify coefficient
  li.set_coeff(r1, x1, 5.0);
  CHECK(li.get_coeff(r1, x1) == doctest::Approx(5.0));
  CHECK(li.supports_set_coeff());
}

TEST_CASE("LinearInterface - set_log_file direct call")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearInterface li;
  li.set_log_file("/tmp/test_direct_log");

  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.uppb = 5.0;
  (void)li.add_row(row);  // NOLINT

  // Solve with logging enabled — exercises open/close log handler
  SolverOptions opts;
  opts.log_level = 1;
  auto result = li.resolve(opts);
  REQUIRE(result.has_value());

  // Cleanup
  const std::string log_file = "/tmp/test_direct_log.log";
  if (std::filesystem::exists(log_file)) {
    std::filesystem::remove(log_file);
  }
}

TEST_CASE("LinearInterface - solve with time_limit option")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearInterface li;
  const auto x1 = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });

  SparseRow row;
  row[x1] = 1.0;
  row.uppb = 5.0;
  (void)li.add_row(row);  // NOLINT

  SolverOptions opts;
  opts.time_limit = 60.0;
  auto result = li.initial_solve(opts);
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
}

// ─── row_index_to_name tests ────────────────────────────────────────────────

TEST_CASE("LinearInterface - row_index_to_name via load_flat")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearProblem lp("RowNames");
  const auto c1 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 1.0,
  });
  const auto c2 = lp.add_col({
      .lowb = 0.0,
      .uppb = 5.0,
      .cost = 2.0,
  });

  auto r1 = lp.add_row({
      .uppb = 8.0,
  });
  lp.set_coeff(r1, c1, 1.0);
  lp.set_coeff(r1, c2, 1.0);

  auto r2 = lp.add_row({
      .uppb = 4.0,
  });
  lp.set_coeff(r2, c1, 1.0);

  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  flat_opts.col_with_name_map = true;
  flat_opts.row_with_name_map = true;
  auto flat_lp = lp.flatten(flat_opts);

  LinearInterface li;
  li.load_flat(flat_lp);

  REQUIRE(li.get_numrows() == 2);

  SUBCASE("row_index_to_name has correct size (empty strings without metadata)")
  {
    const auto& names = li.row_index_to_name();
    // Without metadata, flatten() produces empty name strings, but
    // row_index_to_name is still populated (one entry per row).
    REQUIRE(names.size() == 2);
    CHECK(names[RowIndex {0}].empty());
    CHECK(names[RowIndex {1}].empty());
  }

  SUBCASE("row_name_map is empty without metadata")
  {
    // Empty name strings are not inserted into the name map.
    CHECK(li.row_name_map().empty());
  }
}

TEST_CASE("LinearInterface - row_index_to_name updated by add_row")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearInterface li;
  li.set_label_maker(LabelMaker {LpNamesLevel::all});

  const auto ctx =
      make_stage_context(make_uid<Scenario>(0), make_uid<Stage>(0));
  const auto c1 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .class_name = "V",
      .variable_name = "x",
      .variable_uid = Uid {0},
      .context = ctx,
  });

  auto make_row = [&](std::string_view cname, Uid uid, double uppb) -> SparseRow
  {
    SparseRow r;
    r[c1] = 1.0;
    r.uppb = uppb;
    r.class_name = "Row";
    r.constraint_name = cname;
    r.variable_uid = uid;
    r.context = ctx;
    return r;
  };

  (void)li.add_row(make_row("a", Uid {0}, 5.0));  // NOLINT
  (void)li.add_row(make_row("b", Uid {1}, 3.0));  // NOLINT
  (void)li.add_row(make_row("c", Uid {2}, 7.0));  // NOLINT

  // Under Option B (lazy label synthesis), `row_index_to_name` is not
  // populated at `add_row` time — force materialisation first.
  li.materialize_labels();

  REQUIRE(li.get_numrows() == 3);
  const auto& names = li.row_index_to_name();
  REQUIRE(names.size() == 3);
  CHECK(names[RowIndex {0}] == "row_a_0_0_0");
  CHECK(names[RowIndex {1}] == "row_b_1_0_0");
  CHECK(names[RowIndex {2}] == "row_c_2_0_0");
}

TEST_CASE(
    "LinearInterface - row_index_to_name rebuilt after delete_rows")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearInterface li;
  li.set_label_maker(LabelMaker {LpNamesLevel::all});

  const auto ctx =
      make_stage_context(make_uid<Scenario>(0), make_uid<Stage>(0));
  const auto c1 = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .class_name = "V",
      .variable_name = "x",
      .variable_uid = Uid {0},
      .context = ctx,
  });

  // Add 4 rows using SparseRow with metadata for name generation.
  // Use different UIDs to produce unique names: r_x_<uid>_0_0
  auto make_row = [&](Uid uid) -> SparseRow
  {
    SparseRow r;
    r[c1] = 1.0;
    r.uppb = 10.0;
    r.class_name = "R";
    r.constraint_name = "x";
    r.variable_uid = uid;
    r.context = ctx;
    return r;
  };

  for (int i = 0; i < 4; ++i) {
    (void)li.add_row(make_row(Uid {i}));  // NOLINT
  }
  REQUIRE(li.get_numrows() == 4);
  // Under Option B, force label materialisation before any
  // `row_index_to_name()` / `row_name_map()` inspection — including
  // after `delete_rows` which rebuilds the maps from whatever cached
  // names exist.
  li.materialize_labels();

  SUBCASE("delete middle row shifts indices")
  {
    // Delete row 1 ("r_x_1_0_0")
    std::vector<int> to_delete {
        1,
    };
    li.delete_rows(to_delete);

    REQUIRE(li.get_numrows() == 3);
    const auto& names = li.row_index_to_name();
    REQUIRE(names.size() == 3);
    CHECK(names[RowIndex {0}] == "r_x_0_0_0");
    CHECK(names[RowIndex {1}] == "r_x_2_0_0");
    CHECK(names[RowIndex {2}] == "r_x_3_0_0");

    // row_name_map must agree
    CHECK(li.row_name_map().at("r_x_0_0_0") == RowIndex {0});
    CHECK(li.row_name_map().at("r_x_2_0_0") == RowIndex {1});
    CHECK(li.row_name_map().at("r_x_3_0_0") == RowIndex {2});
    CHECK(li.row_name_map().count("r_x_1_0_0") == 0);
  }

  SUBCASE("delete first and last rows")
  {
    std::vector<int> to_delete {
        0,
        3,
    };
    li.delete_rows(to_delete);

    REQUIRE(li.get_numrows() == 2);
    const auto& names = li.row_index_to_name();
    REQUIRE(names.size() == 2);
    CHECK(names[RowIndex {0}] == "r_x_1_0_0");
    CHECK(names[RowIndex {1}] == "r_x_2_0_0");

    CHECK(li.row_name_map().at("r_x_1_0_0") == RowIndex {0});
    CHECK(li.row_name_map().at("r_x_2_0_0") == RowIndex {1});
  }

  SUBCASE("delete all rows leaves empty maps")
  {
    std::vector<int> to_delete {
        0,
        1,
        2,
        3,
    };
    li.delete_rows(to_delete);

    CHECK(li.get_numrows() == 0);
    CHECK(li.row_index_to_name().empty());
    CHECK(li.row_name_map().empty());
  }

  SUBCASE("add rows after deletion continues correctly")
  {
    std::vector<int> to_delete {
        1,
        2,
    };
    li.delete_rows(to_delete);
    REQUIRE(li.get_numrows() == 2);

    SparseRow r_new;
    r_new[c1] = 1.0;
    r_new.uppb = 5.0;
    r_new.class_name = "R";
    r_new.constraint_name = "new";
    r_new.variable_uid = Uid {4};
    r_new.context = ctx;
    (void)li.add_row(r_new);  // NOLINT
    li.materialize_labels();

    REQUIRE(li.get_numrows() == 3);
    const auto& names = li.row_index_to_name();
    REQUIRE(names.size() == 3);
    CHECK(names[RowIndex {0}] == "r_x_0_0_0");
    CHECK(names[RowIndex {1}] == "r_x_3_0_0");
    CHECK(names[RowIndex {2}] == "r_new_4_0_0");
  }
}

TEST_CASE("LinearInterface - row_index_to_name populated at all")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  LinearInterface li;
  li.set_label_maker(LabelMaker {LpNamesLevel::all});

  const auto c1 = li.add_col(SparseCol {
      .uppb = 10.0,
  });

  SparseRow row {
      .lowb = 0.0,
      .uppb = 5.0,
  };
  row[c1] = 1.0;
  (void)li.add_row(row);  // NOLINT

  CHECK(li.row_index_to_name().empty());
  CHECK(li.row_name_map().empty());
}

// ── ScaledView / get_col_sol() physical-space clamp ────────────────────────
//
// Regression: SDDP state propagation pins `lowb = uppb = prev_col_sol`
// on the next phase's dependent column.  When the solver returns
// `col_sol = uppb + eps` due to primal tolerance, the pin lands
// outside the target column's envelope and makes the next phase
// trivially infeasible.  Clamp at `get_col_sol()` (physical, after
// descaling) scrubs the noise; raw methods are left untouched.

TEST_CASE("ScaledView - clamps to physical bounds when provided")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)

  // Raw data slightly outside [0, 10] in raw space.  With scale=2.0
  // the physical values would be [-0.2, 8.0, 20.02], so the clamp
  // window in physical is [0*2, 10*2] = [0, 20].
  const std::array<double, 3> raw_sol {-0.1, 4.0, 10.01};
  const std::array<double, 3> raw_lo {0.0, 0.0, 0.0};
  const std::array<double, 3> raw_hi {10.0, 10.0, 10.0};
  const std::array<double, 3> scales {2.0, 2.0, 2.0};

  SUBCASE("with clamp bounds — values pulled into physical box")
  {
    const ScaledView view {
        raw_sol.data(),
        raw_sol.size(),
        scales.data(),
        scales.size(),
        raw_lo.data(),
        raw_lo.size(),
        raw_hi.data(),
        raw_hi.size(),
        ScaledView::Op::multiply,
    };
    // -0.1 * 2 = -0.2  → clamps up to 0.0
    CHECK(view[0] == doctest::Approx(0.0));
    // 4.0 * 2 = 8.0   → inside [0, 20], unchanged
    CHECK(view[1] == doctest::Approx(8.0));
    // 10.01 * 2 = 20.02 → clamps down to 20.0
    CHECK(view[2] == doctest::Approx(20.0));
  }

  SUBCASE("without clamp bounds — returns raw * scale unchanged")
  {
    const ScaledView view {
        raw_sol.data(),
        raw_sol.size(),
        scales.data(),
        scales.size(),
        ScaledView::Op::multiply,
    };
    CHECK(view[0] == doctest::Approx(-0.2));
    CHECK(view[1] == doctest::Approx(8.0));
    CHECK(view[2] == doctest::Approx(20.02));
  }

  SUBCASE("degenerate lb > ub — clamp skipped defensively")
  {
    const std::array<double, 1> s_raw {5.0};
    const std::array<double, 1> s_sc {1.0};
    const std::array<double, 1> s_lo {10.0};  // lb > ub (invalid)
    const std::array<double, 1> s_hi {0.0};
    const ScaledView view {
        s_raw.data(),
        1,
        s_sc.data(),
        1,
        s_lo.data(),
        1,
        s_hi.data(),
        1,
        ScaledView::Op::multiply,
    };
    // std::clamp with lb > ub is UB; guard prevents that — value
    // passes through.
    CHECK(view[0] == doctest::Approx(5.0));
  }
}

TEST_CASE(  // NOLINT
    "LinearInterface::get_col_sol respects clamp only at optimal")
{
  using namespace gtopt;  // NOLINT(google-global-names-in-headers)
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,
  // misc-const-correctness)

  // Minimize -x s.t. 0 <= x <= 10.  Optimal at x = 10 (upper bound).
  LinearInterface interface;
  const auto x = interface.add_col(SparseCol {
      .uppb = 10.0,
      .cost = -1.0,
  });

  SUBCASE("after optimal solve — physical view respects bound box")
  {
    const SolverOptions options;
    auto result = interface.initial_solve(options);
    REQUIRE(result.has_value());
    REQUIRE(interface.is_optimal());

    const auto sol_phys = interface.get_col_sol();
    const auto sol_raw = interface.get_col_sol_raw();
    // Both within the column's physical envelope.
    CHECK(sol_phys[x] == doctest::Approx(10.0));
    CHECK(sol_phys[x] >= 0.0);
    CHECK(sol_phys[x] <= 10.0);
    // Raw path untouched by clamp logic — identical here because
    // col_scale defaults to 1.0, but the key invariant is that
    // `get_col_sol_raw()` returns a span<const double> whereas
    // `get_col_sol()` returns a ScaledView (different types).
    CHECK(sol_raw[x] == doctest::Approx(10.0));
    static_assert(!std::is_same_v<decltype(sol_phys), decltype(sol_raw)>);
  }

  SUBCASE("before any solve — non-optimal, view stays unclamped")
  {
    // No solve called yet: `is_optimal()` is false, so the clamp
    // branch of get_col_sol() is NOT taken.  The view passes
    // through raw * scale with no bound enforcement.  We don't
    // check specific values (backend may expose garbage), only
    // that the call is safe and the optimality check holds.
    CHECK_FALSE(interface.is_optimal());
    [[maybe_unused]] const auto sol = interface.get_col_sol();
    CHECK(sol.size() == 1);
  }
}

// ─── Clone independence and invariant tests ─────────────────────────────────
//
// These tests lock in the *observable* behavior of `LinearInterface::clone()`
// so that the planned refactor (moving `m_col_scales_`,
// `m_variable_scale_map_`, `m_label_maker_`, label-meta vectors, etc. behind
// `std::shared_ptr<const T>` for cheap cloning + copy-on-write) cannot
// regress these guarantees.  They must pass both with the current by-value
// storage and with the future shared-pointer storage.
//
// Invariants verified:
//   1. Clones are independent of the original (mutations on one side do not
//      leak to the other) — catches accidental shared-mutable-state bugs.
//   2. Bundles that the clone must *preserve* (col_scales, scale_objective,
//      variable_scale_map) are correctly carried over by `clone()`.
//   3. Multi-clone scenarios: sibling clones, clone-of-clone, clone outliving
//      the original (lifetime test that catches dangling-shared_ptr bugs).

namespace
{
/// Build a small standalone LP:
///   min 2 x1 + 3 x2   s.t.  x1 + x2 >= 5,   x1, x2 in [0, 10]
/// Returns the interface together with the column / row indices.
///
/// Note: the type name `CloneFixture` (rather than e.g. `SimpleLp`) is
/// chosen to avoid a unity-build collision with the helper of the same
/// purpose defined in `test_linear_interface_lowmem.cpp`, whose
/// anonymous namespace would otherwise shadow this one when both files
/// land in the same unity batch.
struct CloneFixture
{
  LinearInterface li;
  ColIndex x1;
  ColIndex x2;
  RowIndex r;
};

[[nodiscard]] CloneFixture make_clone_fixture()
{
  CloneFixture s;
  s.x1 = s.li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 2.0,
  });
  s.x2 = s.li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 3.0,
  });

  SparseRow row;
  row[s.x1] = 1.0;
  row[s.x2] = 1.0;
  row.lowb = 5.0;
  row.uppb = LinearProblem::DblMax;
  s.r = s.li.add_row(row);
  return s;
}
}  // namespace

TEST_CASE("LinearInterface::clone - mutating clone does not affect original")
// NOLINT
{
  auto orig = make_clone_fixture();
  REQUIRE(orig.li.initial_solve().has_value());
  REQUIRE(orig.li.is_optimal());
  const double orig_obj = orig.li.get_obj_value_raw();
  const double orig_x1_low = orig.li.get_col_low()[orig.x1];
  const double orig_x1_upp = orig.li.get_col_upp()[orig.x1];
  const double orig_x1_cost = orig.li.get_obj_coeff()[orig.x1];
  const auto orig_ncols = orig.li.get_numcols();
  const auto orig_nrows = orig.li.get_numrows();

  auto cloned = orig.li.clone();

  SUBCASE("set_col_low on clone leaves original untouched")
  {
    cloned.set_col_low(orig.x1, 4.0);

    CHECK(orig.li.get_col_low()[orig.x1] == doctest::Approx(orig_x1_low));
    CHECK(cloned.get_col_low()[orig.x1] == doctest::Approx(4.0));
  }

  SUBCASE("set_col_upp on clone leaves original untouched")
  {
    cloned.set_col_upp(orig.x1, 7.0);

    CHECK(orig.li.get_col_upp()[orig.x1] == doctest::Approx(orig_x1_upp));
    CHECK(cloned.get_col_upp()[orig.x1] == doctest::Approx(7.0));
  }

  SUBCASE("set_obj_coeff_raw on clone leaves original untouched")
  {
    // `get_obj_coeff()` returns the backend's raw obj vector, so the
    // matching setter for raw round-trip is `set_obj_coeff_raw`.
    cloned.set_obj_coeff_raw(orig.x1, 99.0);

    CHECK(orig.li.get_obj_coeff()[orig.x1] == doctest::Approx(orig_x1_cost));
    CHECK(cloned.get_obj_coeff()[orig.x1] == doctest::Approx(99.0));
  }

  SUBCASE("add_col on clone leaves original ncols untouched")
  {
    [[maybe_unused]] const auto x3 = cloned.add_col(SparseCol {
        .uppb = 1.0,
        .cost = 0.5,
    });

    CHECK(orig.li.get_numcols() == orig_ncols);
    CHECK(cloned.get_numcols() == orig_ncols + 1);
  }

  SUBCASE("add_row on clone leaves original nrows untouched")
  {
    SparseRow row;
    row[orig.x1] = 1.0;
    row.lowb = 1.0;
    row.uppb = LinearProblem::DblMax;
    [[maybe_unused]] const auto rr = cloned.add_row(row);

    CHECK(orig.li.get_numrows() == orig_nrows);
    CHECK(cloned.get_numrows() == orig_nrows + 1);
  }

  SUBCASE("resolving the original after clone still yields original optimum")
  {
    cloned.set_col_low(orig.x1, 4.0);  // perturb clone
    [[maybe_unused]] auto _ = cloned.resolve();

    auto r = orig.li.resolve();
    REQUIRE(r.has_value());
    CHECK(orig.li.get_obj_value_raw() == doctest::Approx(orig_obj));
  }
}

TEST_CASE("LinearInterface::clone - mutating original does not affect clone")
// NOLINT
{
  auto orig = make_clone_fixture();
  REQUIRE(orig.li.initial_solve().has_value());

  auto cloned = orig.li.clone();
  const double clone_x1_low = cloned.get_col_low()[orig.x1];
  const double clone_x1_cost = cloned.get_obj_coeff()[orig.x1];
  const auto clone_ncols = cloned.get_numcols();

  // Mutate original after the clone is taken.
  orig.li.set_col_low(orig.x1, 3.0);
  orig.li.set_obj_coeff_raw(orig.x1, 50.0);
  [[maybe_unused]] const auto x3 = orig.li.add_col(SparseCol {
      .uppb = 1.0,
      .cost = 0.5,
  });

  CHECK(cloned.get_col_low()[orig.x1] == doctest::Approx(clone_x1_low));
  CHECK(cloned.get_obj_coeff()[orig.x1] == doctest::Approx(clone_x1_cost));
  CHECK(cloned.get_numcols() == clone_ncols);
}

TEST_CASE("LinearInterface::clone - preserves scale_objective and col_scales")
// NOLINT
{
  auto orig = make_clone_fixture();

  // Set some non-default scales that clone must propagate.
  orig.li.set_col_scale(orig.x1, 2.5);
  orig.li.set_col_scale(orig.x2, 0.5);

  // `scale_objective()` is an observable getter; the underlying field is
  // populated by `load_flat`, but cloning still needs to copy whatever
  // value the source carries.  We verify the clone reads the same value.
  const double src_scale_obj = orig.li.scale_objective();

  auto cloned = orig.li.clone();

  CHECK(cloned.scale_objective() == doctest::Approx(src_scale_obj));
  REQUIRE(cloned.get_col_scales().size() == orig.li.get_col_scales().size());
  CHECK(cloned.get_col_scales()[orig.x1] == doctest::Approx(2.5));
  CHECK(cloned.get_col_scales()[orig.x2] == doctest::Approx(0.5));

  // After mutating the original's scale, the clone's must remain stable —
  // catches accidental shared-mutable-vector regressions.
  orig.li.set_col_scale(orig.x1, 9.0);
  CHECK(cloned.get_col_scales()[orig.x1] == doctest::Approx(2.5));
}

TEST_CASE("LinearInterface::clone - preserves variable_scale_map lookups")
// NOLINT
{
  // Build a flat LP carrying a non-empty VariableScaleMap so that
  // `load_flat` populates `m_variable_scale_map_`.
  FlatLinearProblem flat;
  flat.ncols = 2;
  flat.nrows = 1;
  flat.matbeg = {0, 1, 2};
  flat.matind = {0, 0};
  flat.matval = {1.0, 1.0};
  flat.collb = {0.0, 0.0};
  flat.colub = {10.0, 10.0};
  flat.objval = {2.0, 3.0};
  flat.rowlb = {5.0};
  flat.rowub = {LinearProblem::DblMax};
  flat.col_scales = {1.0, 1.0};
  flat.row_scales = {1.0};

  const std::vector<VariableScale> scales {
      VariableScale {
          .class_name = "Generator",
          .variable = "p",
          .uid = Uid {7},
          .scale = 4.0,
      },
      VariableScale {
          .class_name = "Bus",
          .variable = "theta",
          .scale = 0.25,
      },
  };
  flat.variable_scale_map = VariableScaleMap {scales};

  LinearInterface src;
  src.load_flat(flat);
  REQUIRE_FALSE(src.variable_scale_map().empty());

  auto cloned = src.clone();

  // Lookups on the clone return the same scales as on the source.
  CHECK(cloned.variable_scale_map().lookup("Generator", "p", Uid {7})
        == doctest::Approx(4.0));
  CHECK(cloned.variable_scale_map().lookup("Bus", "theta")
        == doctest::Approx(0.25));
  // Unknown lookups still fall through to the default 1.0.
  CHECK(cloned.variable_scale_map().lookup("Generator", "missing")
        == doctest::Approx(1.0));
}

TEST_CASE("LinearInterface::clone - sibling clones are mutually independent")
// NOLINT
{
  auto orig = make_clone_fixture();
  REQUIRE(orig.li.initial_solve().has_value());

  auto a = orig.li.clone();
  auto b = orig.li.clone();

  a.set_col_low(orig.x1, 4.0);
  b.set_col_low(orig.x1, 2.0);

  CHECK(a.get_col_low()[orig.x1] == doctest::Approx(4.0));
  CHECK(b.get_col_low()[orig.x1] == doctest::Approx(2.0));
  // Original is also untouched.
  CHECK(orig.li.get_col_low()[orig.x1] == doctest::Approx(0.0));

  // Both clones still solve to their own optima.
  REQUIRE(a.resolve().has_value());
  REQUIRE(b.resolve().has_value());
  REQUIRE(a.is_optimal());
  REQUIRE(b.is_optimal());
}

TEST_CASE("LinearInterface::clone - clone-of-clone preserves invariants")
// NOLINT
{
  auto orig = make_clone_fixture();
  orig.li.set_col_scale(orig.x1, 3.0);
  REQUIRE(orig.li.initial_solve().has_value());
  const double orig_obj = orig.li.get_obj_value_raw();

  auto a = orig.li.clone();
  auto b = a.clone();

  // Scales survive two-level cloning.
  CHECK(b.get_col_scales()[orig.x1] == doctest::Approx(3.0));

  // Mutating the intermediate clone `a` does not affect `b`.
  a.set_col_low(orig.x1, 7.0);
  CHECK(b.get_col_low()[orig.x1] == doctest::Approx(0.0));

  // The grand-clone still solves correctly on its own.
  REQUIRE(b.resolve().has_value());
  REQUIRE(b.is_optimal());
  CHECK(b.get_obj_value_raw() == doctest::Approx(orig_obj));
}

TEST_CASE("LinearInterface::clone - clone outlives original")  // NOLINT
{
  // This is the lifetime test that catches shared_ptr aliasing bugs:
  // if the upcoming refactor stores the source's bundles by reference
  // instead of `shared_ptr`, destroying the source would dangle the
  // clone's view of those bundles.  With `shared_ptr<const T>` (the
  // proposed design) the clone owns its share and keeps reading
  // correctly after the source is destroyed.
  std::optional<LinearInterface> cloned_opt;
  double expected_x1_low = 0.0;
  double expected_x1_scale = 1.0;
  double expected_obj = 0.0;

  {
    auto orig = make_clone_fixture();
    orig.li.set_col_scale(orig.x1, 1.5);
    REQUIRE(orig.li.initial_solve().has_value());
    expected_x1_low = orig.li.get_col_low()[orig.x1];
    expected_x1_scale = orig.li.get_col_scales()[orig.x1];
    expected_obj = orig.li.get_obj_value_raw();

    cloned_opt.emplace(orig.li.clone());
  }  // orig destroyed here

  REQUIRE(cloned_opt.has_value());
  auto& cloned = *cloned_opt;

  // Reads still succeed and return the right values.
  CHECK(cloned.get_col_low()[ColIndex {0}] == doctest::Approx(expected_x1_low));
  CHECK(cloned.get_col_scales()[ColIndex {0}]
        == doctest::Approx(expected_x1_scale));

  // Resolve still works without referencing the destroyed source.
  REQUIRE(cloned.resolve().has_value());
  REQUIRE(cloned.is_optimal());
  CHECK(cloned.get_obj_value_raw() == doctest::Approx(expected_obj));
}

TEST_CASE("LinearInterface - set_obj_coeffs_raw bulk overwrite")  // NOLINT
{
  // Verifies the bulk objective setter:
  //   1. matches per-column writes byte-for-byte (every coefficient
  //      ends up at the requested value);
  //   2. zeroing the entire objective via one call works (the
  //      benders_cut.cpp Phase-1 use case);
  //   3. the no-op guard for empty input does not throw and does not
  //      perturb anything.
  LinearInterface li;
  const auto a = li.add_col(SparseCol {.uppb = 10.0, .cost = 1.5});
  const auto b = li.add_col(SparseCol {.uppb = 10.0, .cost = 2.5});
  const auto c = li.add_col(SparseCol {.uppb = 10.0, .cost = 3.5});

  // Sanity: the SparseCol values landed via add_col.
  REQUIRE(li.get_obj_coeff()[a] == doctest::Approx(1.5));
  REQUIRE(li.get_obj_coeff()[b] == doctest::Approx(2.5));
  REQUIRE(li.get_obj_coeff()[c] == doctest::Approx(3.5));

  SUBCASE("bulk overwrite with arbitrary values")
  {
    const std::array<double, 3> vals {7.0, 8.0, 9.0};
    li.set_obj_coeffs_raw(vals);
    CHECK(li.get_obj_coeff()[a] == doctest::Approx(7.0));
    CHECK(li.get_obj_coeff()[b] == doctest::Approx(8.0));
    CHECK(li.get_obj_coeff()[c] == doctest::Approx(9.0));
  }

  SUBCASE("bulk zero — Phase-1 elastic-clone use case")
  {
    const auto ncols = static_cast<size_t>(li.get_numcols());
    const std::vector<double> zeros(ncols, 0.0);
    li.set_obj_coeffs_raw(zeros);
    CHECK(li.get_obj_coeff()[a] == doctest::Approx(0.0));
    CHECK(li.get_obj_coeff()[b] == doctest::Approx(0.0));
    CHECK(li.get_obj_coeff()[c] == doctest::Approx(0.0));
  }

  SUBCASE("empty input is a no-op")
  {
    const std::span<const double> empty;
    li.set_obj_coeffs_raw(empty);
    // Original values untouched.
    CHECK(li.get_obj_coeff()[a] == doctest::Approx(1.5));
    CHECK(li.get_obj_coeff()[b] == doctest::Approx(2.5));
    CHECK(li.get_obj_coeff()[c] == doctest::Approx(3.5));
  }

  SUBCASE("equivalent to the per-column loop")
  {
    LinearInterface ref;
    std::ignore = ref.add_col(SparseCol {.uppb = 10.0, .cost = 1.5});
    std::ignore = ref.add_col(SparseCol {.uppb = 10.0, .cost = 2.5});
    std::ignore = ref.add_col(SparseCol {.uppb = 10.0, .cost = 3.5});

    const std::array<double, 3> vals {-1.0, 0.5, 4.25};
    // Reference: per-column loop (the path replaced by set_obj_coeffs_raw).
    for (size_t i = 0; i < vals.size(); ++i) {
      ref.set_obj_coeff_raw(ColIndex {static_cast<int>(i)}, vals[i]);
    }
    li.set_obj_coeffs_raw(vals);
    for (size_t i = 0; i < vals.size(); ++i) {
      const auto idx = ColIndex {static_cast<int>(i)};
      CHECK(li.get_obj_coeff()[idx]
            == doctest::Approx(ref.get_obj_coeff()[idx]));
    }
  }
}

TEST_CASE("LinearProblem::add_obj_constant — physical-units accumulator")
{
  using namespace gtopt;
  // The user-facing API is physical-scale: `add_obj_constant(c)`
  // accumulates into `m_obj_constant_` (physical) and `flatten()`
  // converts to raw via `/scale_objective` into
  // `FlatLinearProblem::obj_constant_raw`.

  LinearProblem lp("obj_const_acc");
  CHECK(lp.get_obj_constant() == doctest::Approx(0.0));

  lp.add_obj_constant(7.5);
  lp.add_obj_constant(-2.5);  // accumulates additively
  CHECK(lp.get_obj_constant() == doctest::Approx(5.0));

  // Need a non-empty LP for flatten() to return a populated struct.
  [[maybe_unused]] const auto col = lp.add_col(SparseCol {
      .uppb = 1.0,
  });
  [[maybe_unused]] const auto row = lp.add_row(SparseRow {
      .uppb = 1.0,
  });
  lp.set_coeff(row, col, 1.0);

  // Default `scale_objective = 1.0` → raw == physical.
  const auto flat_lp = lp.flatten();
  CHECK(flat_lp.obj_constant_raw == doctest::Approx(5.0));
}

TEST_CASE(
    "LinearInterface::get_obj_value — raw includes obj_constant; phys = raw × "
    "scale")
{
  using namespace gtopt;
  // `get_obj_value_raw()` composes the solver's raw value with the
  // raw-scale `obj_constant` stored on the LP.  `get_obj_value()`
  // is `get_obj_value_raw() × scale_objective`.  No extra physical
  // term in the composition — both views fall out of one formula.
  //
  // Setup: minimise 4·x subject to 0 ≤ x ≤ 10 ⇒ x* = 0, solver raw = 0.
  // With `obj_constant = 12.5` (physical) and `scale_objective = 2.0`,
  // raw-scale constant = 6.25; raw view = 0 + 6.25 = 6.25;
  // physical view = 6.25 × 2.0 = 12.5.

  LinearProblem lp("obj_const_compose");
  [[maybe_unused]] const auto x = lp.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 4.0,
  });
  // Add a trivial row so flatten() materialises both columns and rows.
  [[maybe_unused]] const auto r = lp.add_row(SparseRow {
      .uppb = 10.0,
  });
  lp.set_coeff(r, x, 1.0);

  lp.add_obj_constant(12.5);

  // Non-unit scale_objective exercises the conversion at flatten()
  // (physical → raw) and the inverse at `get_obj_value()` (raw →
  // physical).  At unit scale both views would be identical and the
  // test wouldn't distinguish the two.
  LpMatrixOptions opts;
  opts.scale_objective = 2.0;
  const auto flat_lp = lp.flatten(opts);

  LinearInterface li;
  li.load_flat(flat_lp);

  const auto result = li.resolve();
  REQUIRE(result.has_value());

  // Solver raw obj = 0 (x* = 0).  Raw view = 0 + 12.5/2 = 6.25.
  CHECK(li.get_obj_value_raw() == doctest::Approx(6.25));
  // Physical view = 6.25 × 2.0 = 12.5 — same as the
  // `add_obj_constant(12.5)` input, since the LP has no other cost.
  CHECK(li.get_obj_value() == doctest::Approx(12.5));
}

TEST_CASE("LinearInterface — obj_constant propagates across clone()")
{
  using namespace gtopt;
  // Native backend.clone() route: verify m_obj_constant_ is copied so
  // a cloned interface reports the same algebraic obj_value after
  // re-solving from a fresh solver state.

  LinearProblem lp("obj_const_clone");
  [[maybe_unused]] const auto x = lp.add_col(SparseCol {
      .uppb = 5.0,
      .cost = 1.0,
  });
  [[maybe_unused]] const auto r = lp.add_row(SparseRow {
      .uppb = 5.0,
  });
  lp.set_coeff(r, x, 1.0);
  lp.add_obj_constant(99.0);

  const auto flat_lp = lp.flatten();
  LinearInterface src;
  src.load_flat(flat_lp);
  REQUIRE(src.resolve().has_value());
  CHECK(src.get_obj_value() == doctest::Approx(99.0));

  // CloneKind::deep is the typical SDDP / cascade path.
  auto cloned = src.clone(LinearInterface::CloneKind::deep);
  REQUIRE(cloned.resolve().has_value());
  CHECK(cloned.get_obj_value() == doctest::Approx(99.0));
}

// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,
// misc-const-correctness)