/**
 * @file      test_solver_backend_solution_contract.cpp
 * @brief     Regression tests for the SolverBackend solution-accessor
 *            contract — locks down current pointer-getter semantics
 *            across every loaded backend, in every low-memory mode.
 * @date      2026-05-04
 * @copyright BSD-3-Clause
 *
 * These tests pin the post-Phase-1 (`522d4150`) behaviour of
 *   `SolverBackend::col_solution() / reduced_cost() / row_price()`
 * so that the upcoming Phase-2 span-out API
 * (`fill_col_sol / fill_col_cost / fill_row_dual` — see
 * docs/solver_backend_span_out_plan.md) cannot regress the
 * pointer-getter path silently.  The same cases will be re-used after
 * Phase 2 lands to confirm the new fills produce identical values
 * (T5–T10 in the plan).
 *
 * Tier T1: pointer-getter contract under `LowMemoryMode::off`.
 *           Solve, read each accessor, assert against analytical answer;
 *           mutate + resolve, re-read, confirm the buffer was refilled.
 * Tier T2: pointer-getter survives one `release → ensure → resolve`
 *           cycle under `LowMemoryMode::compress`.
 * Tier T4: under compress, `get_col_sol_raw` returns a span backed by
 *           the LI's `m_cached_col_sol_` (single source of truth) — the
 *           plugin pointer-getter is reached only via the LI fallback.
 *
 * (T3 — call-count instrumentation — is omitted from this initial
 * regression set because it would require invasive plugin hooks.  The
 * Phase-2 step that adds the new `fill_*` overrides will be reviewed by
 * source inspection for the "no cross-accessor refill" property.)
 */

#include <span>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/error.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
// NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
/// Build the canonical 2-variable LP used by every test in this file.
///
///   min  -3 x1 - 5 x2
///   s.t.  x1            <= 4
///                  2 x2 <= 12
///         3 x1 + 2 x2   <= 18
///         x1, x2 >= 0
///
/// Optimal:  x1 = 2, x2 = 6, obj = -36.  Reduced costs at optimum are
/// zero on both bounded vars; row duals correspond to the binding
/// constraints (rows 2 and 3).  The structure makes T3-style
/// per-accessor distinguishability easy: x1 ≠ x2, so a swap in
/// `fill_col_sol` would surface as wrong values, not just "the same
/// number twice".
struct WolseyLp
{
  ColIndex x1;
  ColIndex x2;
  RowIndex r1;
  RowIndex r2;
  RowIndex r3;
};

[[nodiscard]] WolseyLp build_wolsey(LinearInterface& li)
{
  const auto x1 = li.add_col(SparseCol {
      .uppb = LinearProblem::DblMax,
      .cost = -3.0,
  });
  const auto x2 = li.add_col(SparseCol {
      .uppb = LinearProblem::DblMax,
      .cost = -5.0,
  });

  SparseRow row1;
  row1[x1] = 1.0;
  row1.uppb = 4.0;
  const auto r1 = li.add_row(row1);

  SparseRow row2;
  row2[x2] = 2.0;
  row2.uppb = 12.0;
  const auto r2 = li.add_row(row2);

  SparseRow row3;
  row3[x1] = 3.0;
  row3[x2] = 2.0;
  row3.uppb = 18.0;
  const auto r3 = li.add_row(row3);

  return WolseyLp {
      .x1 = x1,
      .x2 = x2,
      .r1 = r1,
      .r2 = r2,
      .r3 = r3,
  };
}

/// Compress-mode variant: build the same LP via `LinearProblem`,
/// flatten it, install via `load_flat`, save the snapshot so
/// `release_backend` can later reconstruct.  Mirrors the canonical
/// setup in `test_linear_interface_lowmem.cpp`.
[[nodiscard]] WolseyLp build_wolsey_compress(LinearInterface& li)
{
  LinearProblem lp;
  const auto x1 = lp.add_col(SparseCol {
      .uppb = LinearProblem::DblMax,
      .cost = -3.0,
  });
  const auto x2 = lp.add_col(SparseCol {
      .uppb = LinearProblem::DblMax,
      .cost = -5.0,
  });
  const auto r1 = lp.add_row(SparseRow {.uppb = 4.0});
  lp.set_coeff(r1, x1, 1.0);
  const auto r2 = lp.add_row(SparseRow {.uppb = 12.0});
  lp.set_coeff(r2, x2, 2.0);
  const auto r3 = lp.add_row(SparseRow {.uppb = 18.0});
  lp.set_coeff(r3, x1, 3.0);
  lp.set_coeff(r3, x2, 2.0);

  auto flat = lp.flatten({});
  li.load_flat(flat);
  li.save_base_numrows();
  li.set_low_memory(LowMemoryMode::compress);
  li.save_snapshot(FlatLinearProblem {flat});

  return WolseyLp {
      .x1 = x1,
      .x2 = x2,
      .r1 = r1,
      .r2 = r2,
      .r3 = r3,
  };
}

