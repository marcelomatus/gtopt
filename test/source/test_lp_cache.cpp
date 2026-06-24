// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_lp_cache.cpp
 * @brief     Unit tests for LpCache (Phase 1 of LinearInterface split)
 * @date      2026-05-04
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Pins the C1–C8 invariants from
 * ``docs/linear_interface_split_plan.md`` §3.2 — the contract that
 * ``LpCache`` must enforce so the LinearInterface facade can rely on
 * it as a pure encapsulation of the post-solve cache state.
 */

#include <cstddef>
#include <optional>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/lp_cache.hpp>

using namespace gtopt;  // NOLINT(google-build-using-namespace)
// NOLINTBEGIN(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ── C8: default-constructed state ────────────────────────────────────────

TEST_CASE("LpCache C8 — default-constructed state")  // NOLINT
{
  const LpCache cache {};
  CHECK(cache.obj_value() == doctest::Approx(0.0));
  CHECK_FALSE(cache.kappa().has_value());
  CHECK(cache.numrows() == Index {0});
  CHECK(cache.numcols() == Index {0});
  CHECK_FALSE(cache.is_optimal());
  CHECK_FALSE(cache.backend_solution_fresh());
  CHECK(cache.col_sol().empty());
  CHECK(cache.col_cost().empty());
  CHECK(cache.row_dual().empty());
  CHECK(cache.size_bytes() == 0);
  CHECK(cache.validate_consistency());
}

// ── Scalar setters round-trip ────────────────────────────────────────────

TEST_CASE("LpCache scalar setters round-trip")  // NOLINT
{
  LpCache cache {};
  cache.set_obj_value(123.456);
  cache.set_kappa(std::optional<double> {1.5e-7});
  cache.set_numrows(Index {42});
  cache.set_numcols(Index {17});
  CHECK(cache.obj_value() == doctest::Approx(123.456));
  CHECK(
      (cache.kappa().has_value() && *cache.kappa() == doctest::Approx(1.5e-7)));
  CHECK(cache.numrows() == Index {42});
  CHECK(cache.numcols() == Index {17});

  cache.set_kappa(std::nullopt);
  CHECK_FALSE(cache.kappa().has_value());
}

// ── C4: only set_is_optimal flips the flag to true ───────────────────────

TEST_CASE("LpCache C4 — set_is_optimal is the only path to true")  // NOLINT
{
  LpCache cache {};
  CHECK_FALSE(cache.is_optimal());

  cache.set_is_optimal(/*v=*/true);
  CHECK(cache.is_optimal());

  cache.set_is_optimal(/*v=*/false);
  CHECK_FALSE(cache.is_optimal());

  cache.set_is_optimal(/*v=*/true);
  // Other mutators do NOT flip true → false except invalidate_*.
  cache.set_obj_value(1.0);
  CHECK(cache.is_optimal());
  cache.set_numrows(Index {5});
  CHECK(cache.is_optimal());

  // invalidate fires on flag, leaves vectors alone (C2).
  cache.set_is_optimal(/*v=*/true);
  cache.invalidate_optimal_on_mutation();
  CHECK_FALSE(cache.is_optimal());
}

// ── C2: invalidate is idempotent + does NOT clear vectors ─────────────────

TEST_CASE("LpCache C2 — invalidate idempotent, vectors preserved")  // NOLINT
{
  LpCache cache {};
  // Populate vectors via the buffer API.
  const auto sp_sol = cache.col_sol_buffer(3);
  sp_sol[0] = 1.0;
  sp_sol[1] = 2.0;
  sp_sol[2] = 3.0;
  const auto sp_cost = cache.col_cost_buffer(3);
  sp_cost[0] = 0.1;
  const auto sp_dual = cache.row_dual_buffer(2);
  sp_dual[0] = -0.5;
  sp_dual[1] = -0.6;
  cache.set_is_optimal(/*v=*/true);

  // First invalidate.
  cache.invalidate_optimal_on_mutation();
  CHECK_FALSE(cache.is_optimal());
  CHECK(cache.col_sol().size() == 3);
  CHECK(cache.col_cost().size() == 3);
  CHECK(cache.row_dual().size() == 2);
  CHECK(cache.col_sol()[0] == doctest::Approx(1.0));
  CHECK(cache.row_dual()[1] == doctest::Approx(-0.6));

  // Second invalidate is a no-op.
  cache.invalidate_optimal_on_mutation();
  CHECK_FALSE(cache.is_optimal());
  CHECK(cache.col_sol().size() == 3);
  CHECK(cache.col_cost().size() == 3);
  CHECK(cache.row_dual().size() == 2);
}

