// SPDX-License-Identifier: BSD-3-Clause
//
// Round-trip tests for SparseRow's row-sense setters
// (`.equal()` / `.less_equal()` / `.greater_equal()`) through the
// full LP build pipeline — struct → LinearProblem::add_row →
// flatten → backend → LinearInterface introspection.
//
// The struct-level tests in `test_sparse_row.cpp` only check that
// `.equal()` / `.less_equal()` / `.greater_equal()` set the
// `SparseRow::lowb` / `SparseRow::uppb` members correctly.  They
// do NOT verify that the backend actually receives a row of the
// right sense, and they do NOT cover what happens when the row
// gets mutated post-flatten via the `set_row_low` / `set_row_upp`
// / `set_row_bounds` / `set_row_equal_to` family.
//
// That gap was the RALCO discharge-limit bug (commit 488555548):
// `ReservoirDischargeLimitLP` constructed rows with
// ``.less_equal(intercept)``, then `update_lp` re-issued the
// per-phase RHS via the legacy ``li.set_rhs(row, new_rhs)`` API,
// which silently rewrote BOTH row bounds to the new RHS, flipping
// `<=` rows to equalities — so RALCO's volume-dependent DCMax cap
// became a hard equality demand and any phase where the turbine
// was in maintenance (RALCO_pmax = 24.64 MW at block 141 in
// `plp_2_years`) reported "elastic filter produced no feasibility
// cut" on SDDP forward.  The legacy ``set_rhs`` name was removed;
// callers now pick the explicit setter that matches their intent
// (``set_row_upp`` / ``set_row_low`` for one-sided update,
// ``set_row_equal_to`` for force-to-equality, ``set_row_bounds``
// for an explicit two-sided update).
//
// These tests pin:
//   1. `.equal()`     → row reaches backend as ``(rhs, rhs)``
//   2. `.less_equal()`→ row reaches backend as ``(-inf, ub)``
//   3. `.greater_equal()` → row reaches backend as ``(lb, +inf)``
//   4. `set_row_upp(row, v)` preserves a `<=` row (RDL-correct pattern)
//   5. `set_row_low(row, v)` preserves a `>=` row (symmetric)
//   6. `set_row_equal_to(row, v)` forces a ``<=`` row to ``(v, v)``.
//   7. `set_row_equal_to(row, v)` on an ``.equal()`` row refreshes
//      the equality target (Seepage call-site contract).
//   8. `set_row_bounds(row, lb, ub)` writes both bounds independently.

#include <array>
#include <cmath>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;

namespace
{

// 2-column toy LP that any solver backend can accept after
// `flatten`.  Cols are unscaled (scale = 1.0) so the backend sees
// the same numeric bounds we wrote into the SparseRow.
struct TwoColLP
{
  LinearProblem lp {"row_sense_roundtrip"};
  ColIndex x {};
  ColIndex y {};

  TwoColLP()
  {
    x = lp.add_col(SparseCol {
        .uppb = 100.0,
        .cost = 1.0,
    });
    y = lp.add_col(SparseCol {
        .uppb = 100.0,
        .cost = 1.0,
    });
  }

  // Build a LinearInterface around the flattened LP so callers can
  // mutate row bounds and introspect via ``get_row_low_raw`` /
  // ``get_row_upp_raw``.
  [[nodiscard]] LinearInterface make_li()
  {
    return LinearInterface {"", lp.flatten({})};
  }
};

// Backend treats DblMax (and CPX_INFBOUND / HiGHS infinity etc.)
// as "no bound" — `LinearProblem::add_row` normalizes ``DblMax`` to
// the configured backend infinity.  ``infy_tol`` is large enough to
// catch any value that is operationally unbounded; if a sense bug
// leaked a finite RHS into the wrong side, the assertion would
// catch it because such a value would be far below this threshold.
constexpr double kInfyTol = 1e20;

}  // namespace