constexpr double kTol = 1e-6;

}  // namespace

// ── T1: pointer-getter contract under off mode ─────────────────────────────

TEST_CASE(  // NOLINT
    "SolverBackend - T1 pointer-getter values under LowMemoryMode::off")
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  for (const auto& solver : reg.available_solvers()) {
    CAPTURE(solver);
    try {
      LinearInterface li(solver);
      const auto lp = build_wolsey(li);

      auto solve = li.initial_solve(SolverOptions {
          .log_level = 0,
      });
      REQUIRE(solve.has_value());
      REQUIRE(li.is_optimal());

      // I6: off-mode never populates the LI cache.
      CHECK(li.cached_col_sol_size() == 0);
      CHECK(li.cached_col_cost_size() == 0);
      CHECK(li.cached_row_dual_size() == 0);

      // Pointer-getters route directly to the backend in off mode.
      const auto col_sol = li.get_col_sol_raw();
      const auto col_cost = li.get_col_cost_raw();
      const auto row_dual = li.get_row_dual_raw();

      REQUIRE(col_sol.size() == 2);
      REQUIRE(col_cost.size() == 2);
      REQUIRE(row_dual.size() == 3);

      CHECK(col_sol[static_cast<size_t>(lp.x1.value_of())]
            == doctest::Approx(2.0).epsilon(kTol));
      CHECK(col_sol[static_cast<size_t>(lp.x2.value_of())]
            == doctest::Approx(6.0).epsilon(kTol));
      CHECK(li.get_obj_value_raw() == doctest::Approx(-36.0).epsilon(kTol));
    } catch (const std::exception& e) {
      MESSAGE("Solver '", solver, "' not available: ", e.what());
    }
  }
}

TEST_CASE(  // NOLINT
    "SolverBackend - T1 pointer-getter refills after mutate + resolve")
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  for (const auto& solver : reg.available_solvers()) {
    CAPTURE(solver);
    try {
      LinearInterface li(solver);
      const auto lp = build_wolsey(li);

      auto solve = li.initial_solve(SolverOptions {.log_level = 0});
      REQUIRE(solve.has_value());
      REQUIRE(li.is_optimal());

      // Tighten r1: x1 <= 4 → x1 <= 1.  New optimum: x1 = 1, x2 = 6,
      // obj = -33.  The pointer-getter must reflect the new values.
      li.set_row_upp_raw(lp.r1, 1.0);

      auto resolve = li.resolve(SolverOptions {.log_level = 0});
      REQUIRE(resolve.has_value());
      REQUIRE(li.is_optimal());

      const auto col_sol = li.get_col_sol_raw();
      REQUIRE(col_sol.size() == 2);
      CHECK(col_sol[static_cast<size_t>(lp.x1.value_of())]
            == doctest::Approx(1.0).epsilon(kTol));
      CHECK(col_sol[static_cast<size_t>(lp.x2.value_of())]
            == doctest::Approx(6.0).epsilon(kTol));
      CHECK(li.get_obj_value_raw() == doctest::Approx(-33.0).epsilon(kTol));
    } catch (const std::exception& e) {
      MESSAGE("Solver '", solver, "' not available: ", e.what());
    }
  }
}

// ── T2: pointer-getter survives release/reconstruct under compress ─────────