// ── C3: drop_solution_caches clears col_sol/col_cost only ────────────────

TEST_CASE("LpCache C3 — drop_solution_caches preserves row_dual + scalars")
{  // NOLINT
  LpCache cache {};
  const auto sp_sol = cache.col_sol_buffer(2);
  sp_sol[0] = 1.0;
  sp_sol[1] = 2.0;
  const auto sp_cost = cache.col_cost_buffer(2);
  sp_cost[0] = 0.5;
  sp_cost[1] = 0.6;
  const auto sp_dual = cache.row_dual_buffer(3);
  sp_dual[0] = -1.0;
  sp_dual[1] = -2.0;
  sp_dual[2] = -3.0;
  cache.set_obj_value(99.0);
  cache.set_numrows(Index {3});
  cache.set_numcols(Index {2});
  cache.set_is_optimal(/*v=*/true);

  cache.drop_solution_caches();

  // Cleared
  CHECK(cache.col_sol().empty());
  CHECK(cache.col_cost().empty());
  // Preserved
  CHECK(cache.row_dual().size() == 3);
  CHECK(cache.row_dual()[2] == doctest::Approx(-3.0));
  CHECK(cache.obj_value() == doctest::Approx(99.0));
  CHECK(cache.numrows() == Index {3});
  CHECK(cache.numcols() == Index {2});
  CHECK(cache.is_optimal());  // optimality flag survives the drop
}

// ── C7: span-out write buffers resize + return writable spans ─────────────

TEST_CASE("LpCache C7 — col_sol_buffer / col_cost_buffer / row_dual_buffer")
{  // NOLINT
  LpCache cache {};

  // First call resizes from 0.
  auto sp1 = cache.col_sol_buffer(5);
  CHECK(sp1.size() == 5);
  CHECK(cache.col_sol().size() == 5);
  for (std::size_t i = 0; i < sp1.size(); ++i) {
    sp1[i] = static_cast<double>(i);
  }
  for (std::size_t i = 0; i < sp1.size(); ++i) {
    CHECK(cache.col_sol()[i] == doctest::Approx(static_cast<double>(i)));
  }

  // Second call resizes to a different size — old data is preserved up
  // to the smaller of old/new size (std::vector::resize semantics);
  // the contract here is that after the call the span has the
  // requested size and is writable.
  auto sp2 = cache.col_sol_buffer(8);
  CHECK(sp2.size() == 8);
  CHECK(cache.col_sol().size() == 8);

  // Empty buffer is allowed.
  auto sp3 = cache.col_sol_buffer(0);
  CHECK(sp3.empty());
  CHECK(cache.col_sol().empty());

  // col_cost_buffer / row_dual_buffer follow the same contract.
  auto sp_cost = cache.col_cost_buffer(4);
  CHECK(sp_cost.size() == 4);
  auto sp_dual = cache.row_dual_buffer(7);
  CHECK(sp_dual.size() == 7);
  CHECK(cache.col_cost().size() == 4);
  CHECK(cache.row_dual().size() == 7);

  // col_low_buffer / col_upp_buffer (added 2026-05-07 via eaa70b4a so
  // that `populate_solution_cache_post_solve` snapshots the
  // construction-time column bounds via the new
  // `SolverBackend::fill_col_lower` / `fill_col_upper` span-out APIs
  // — without these the LI's `get_col_low_raw` / `get_col_upp_raw` and
  // the `get_col_sol()` clamp path force the CPLEX/MindOpt/Gurobi
  // backend to allocate a per-instance `numcols`-sized scratch on
  // every read).  Same C7 contract as the three solution buffers.
  auto sp_lo = cache.col_low_buffer(3);
  CHECK(sp_lo.size() == 3);
  for (std::size_t i = 0; i < sp_lo.size(); ++i) {
    sp_lo[i] = -static_cast<double>(i);  // arbitrary negative pattern
  }
  for (std::size_t i = 0; i < cache.col_low().size(); ++i) {
    CHECK(cache.col_low()[i] == doctest::Approx(-static_cast<double>(i)));
  }

  auto sp_up = cache.col_upp_buffer(3);
  CHECK(sp_up.size() == 3);
  for (std::size_t i = 0; i < sp_up.size(); ++i) {
    sp_up[i] = 1000.0 + static_cast<double>(i);
  }
  for (std::size_t i = 0; i < cache.col_upp().size(); ++i) {
    CHECK(cache.col_upp()[i]
          == doctest::Approx(1000.0 + static_cast<double>(i)));
  }

  // Empty buffer is allowed (mirror of col_sol_buffer(0) above).
  auto sp_lo_empty = cache.col_low_buffer(0);
  CHECK(sp_lo_empty.empty());
  CHECK(cache.col_low().empty());
}

