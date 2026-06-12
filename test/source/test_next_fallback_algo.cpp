/**
 * @file      test_next_fallback_algo.cpp
 * @brief     Direct unit test for the LP algorithm fallback cycle.
 * @date      2026-04-26
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `next_fallback_algo` is `constexpr` and lives in
 * @file include/gtopt/solver_enums.hpp.  Pinning it down here at
 * compile time (via `static_assert`) is the cheapest possible
 * regression net for the algorithm-fallback cycle exercised by
 * `LinearInterface::initial_solve` / `resolve`.  It fires before any
 * LP / solver backend is involved, so a mistake in the cycle table
 * surfaces as a build break, not a test failure five minutes into a
 * regression run.
 */

#include <doctest/doctest.h>
#include <gtopt/solver_enums.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ── Compile-time pin: cycle is barrier → dual → primal → barrier; ───────
// ── default_algo and last_algo both map onto dual. ─────────────────────
static_assert(next_fallback_algo(LPAlgo::barrier) == LPAlgo::dual);
static_assert(next_fallback_algo(LPAlgo::dual) == LPAlgo::primal);
static_assert(next_fallback_algo(LPAlgo::primal) == LPAlgo::barrier);
static_assert(next_fallback_algo(LPAlgo::default_algo) == LPAlgo::dual);
static_assert(next_fallback_algo(LPAlgo::last_algo) == LPAlgo::dual);

// Three steps starting from barrier completes the simplex cycle —
// confirms there is no off-by-one transition.
static_assert(
    next_fallback_algo(next_fallback_algo(next_fallback_algo(LPAlgo::barrier)))
    == LPAlgo::barrier);

// noexcept contract — required because the helper is called inside
// noexcept paths in `release_backend` / cache helpers.
static_assert(noexcept(next_fallback_algo(LPAlgo::dual)));

TEST_CASE(
    "next_fallback_algo — runtime cycle matches static_asserts")  // NOLINT
{
  // Mirror the static_asserts at runtime so a doctest summary line
  // reflects this coverage even when no compile error fires.
  CHECK(next_fallback_algo(LPAlgo::barrier) == LPAlgo::dual);
  CHECK(next_fallback_algo(LPAlgo::dual) == LPAlgo::primal);
  CHECK(next_fallback_algo(LPAlgo::primal) == LPAlgo::barrier);
  CHECK(next_fallback_algo(LPAlgo::default_algo) == LPAlgo::dual);
  CHECK(next_fallback_algo(LPAlgo::last_algo) == LPAlgo::dual);
}

TEST_CASE("next_fallback_algo — three-step cycle returns to start")  // NOLINT
{
  // Walking three steps from each "real" algorithm must land back on
  // itself (cycle period == 3 for {barrier, dual, primal}).
  for (const auto start : {LPAlgo::barrier, LPAlgo::dual, LPAlgo::primal}) {
    auto a = next_fallback_algo(start);
    a = next_fallback_algo(a);
    a = next_fallback_algo(a);
    CHECK(a == start);
  }
}