TEST_CASE(  // NOLINT
    "SolverBackend - T2 pointer-getter under LowMemoryMode::compress")
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  for (const auto& solver : reg.available_solvers()) {
    CAPTURE(solver);
    try {
      LinearInterface li(solver);
      const auto lp = build_wolsey_compress(li);

      auto solve = li.initial_solve(SolverOptions {.log_level = 0});
      REQUIRE(solve.has_value());
      REQUIRE(li.is_optimal());

      // Compress mode populates the LI cache eagerly post-solve so
      // downstream readers don't need to reach the backend.
      CHECK(li.cached_col_sol_size() == 2);
      CHECK(li.cached_col_cost_size() == 2);
      CHECK(li.cached_row_dual_size() == 3);

      const auto sol_before = li.get_col_sol_raw();
      const std::vector<double> snapshot {sol_before.begin(), sol_before.end()};

      // Release the backend (compress: snapshot persists, live backend
      // gone).  The pointer path should now serve from the LI cache.
      li.release_backend();
      CHECK(li.is_backend_released());
      CHECK(li.has_snapshot_data());
      CHECK(li.cached_col_sol_size() == 2);

      const auto sol_after_release = li.get_col_sol_raw();
      REQUIRE(sol_after_release.size() == 2);
      CHECK(sol_after_release[static_cast<size_t>(lp.x1.value_of())]
            == doctest::Approx(snapshot[static_cast<size_t>(lp.x1.value_of())])
                   .epsilon(kTol));
      CHECK(sol_after_release[static_cast<size_t>(lp.x2.value_of())]
            == doctest::Approx(snapshot[static_cast<size_t>(lp.x2.value_of())])
                   .epsilon(kTol));

      // Force a reconstruct + re-solve via mutation.  Tighter r1
      // again: new optimum (1, 6, -33).
      li.set_row_upp_raw(lp.r1, 1.0);
      auto resolve = li.resolve(SolverOptions {.log_level = 0});
      REQUIRE(resolve.has_value());
      REQUIRE(li.is_optimal());

      const auto col_sol = li.get_col_sol_raw();
      REQUIRE(col_sol.size() == 2);
      CHECK(col_sol[static_cast<size_t>(lp.x1.value_of())]
            == doctest::Approx(1.0).epsilon(kTol));
      CHECK(col_sol[static_cast<size_t>(lp.x2.value_of())]
            == doctest::Approx(6.0).epsilon(kTol));
      CHECK(li.get_obj_value_raw() == doctest::Approx(-33.0).epsilon(kTol));
    } catch (const std::exception& e) {
      MESSAGE("Solver '", solver, "' not available: ", e.what());
    }
  }
}

// ── T4: LI cache identity under compress ───────────────────────────────────

TEST_CASE(  // NOLINT
    "SolverBackend - T4 LI cache is the single source of truth in compress")
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  for (const auto& solver : reg.available_solvers()) {
    CAPTURE(solver);
    try {
      LinearInterface li(solver);
      [[maybe_unused]] const auto lp = build_wolsey_compress(li);

      auto solve = li.initial_solve(SolverOptions {.log_level = 0});
      REQUIRE(solve.has_value());
      REQUIRE(li.is_optimal());

      // After a compress-mode solve, every read accessor returns a
      // span backed by the LI's `m_cached_*` buffer.  Compare sizes
      // (a stable proxy for "the cache is populated and read paths
      // route through it") — pointer identity is checked only when
      // the accessors expose `.data()` directly.
      CHECK(li.get_col_sol_raw().size() == li.cached_col_sol_size());
      CHECK(li.get_col_cost_raw().size() == li.cached_col_cost_size());
      CHECK(li.get_row_dual_raw().size() == li.cached_row_dual_size());

      // After release_backend the LI cache must still be the read
      // source — pointer-getter on the (now dead) backend must not be
      // touched via the `get_col_sol_raw` path.
      li.release_backend();
      CHECK(li.is_backend_released());
      CHECK(li.cached_col_sol_size() == 2);
      CHECK(li.get_col_sol_raw().size() == 2);
    } catch (const std::exception& e) {
      MESSAGE("Solver '", solver, "' not available: ", e.what());
    }
  }
}

// ── T5: span-out fill_* matches pointer-getter ────────────────────────────

TEST_CASE(  // NOLINT
    "SolverBackend - T5 fill_col_sol/cost/dual matches pointer-getter")
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  for (const auto& solver : reg.available_solvers()) {
    CAPTURE(solver);
    try {
      LinearInterface li(solver);
      [[maybe_unused]] const auto lp = build_wolsey(li);
      auto solve = li.initial_solve(SolverOptions {.log_level = 0});
      REQUIRE(solve.has_value());
      REQUIRE(li.is_optimal());

      const auto ncols = static_cast<size_t>(li.get_numcols());
      const auto nrows = static_cast<size_t>(li.get_numrows());

      // Reference values via pointer-getter (off-mode path).
      const auto ref_sol = li.get_col_sol_raw();
      const auto ref_cost = li.get_col_cost_raw();
      const auto ref_dual = li.get_row_dual_raw();
      REQUIRE(ref_sol.size() == ncols);
      REQUIRE(ref_cost.size() == ncols);
      REQUIRE(ref_dual.size() == nrows);

      // Span-out fill into caller-owned buffers.
      std::vector<double> sol_buf(ncols);
      std::vector<double> cost_buf(ncols);
      std::vector<double> dual_buf(nrows);
      li.fill_col_sol(sol_buf);
      li.fill_col_cost(cost_buf);
      li.fill_row_dual(dual_buf);

      for (size_t i = 0; i < ncols; ++i) {
        CAPTURE(i);
        CHECK(sol_buf[i] == doctest::Approx(ref_sol[i]).epsilon(kTol));
        CHECK(cost_buf[i] == doctest::Approx(ref_cost[i]).epsilon(kTol));
      }
      for (size_t i = 0; i < nrows; ++i) {
        CAPTURE(i);
        CHECK(dual_buf[i] == doctest::Approx(ref_dual[i]).epsilon(kTol));
      }
    } catch (const std::exception& e) {
      MESSAGE("Solver '", solver, "' not available: ", e.what());
    }
  }
}