// ── C5: mark_solution_fresh tracks live backend ──────────────────────────

TEST_CASE("LpCache C5 — mark_solution_fresh round-trips")  // NOLINT
{
  LpCache cache {};
  CHECK_FALSE(cache.backend_solution_fresh());

  cache.mark_solution_fresh(/*v=*/true);
  CHECK(cache.backend_solution_fresh());

  cache.mark_solution_fresh(/*v=*/false);
  CHECK_FALSE(cache.backend_solution_fresh());

  // Independent of is_optimal: a backend can be loaded but not solved
  // (fresh=false, optimal=true means cache holds the prior solve).
  cache.set_is_optimal(/*v=*/true);
  cache.mark_solution_fresh(/*v=*/false);
  CHECK(cache.is_optimal());
  CHECK_FALSE(cache.backend_solution_fresh());
}

// ── C6: size_bytes accurate ──────────────────────────────────────────────

TEST_CASE("LpCache C6 — size_bytes accurate")  // NOLINT
{
  LpCache cache {};
  CHECK(cache.size_bytes() == 0);

  (void)cache.col_sol_buffer(3);
  CHECK(cache.size_bytes() == 3 * sizeof(double));

  (void)cache.col_cost_buffer(2);
  CHECK(cache.size_bytes() == (3 + 2) * sizeof(double));

  (void)cache.row_dual_buffer(5);
  CHECK(cache.size_bytes() == (3 + 2 + 5) * sizeof(double));

  // col_low / col_upp added by eaa70b4a — must be reflected in
  // `size_bytes()` so the TUI memory dashboard accounts for them.
  (void)cache.col_low_buffer(3);
  CHECK(cache.size_bytes() == (3 + 2 + 5 + 3) * sizeof(double));

  (void)cache.col_upp_buffer(3);
  CHECK(cache.size_bytes() == (3 + 2 + 5 + 3 + 3) * sizeof(double));

  cache.drop_solution_caches();
  // col_sol + col_cost cleared by `drop_solution_caches`; row_dual,
  // col_low, and col_upp remain.  This is the "drop the largest
  // primal/cost cache, keep duals + bounds" contract — duals are
  // still needed for output writing and bounds are still needed for
  // any subsequent `get_col_sol()` clamp.
  CHECK(cache.size_bytes() == (5 + 3 + 3) * sizeof(double));

  cache.clear_all_solution_vectors();
  CHECK(cache.size_bytes() == 0);
}

// ── C1: validate_consistency on optimal state ────────────────────────────

