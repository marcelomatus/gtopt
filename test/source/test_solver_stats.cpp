/**
 * @file      test_solver_stats.cpp
 * @brief     Tests for the SolverStats counter struct and its wiring
 *            into LinearInterface.
 * @date      2026-04-16
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/low_memory_snapshot.hpp>
#include <gtopt/memory_compress.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_stats.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ---------------------------------------------------------------------------
// Pure struct behaviour (no backend)
// ---------------------------------------------------------------------------

TEST_CASE("SolverStats default state is zero")  // NOLINT
{
  constexpr SolverStats s {};
  static_assert(s.load_problem_calls == 0);
  static_assert(s.initial_solve_calls == 0);
  static_assert(s.resolve_calls == 0);
  static_assert(s.fallback_solves == 0);
  static_assert(s.crossover_solves == 0);
  static_assert(s.infeasible_count == 0);
  static_assert(s.primal_infeasible == 0);
  static_assert(s.dual_infeasible == 0);
  static_assert(s.total_ncols == 0);
  static_assert(s.total_nrows == 0);
  static_assert(s.total_solve_time_s == 0.0);
  static_assert(s.max_kappa == -1.0);
  static_assert(s.total_solve_calls() == 0);
  static_assert(s.total_backend_solves() == 0);
  CHECK(s.avg_ncols() == doctest::Approx(0.0));
  CHECK(s.avg_nrows() == doctest::Approx(0.0));
}

TEST_CASE("SolverStats derived helpers compute correctly")  // NOLINT
{
  SolverStats s;
  s.initial_solve_calls = 2;
  s.resolve_calls = 3;
  s.fallback_solves = 1;
  s.crossover_solves = 4;
  s.total_ncols = 500;
  s.total_nrows = 1000;

  CHECK(s.total_solve_calls() == 5);
  CHECK(s.total_backend_solves() == 10);
  CHECK(s.avg_ncols() == doctest::Approx(100.0));
  CHECK(s.avg_nrows() == doctest::Approx(200.0));
}

TEST_CASE("SolverStats operator+= sums counters and takes max_kappa")  // NOLINT
{
  SolverStats a;
  a.load_problem_calls = 1;
  a.initial_solve_calls = 2;
  a.resolve_calls = 3;
  a.fallback_solves = 4;
  a.crossover_solves = 5;
  a.infeasible_count = 6;
  a.primal_infeasible = 4;
  a.dual_infeasible = 2;
  a.total_solve_time_s = 0.25;
  a.max_kappa = 1.0e4;
  a.total_ncols = 10;
  a.total_nrows = 20;

  SolverStats b;
  b.load_problem_calls = 10;
  b.initial_solve_calls = 20;
  b.resolve_calls = 30;
  b.fallback_solves = 40;
  b.crossover_solves = 50;
  b.infeasible_count = 60;
  b.primal_infeasible = 40;
  b.dual_infeasible = 20;
  b.total_solve_time_s = 0.75;
  b.max_kappa = 7.5e3;  // smaller than a.max_kappa
  b.total_ncols = 100;
  b.total_nrows = 200;

  a += b;

  CHECK(a.load_problem_calls == 11);
  CHECK(a.initial_solve_calls == 22);
  CHECK(a.resolve_calls == 33);
  CHECK(a.fallback_solves == 44);
  CHECK(a.crossover_solves == 55);
  CHECK(a.infeasible_count == 66);
  CHECK(a.primal_infeasible == 44);
  CHECK(a.dual_infeasible == 22);
  CHECK(a.total_solve_time_s == doctest::Approx(1.0));
  CHECK(a.max_kappa == doctest::Approx(1.0e4));  // kept a's larger kappa
  CHECK(a.total_ncols == 110);
  CHECK(a.total_nrows == 220);
}

TEST_CASE(
    "SolverStats operator+= promotes max_kappa from rhs when larger")  // NOLINT
{
  SolverStats a;
  a.max_kappa = 10.0;
  SolverStats b;
  b.max_kappa = 1.0e6;

  a += b;
  CHECK(a.max_kappa == doctest::Approx(1.0e6));
}

TEST_CASE(
    "SolverStats operator+= preserves -1.0 sentinel if rhs has no kappa")  // NOLINT
{
  SolverStats a;  // max_kappa = -1.0
  const SolverStats b;  // max_kappa = -1.0
  a += b;
  CHECK(a.max_kappa == doctest::Approx(-1.0));
}

TEST_CASE("SolverStats operator+ is non-mutating")  // NOLINT
{
  SolverStats a;
  a.initial_solve_calls = 2;
  SolverStats b;
  b.initial_solve_calls = 3;

  const auto c = a + b;
  CHECK(a.initial_solve_calls == 2);  // unchanged
  CHECK(b.initial_solve_calls == 3);  // unchanged
  CHECK(c.initial_solve_calls == 5);
}

TEST_CASE("SolverStats reset restores default state")  // NOLINT
{
  SolverStats s;
  s.initial_solve_calls = 99;
  s.max_kappa = 1.0e9;
  s.total_ncols = 12345;

  s.reset();

  CHECK(s.initial_solve_calls == 0);
  CHECK(s.max_kappa == doctest::Approx(-1.0));
  CHECK(s.total_ncols == 0);
  CHECK(s.total_solve_calls() == 0);
}

// ---------------------------------------------------------------------------
// LinearInterface integration
// ---------------------------------------------------------------------------

namespace
// NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
struct SimpleLp
{
  LinearInterface li;
  FlatLinearProblem flat;
  ColIndex x1;
  ColIndex x2;
};

SimpleLp make_feasible_lp()
{
  // min 2x1 + 3x2  s.t.  x1 + x2 >= 5,  0 <= x1,x2 <= 10
  LinearProblem lp;
  const auto c1 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 2.0,
  });
  const auto c2 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 3.0,
  });
  const auto r = lp.add_row({
      .lowb = 5.0,
      .uppb = SparseRow::DblMax,
  });
  lp.set_coeff(r, c1, 1.0);
  lp.set_coeff(r, c2, 1.0);

  LpMatrixOptions opts;
  opts.col_with_names = true;
  opts.row_with_names = true;
  auto flat = lp.flatten(opts);

  LinearInterface li;
  li.load_flat(flat);
  li.save_base_numrows();

  return SimpleLp {
      .li = std::move(li),
      .flat = std::move(flat),
      .x1 = ColIndex {0},
      .x2 = ColIndex {1},
  };
}

}  // namespace

TEST_CASE("SolverStats load_flat increments load_problem_calls")  // NOLINT
{
  auto [li, flat, x1, x2] = make_feasible_lp();
  CHECK(li.solver_stats().load_problem_calls == 1);
  CHECK(li.solver_stats().total_solve_calls() == 0);
}

TEST_CASE("SolverStats initial_solve wires counters")  // NOLINT
{
  auto [li, flat, x1, x2] = make_feasible_lp();
  const auto ncols = static_cast<std::size_t>(li.get_numcols());
  const auto nrows = static_cast<std::size_t>(li.get_numrows());

  auto r = li.initial_solve();
  REQUIRE(r.has_value());

  const auto& s = li.solver_stats();
  CHECK(s.initial_solve_calls == 1);
  CHECK(s.resolve_calls == 0);
  CHECK(s.infeasible_count == 0);
  CHECK(s.total_ncols == ncols);
  CHECK(s.total_nrows == nrows);
  CHECK(s.total_solve_calls() == 1);
  CHECK(s.avg_ncols() == doctest::Approx(static_cast<double>(ncols)));
  CHECK(s.avg_nrows() == doctest::Approx(static_cast<double>(nrows)));
  CHECK(s.total_solve_time_s >= 0.0);
}

TEST_CASE("SolverStats resolve increments resolve_calls")  // NOLINT
{
  auto [li, flat, x1, x2] = make_feasible_lp();
  const auto ncols = static_cast<std::size_t>(li.get_numcols());
  const auto nrows = static_cast<std::size_t>(li.get_numrows());

  auto r1 = li.initial_solve();
  REQUIRE(r1.has_value());
  auto r2 = li.resolve();
  REQUIRE(r2.has_value());
  auto r3 = li.resolve();
  REQUIRE(r3.has_value());

  const auto& s = li.solver_stats();
  CHECK(s.initial_solve_calls == 1);
  CHECK(s.resolve_calls == 2);
  CHECK(s.total_solve_calls() == 3);
  CHECK(s.total_ncols == 3 * ncols);
  CHECK(s.total_nrows == 3 * nrows);
}

TEST_CASE("SolverStats counts primal-infeasible solves")  // NOLINT
{
  // x >= 10 AND x <= 5 — primal infeasible.
  LinearInterface li;
  const auto x = li.add_col(SparseCol {
      .uppb = 5.0,
      .cost = 1.0,
  });
  SparseRow row;
  row[x] = 1.0;
  row.lowb = 10.0;
  row.uppb = LinearProblem::DblMax;
  li.add_row(row);

  auto r = li.initial_solve(SolverOptions {.log_level = 0});
  REQUIRE_FALSE(r.has_value());

  const auto& s = li.solver_stats();
  CHECK(s.initial_solve_calls == 1);
  CHECK(s.infeasible_count == 1);
  // Either primal_infeasible or dual_infeasible must have fired.
  CHECK((s.primal_infeasible + s.dual_infeasible) >= 1);
  // Fallback cycle tried multiple algorithms.
  CHECK(s.fallback_solves >= 1);
}

TEST_CASE("SolverStats merge_solver_stats folds clone counters")  // NOLINT
{
  auto [li, flat, x1, x2] = make_feasible_lp();
  auto r = li.initial_solve();
  REQUIRE(r.has_value());
  const auto base = li.solver_stats();

  SolverStats fake_clone;
  fake_clone.initial_solve_calls = 7;
  fake_clone.resolve_calls = 11;
  fake_clone.total_ncols = 123;

  li.merge_solver_stats(fake_clone);

  const auto& merged = li.solver_stats();
  CHECK(merged.initial_solve_calls == base.initial_solve_calls + 7);
  CHECK(merged.resolve_calls == base.resolve_calls + 11);
  CHECK(merged.total_ncols == base.total_ncols + 123);
}

TEST_CASE(
    "SolverStats low_memory snapshot reconstruct bumps load_problem_calls")  // NOLINT
{
  auto [li, flat, x1, x2] = make_feasible_lp();
  // Baseline: 1 load_flat at construction.
  CHECK(li.solver_stats().load_problem_calls == 1);

  li.set_low_memory(LowMemoryMode::snapshot);
  li.save_snapshot(FlatLinearProblem {flat});

  // Release/reconstruct cycle → reconstruct_backend re-invokes load_flat.
  li.release_backend();
  li.reconstruct_backend();
  CHECK(li.solver_stats().load_problem_calls == 2);

  li.release_backend();
  li.reconstruct_backend();
  CHECK(li.solver_stats().load_problem_calls == 3);
}

TEST_CASE(
    "SolverStats low_memory compress reconstruct bumps load_problem_calls")  // NOLINT
{
  auto [li, flat, x1, x2] = make_feasible_lp();
  CHECK(li.solver_stats().load_problem_calls == 1);

  li.set_low_memory(LowMemoryMode::compress, CompressionCodec::zstd);
  li.save_snapshot(FlatLinearProblem {flat});

  li.release_backend();
  li.reconstruct_backend();
  auto r = li.resolve();
  REQUIRE(r.has_value());

  const auto& s = li.solver_stats();
  CHECK(s.load_problem_calls == 2);
  CHECK(s.resolve_calls == 1);
  CHECK(s.total_solve_calls() == 1);
}
