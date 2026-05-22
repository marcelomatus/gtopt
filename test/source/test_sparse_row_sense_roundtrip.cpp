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
// / `set_rhs` family.
//
// That gap was the RALCO discharge-limit bug (commit 488555548):
// `ReservoirDischargeLimitLP` constructed rows with
// ``.less_equal(intercept)``, then `update_lp` re-issued the
// per-phase RHS via `li.set_rhs(row, new_rhs)`.  `set_rhs_raw`
// silently rewrote BOTH row bounds to the new RHS, flipping
// `<=` rows to equalities — so RALCO's volume-dependent DCMax cap
// became a hard equality demand and any phase where the turbine
// was in maintenance (RALCO_pmax = 24.64 MW at block 141 in
// `plp_2_years`) reported "elastic filter produced no feasibility
// cut" on SDDP forward.
//
// These tests pin:
//   1. `.equal()`     → row reaches backend as ``(rhs, rhs)``
//   2. `.less_equal()`→ row reaches backend as ``(-inf, ub)``
//   3. `.greater_equal()` → row reaches backend as ``(lb, +inf)``
//   4. `set_row_upp(row, v)` preserves a `<=` row (RDL-correct pattern)
//   5. `set_row_low(row, v)` preserves a `>=` row (symmetric)
//   6. `set_rhs(row, v)`  ≡ force-to-equality.  Regression-anchor
//      for the documented footgun in `check_solvers.cpp` and the
//      misuse that caused the RALCO bug.

#include <array>
#include <cmath>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
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

  // ── 6. set_rhs is "force-to-equality" — regression anchor ─────────
  //
  // Documents the current ``LinearInterface::set_rhs_raw`` contract
  // (also pinned by `check_solvers.cpp` ~line 271): both row
  // bounds are rewritten to ``rhs``, so a previously ``<=`` row
  // is silently converted to ``= rhs`` after the call.
  //
  // This is the trap that bit RDL before commit 488555548 — RDL
  // built rows with ``.less_equal(intercept)`` then called
  // ``li.set_rhs(row, new_rhs)`` on every phase, flipping
  // ``flow ≤ DCMax(V)`` to ``flow == DCMax(V)``.  Callers updating
  // a ``<=`` row should use ``set_row_upp`` (or symmetrically
  // ``set_row_low`` for ``>=``); callers updating an ``.equal()``
  // row can keep using ``set_rhs``.  If you flip ``set_rhs`` to
  // "preserve sense", this test will fail and you'll know to
  // re-audit every caller.
  TEST_CASE(  // NOLINT
      "set_rhs on a less_equal row forces it to equality (current contract)")
  {
    TwoColLP fix;
    auto r = SparseRow {};
    r[fix.x] = 1.0;
    r[fix.y] = 1.0;
    r.less_equal(5.0);
    const auto row = fix.lp.add_row(std::move(r));
    auto li = fix.make_li();

    li.set_rhs(row, 72.0);

    const auto lows = li.get_row_low();
    const auto upps = li.get_row_upp();

    // ``set_rhs`` forces both bounds to the same value — the
    // ``-inf`` lower bound from ``.less_equal()`` is overwritten.
    CHECK(lows[row] == doctest::Approx(72.0));
    CHECK(upps[row] == doctest::Approx(72.0));
  }

  // ── 7. set_rhs on an ``.equal()`` row is a no-op semantic-wise ────
  //
  // Confirms the Seepage call site (`reservoir_seepage_lp.cpp:213`)
  // is safe: the row is built with ``.equal(rhs)`` so
  // ``set_rhs(row, new_rhs)`` correctly updates the equality target.
  TEST_CASE(  // NOLINT
      "set_rhs on an equal() row updates the equality target")
  {
    TwoColLP fix;
    auto r = SparseRow {};
    r[fix.x] = 1.0;
    r[fix.y] = 1.0;
    r.equal(2.5);
    const auto row = fix.lp.add_row(std::move(r));
    auto li = fix.make_li();

    li.set_rhs(row, 9.0);

    const auto lows = li.get_row_low();
    const auto upps = li.get_row_upp();

    CHECK(lows[row] == doctest::Approx(9.0));
    CHECK(upps[row] == doctest::Approx(9.0));
  }
}