TEST_CASE("LpCache C1 — validate_consistency on optimal state")  // NOLINT
{
  LpCache cache {};

  // Default: optimal=false → trivially consistent.
  CHECK(cache.validate_consistency());

  // Optimal but no vectors yet: still consistent (vectors empty is the
  // post-`drop_solution_caches` state — readers fall back via the empty
  // path).
  cache.set_numrows(Index {3});
  cache.set_numcols(Index {2});
  cache.set_is_optimal(/*v=*/true);
  CHECK(cache.validate_consistency());

  // Optimal with col_sol size matching numcols: consistent.
  (void)cache.col_sol_buffer(2);
  CHECK(cache.validate_consistency());

  // Optimal with col_sol size mismatch: inconsistent.
  (void)cache.col_sol_buffer(99);
  CHECK_FALSE(cache.validate_consistency());

  // Reset to consistent, then break row_dual.
  (void)cache.col_sol_buffer(2);
  CHECK(cache.validate_consistency());
  (void)cache.row_dual_buffer(99);
  CHECK_FALSE(cache.validate_consistency());

  // Once optimality is invalidated, validate_consistency is true
  // regardless of vector sizes.
  cache.invalidate_optimal_on_mutation();
  CHECK(cache.validate_consistency());
}

// ── clear_all_solution_vectors clears all 3 vectors ──────────────────────

TEST_CASE("LpCache clear_all_solution_vectors")  // NOLINT
{
  LpCache cache {};
  (void)cache.col_sol_buffer(2);
  (void)cache.col_cost_buffer(2);
  (void)cache.row_dual_buffer(3);
  // Bound caches added by eaa70b4a — `clear_all_solution_vectors`
  // MUST clear them too, otherwise a `release_backend` cycle that
  // intends to drop the entire cached solve would silently retain
  // stale bounds and the next-cycle `get_col_low_raw()` reader would
  // see pre-mutation bounds.
  (void)cache.col_low_buffer(2);
  (void)cache.col_upp_buffer(2);
  cache.set_obj_value(7.0);
  cache.set_is_optimal(/*v=*/true);

  cache.clear_all_solution_vectors();

  CHECK(cache.col_sol().empty());
  CHECK(cache.col_cost().empty());
  CHECK(cache.row_dual().empty());
  CHECK(cache.col_low().empty());
  CHECK(cache.col_upp().empty());
  // Scalars + optimality flag survive — caller must invalidate them
  // separately if desired.
  CHECK(cache.obj_value() == doctest::Approx(7.0));
  CHECK(cache.is_optimal());
}

// ── clear_col_bounds_cache evicts ONLY the column-bound vectors ───────────
TEST_CASE("LpCache clear_col_bounds_cache evicts only col bounds")  // NOLINT
{
  // Backs the ab46e0d1a fix: `reconstruct_backend()` must evict the
  // stale post-solve column-bound snapshot (which may hold presolve-
  // tightened finite values for a structurally unbounded column) while
  // leaving the primal/dual solution vectors untouched.  This locks the
  // method's surface deterministically; the LinearInterface-level
  // reconstruct test exercises the call site.
  LpCache cache;
  auto sol = cache.col_sol_buffer(2);
  sol[0] = 1.0;
  sol[1] = 2.0;
  auto cost = cache.col_cost_buffer(2);
  cost[0] = 3.0;
  cost[1] = 4.0;
  auto dual = cache.row_dual_buffer(1);
  dual[0] = 5.0;
  auto lo = cache.col_low_buffer(2);
  lo[0] = 3.0;
  lo[1] = 3.0;
  auto up = cache.col_upp_buffer(2);
  up[0] = 8.0;
  up[1] = 8.0;
  REQUIRE(cache.col_low().size() == 2);
  REQUIRE(cache.col_upp().size() == 2);

  cache.clear_col_bounds_cache();

  // Column-bound caches are gone …
  CHECK(cache.col_low().empty());
  CHECK(cache.col_upp().empty());
  // … but the solution / cost / dual vectors are preserved (they are
  // gated separately on `is_optimal()`).
  CHECK(cache.col_sol().size() == 2);
  CHECK(cache.col_cost().size() == 2);
  CHECK(cache.row_dual().size() == 1);

  // Idempotent: a second clear on already-empty bounds is a no-op.
  cache.clear_col_bounds_cache();
  CHECK(cache.col_low().empty());
  CHECK(cache.col_upp().empty());
}

}  // namespace

// NOLINTEND(google-global-names-in-headers)