TEST_SUITE("SparseRow row-sense round-trip")
{
  // ── 1. ``.equal(rhs)`` round-trip ────────────────────────────────
  TEST_CASE(  // NOLINT
      "SparseRow.equal() round-trip — backend receives (rhs, rhs)")
  {
    TwoColLP fix;
    auto r = SparseRow {};
    r[fix.x] = 1.0;
    r[fix.y] = 1.0;
    r.equal(2.5);
    const auto row = fix.lp.add_row(std::move(r));

    auto li = fix.make_li();
    const auto lows = li.get_row_low();
    const auto upps = li.get_row_upp();

    CHECK(lows[row] == doctest::Approx(2.5));
    CHECK(upps[row] == doctest::Approx(2.5));
  }

  // ── 2. ``.less_equal(ub)`` round-trip ────────────────────────────
  TEST_CASE(  // NOLINT
      "SparseRow.less_equal() round-trip — backend receives (-inf, ub)")
  {
    TwoColLP fix;
    auto r = SparseRow {};
    r[fix.x] = 1.0;
    r[fix.y] = 1.0;
    r.less_equal(5.0);
    const auto row = fix.lp.add_row(std::move(r));

    auto li = fix.make_li();
    const auto lows = li.get_row_low();
    const auto upps = li.get_row_upp();

    // Lower bound should be operationally -infinity.
    CHECK(lows[row] <= -kInfyTol);
    // Upper bound is the explicit RHS we set.
    CHECK(upps[row] == doctest::Approx(5.0));
  }

  // ── 3. ``.greater_equal(lb)`` round-trip ─────────────────────────
  TEST_CASE(  // NOLINT
      "SparseRow.greater_equal() round-trip — backend receives (lb, +inf)")
  {
    TwoColLP fix;
    auto r = SparseRow {};
    r[fix.x] = 1.0;
    r[fix.y] = 1.0;
    r.greater_equal(3.0);
    const auto row = fix.lp.add_row(std::move(r));

    auto li = fix.make_li();
    const auto lows = li.get_row_low();
    const auto upps = li.get_row_upp();

    CHECK(lows[row] == doctest::Approx(3.0));
    CHECK(upps[row] >= kInfyTol);
  }

  // ── 4. set_row_upp on a ``<=`` row PRESERVES sense ────────────────
  //
  // This is the pattern ``ReservoirDischargeLimitLP::update_lp``
  // uses after commit 488555548: the row was emitted with
  // ``.less_equal(intercept)`` in `add_to_lp` and ``set_row_upp``
  // refreshes the upper bound at each phase without touching the
  // lower bound.  ``flow_b - slope · efin ≤ new_rhs`` semantics
  // survives the mutation.
  TEST_CASE(  // NOLINT
      "set_row_upp on a less_equal row preserves the <= sense")
  {
    TwoColLP fix;
    auto r = SparseRow {};
    r[fix.x] = 1.0;
    r[fix.y] = 1.0;
    r.less_equal(5.0);
    const auto row = fix.lp.add_row(std::move(r));
    auto li = fix.make_li();

    // Refresh the RHS (e.g. volume-dependent DCMax in RDL).
    li.set_row_upp(row, 72.0);

    const auto lows = li.get_row_low();
    const auto upps = li.get_row_upp();

    // Lower bound still -infinity → row is still ``<=``.
    CHECK(lows[row] <= -kInfyTol);
    CHECK(upps[row] == doctest::Approx(72.0));
  }

  // ── 5. set_row_low on a ``>=`` row PRESERVES sense ────────────────
  TEST_CASE(  // NOLINT
      "set_row_low on a greater_equal row preserves the >= sense")
  {
    TwoColLP fix;
    auto r = SparseRow {};
    r[fix.x] = 1.0;
    r[fix.y] = 1.0;
    r.greater_equal(3.0);
    const auto row = fix.lp.add_row(std::move(r));
    auto li = fix.make_li();

    li.set_row_low(row, 12.5);

    const auto lows = li.get_row_low();
    const auto upps = li.get_row_upp();

    CHECK(lows[row] == doctest::Approx(12.5));
    CHECK(upps[row] >= kInfyTol);
  }

  // ── 6. set_row_equal_to forces a ``<=`` row to equality ─────────
  //
  // Confirms ``set_row_equal_to`` is the explicit force-to-equality
  // operation: a ``<=`` row's ``-inf`` lower bound gets overwritten
  // and the row becomes ``lowb = uppb = value``.  This is the
  // semantics that used to live under the misleading ``set_rhs``
  // name (removed in this commit — see commit 488555548 for the
  // RALCO regression that motivated the rename: RDL used
  // ``li.set_rhs`` to refresh a ``.less_equal()`` row's RHS and
  // silently flipped DCMax(V) into a hard equality).
  TEST_CASE(  // NOLINT
      "set_row_equal_to forces a less_equal row to equality at value")
  {
    TwoColLP fix;
    auto r = SparseRow {};
    r[fix.x] = 1.0;
    r[fix.y] = 1.0;
    r.less_equal(5.0);
    const auto row = fix.lp.add_row(std::move(r));
    auto li = fix.make_li();

    li.set_row_equal_to(row, 72.0);

    const auto lows = li.get_row_low();
    const auto upps = li.get_row_upp();

    CHECK(lows[row] == doctest::Approx(72.0));
    CHECK(upps[row] == doctest::Approx(72.0));
  }

  // ── 7. set_row_equal_to refreshes an ``.equal()`` row target ────
  //
  // Covers the Seepage call site (`reservoir_seepage_lp.cpp:213`):
  // the row is constructed with ``.equal(rhs)`` and
  // ``set_row_equal_to(row, new_rhs)`` updates the equality target.
  TEST_CASE(  // NOLINT
      "set_row_equal_to updates the equality target on an equal() row")
  {
    TwoColLP fix;
    auto r = SparseRow {};
    r[fix.x] = 1.0;
    r[fix.y] = 1.0;
    r.equal(2.5);
    const auto row = fix.lp.add_row(std::move(r));
    auto li = fix.make_li();

    li.set_row_equal_to(row, 9.0);

    const auto lows = li.get_row_low();
    const auto upps = li.get_row_upp();

    CHECK(lows[row] == doctest::Approx(9.0));
    CHECK(upps[row] == doctest::Approx(9.0));
  }

  // ── 8. set_row_bounds — generic two-sided setter ────────────────
  //
  // ``set_row_bounds(row, lb, ub)`` mirrors the bulk
  // ``set_col_bounds`` API and lets callers turn any row into a
  // range constraint (or a one-sided constraint by passing ±inf).
  TEST_CASE(  // NOLINT
      "set_row_bounds writes both bounds independently")
  {
    TwoColLP fix;
    auto r = SparseRow {};
    r[fix.x] = 1.0;
    r[fix.y] = 1.0;
    r.equal(2.5);
    const auto row = fix.lp.add_row(std::move(r));
    auto li = fix.make_li();

    li.set_row_bounds(row, -3.0, 8.5);

    const auto lows = li.get_row_low();
    const auto upps = li.get_row_upp();

    CHECK(lows[row] == doctest::Approx(-3.0));
    CHECK(upps[row] == doctest::Approx(8.5));
  }
}