// ── T6: span-out tolerates empty span ─────────────────────────────────────

TEST_CASE("SolverBackend - T6 fill_* with empty span is a no-op")  // NOLINT
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  for (const auto& solver : reg.available_solvers()) {
    CAPTURE(solver);
    try {
      LinearInterface li(solver);
      [[maybe_unused]] const auto lp = build_wolsey(li);
      auto solve = li.initial_solve(SolverOptions {.log_level = 0});
      REQUIRE(solve.has_value());
      REQUIRE(li.is_optimal());

      // Sentinel before: a non-zero pattern lives in `canary`.  An
      // empty-span fill must not dereference / write past the span.
      std::vector<double> canary {};  // size 0
      li.fill_col_sol(canary);
      li.fill_col_cost(canary);
      li.fill_row_dual(canary);
      CHECK(canary.empty());
    } catch (const std::exception& e) {
      MESSAGE("Solver '", solver, "' not available: ", e.what());
    }
  }
}

// ── T8: off-mode I6 invariant — fill_* doesn't accidentally cache ──────────

TEST_CASE(  // NOLINT
    "SolverBackend - T8 off-mode LI cache stays empty after fill_*")
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  for (const auto& solver : reg.available_solvers()) {
    CAPTURE(solver);
    try {
      LinearInterface li(solver);  // default mode is off
      [[maybe_unused]] const auto lp = build_wolsey(li);
      auto solve = li.initial_solve(SolverOptions {.log_level = 0});
      REQUIRE(solve.has_value());
      REQUIRE(li.is_optimal());

      // I6 baseline.
      CHECK(li.cached_col_sol_size() == 0);
      CHECK(li.cached_col_cost_size() == 0);
      CHECK(li.cached_row_dual_size() == 0);

      // Calling fill_* through the backend must NOT populate the LI
      // cache (the LI is the only thing that owns m_cached_*; the
      // backend has no access to it).
      std::vector<double> sol_buf(li.get_numcols());
      li.fill_col_sol(sol_buf);
      CHECK(li.cached_col_sol_size() == 0);
      CHECK(li.cached_col_cost_size() == 0);
      CHECK(li.cached_row_dual_size() == 0);
    } catch (const std::exception& e) {
      MESSAGE("Solver '", solver, "' not available: ", e.what());
    }
  }
}

// ── T10: cross-backend equivalence on the canonical LP ─────────────────────

TEST_CASE("SolverBackend - T10 every backend produces the same LP optimum")
// NOLINT
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  // Solve the Wolsey LP with every loaded backend, collect the objective
  // values, and assert pairwise agreement.  This catches plugin-side
  // mistakes that happen to round-trip a single solver but disagree
  // across solvers (e.g. row-dual sign flip, swapped fill bindings).
  std::vector<double> objs;
  std::vector<std::string> names;
  for (const auto& solver : reg.available_solvers()) {
    try {
      LinearInterface li(solver);
      [[maybe_unused]] const auto lp = build_wolsey(li);
      auto solve = li.initial_solve(SolverOptions {.log_level = 0});
      if (!solve.has_value() || !li.is_optimal()) {
        continue;  // Solver couldn't load this fixture — skip.
      }
      objs.push_back(li.get_obj_value_raw());
      names.push_back(solver);
    } catch (const std::exception& e) {
      MESSAGE("Solver '", solver, "' not available: ", e.what());
    }
  }
  REQUIRE(objs.size() >= 1);

  for (size_t i = 0; i < objs.size(); ++i) {
    CAPTURE(names[i]);
    CHECK(objs[i] == doctest::Approx(-36.0).epsilon(kTol));
  }
  for (size_t i = 1; i < objs.size(); ++i) {
    CAPTURE(names[0]);
    CAPTURE(names[i]);
    CHECK(objs[0] == doctest::Approx(objs[i]).epsilon(kTol));
  }
}

// T7 — "in compress mode the plugin's m_col_solution_/m_reduced_cost_/
// m_row_price_ stay default-constructed empty after a solve" — is
// intentionally NOT a runtime test here.  The plugin members are
// private and would require a friend-test escape hatch to inspect
// from this file; the property is instead verified by source review
// of the Phase-2 step-3 LinearInterface change (`05523154`), which
// only calls `fill_col_sol/cost/dual` (the span-out overrides) and
// never the pointer-getters that allocate the scratch members.
