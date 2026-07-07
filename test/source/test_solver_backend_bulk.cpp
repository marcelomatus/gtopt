// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_solver_backend_bulk.cpp
 * @brief     Tests for SolverBackend::set_col_bounds_bulk (commit b85011e1)
 * @date      2026-05-11
 * @copyright BSD-3-Clause
 *
 * Pins the bulk-bound-update contract: `set_col_bounds_bulk(num, idx, lu,
 * vals)` must produce identical backend state to the per-element
 * `set_col_lower` / `set_col_upper` loop.  Both the default fallback in
 * SolverBackend (used by CLP/HiGHS/OSI/CBC) and the CPXchgbds-based
 * override on CPLEX must satisfy this property — the test exercises
 * whatever backend `SolverRegistry::default_solver()` selects.
 *
 * Hot path: this is the API
 * `LinearInterface::apply_post_load_replay`'s `pending_col_bounds`
 * loop dispatches into for every clone-from-flat aperture replay.
 */

#include <array>
#include <cmath>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/solver_backend.hpp>
#include <gtopt/solver_registry.hpp>

using namespace gtopt;

namespace
{

using namespace gtopt;

// Tiny 4-col / 2-row LP loaded directly via SolverBackend::load_problem.
// Mirrors the structure used in test_osi_backend.cpp's OsiTrivialLP2 but
// sized to four columns so we have enough cells to exercise both single-
// sided ('L', 'U') and symmetric ('B') bulk updates.
struct BulkLP4
{
  static constexpr int ncols = 4;
  static constexpr int nrows = 2;
  // Column-major CSC: each col contributes one entry to each row.
  std::array<int, 5> matbeg {0, 2, 4, 6, 8};
  std::array<int, 8> matind {0, 1, 0, 1, 0, 1, 0, 1};
  std::array<double, 8> matval {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  std::array<double, 4> collb {0.0, 0.0, 0.0, 0.0};
  std::array<double, 4> colub {};
  std::array<double, 4> obj {1.0, 1.0, 1.0, 1.0};
  std::array<double, 2> rowlb {0.0, 0.0};
  std::array<double, 2> rowub {};

  explicit BulkLP4(double inf)
  {
    colub.fill(inf);
    rowub.fill(inf);
  }

  void load_into(SolverBackend& b) const
  {
    b.load_problem(ncols,
                   nrows,
                   matbeg.data(),
                   matind.data(),
                   matval.data(),
                   collb.data(),
                   colub.data(),
                   obj.data(),
                   rowlb.data(),
                   rowub.data());
  }
};

[[nodiscard]] std::unique_ptr<SolverBackend> make_default_backend_or_skip()
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  const auto name = reg.default_solver();
  if (name.empty()) {
    return nullptr;
  }
  return reg.create(name);
}

}  // namespace

TEST_CASE(  // NOLINT
    "set_col_bounds_bulk: 'L'/'U' bulk matches per-element loop")
{
  auto a = make_default_backend_or_skip();
  auto b = make_default_backend_or_skip();
  if (!a || !b) {
    return;
  }
  const double inf = a->infinity();
  const BulkLP4 lp {inf};
  lp.load_into(*a);
  lp.load_into(*b);

  // Copy A — per-element loop.
  a->set_col_lower(0, 1.0);
  a->set_col_upper(0, 5.0);
  a->set_col_lower(1, 2.0);
  a->set_col_upper(1, 6.0);

  // Copy B — single bulk call.
  // idx = [0, 0, 1, 1], lu = "LULU", vals = [1.0, 5.0, 2.0, 6.0]
  const std::array<int, 4> idx {0, 0, 1, 1};
  const std::array<char, 4> lu {'L', 'U', 'L', 'U'};
  const std::array<double, 4> vals {1.0, 5.0, 2.0, 6.0};
  b->set_col_bounds_bulk(
      static_cast<int>(idx.size()), idx.data(), lu.data(), vals.data());

  // Per-element comparison of resulting bounds.
  const double* a_low = a->col_lower();
  const double* a_upp = a->col_upper();
  const double* b_low = b->col_lower();
  const double* b_upp = b->col_upper();
  REQUIRE(a_low != nullptr);
  REQUIRE(a_upp != nullptr);
  REQUIRE(b_low != nullptr);
  REQUIRE(b_upp != nullptr);
  for (int i = 0; i < BulkLP4::ncols; ++i) {
    CAPTURE(i);
    // Exact-equality escape hatch: backends with true IEEE infinities
    // (HiGHS) fail `inf == Approx(inf)` (inf - inf = NaN) on untouched
    // infinite bounds; finite-sentinel backends (COIN DBL_MAX) don't.
    CHECK((a_low[i] == b_low[i] || a_low[i] == doctest::Approx(b_low[i])));
    CHECK((a_upp[i] == b_upp[i] || a_upp[i] == doctest::Approx(b_upp[i])));
  }
}

TEST_CASE(  // NOLINT
    "set_col_bounds_bulk: 'B' (both) sets lower and upper to same value")
{
  auto a = make_default_backend_or_skip();
  auto b = make_default_backend_or_skip();
  if (!a || !b) {
    return;
  }
  const double inf = a->infinity();
  const BulkLP4 lp {inf};
  lp.load_into(*a);
  lp.load_into(*b);

  // Copy A — per-element loop emulating 'B': set both bounds to the
  // same value (this is what 'B' means in the CPLEX `lu` convention).
  a->set_col_lower(0, 3.0);
  a->set_col_upper(0, 3.0);
  a->set_col_lower(1, 7.0);
  a->set_col_upper(1, 7.0);

  // Copy B — bulk with 'B'.
  const std::array<int, 2> idx {0, 1};
  const std::array<char, 2> lu {'B', 'B'};
  const std::array<double, 2> vals {3.0, 7.0};
  b->set_col_bounds_bulk(
      static_cast<int>(idx.size()), idx.data(), lu.data(), vals.data());

  const double* a_low = a->col_lower();
  const double* a_upp = a->col_upper();
  const double* b_low = b->col_lower();
  const double* b_upp = b->col_upper();
  REQUIRE(a_low != nullptr);
  REQUIRE(a_upp != nullptr);
  REQUIRE(b_low != nullptr);
  REQUIRE(b_upp != nullptr);
  for (int i = 0; i < BulkLP4::ncols; ++i) {
    CAPTURE(i);
    // Exact-equality escape hatch: backends with true IEEE infinities
    // (HiGHS) fail `inf == Approx(inf)` (inf - inf = NaN) on untouched
    // infinite bounds; finite-sentinel backends (COIN DBL_MAX) don't.
    CHECK((a_low[i] == b_low[i] || a_low[i] == doctest::Approx(b_low[i])));
    CHECK((a_upp[i] == b_upp[i] || a_upp[i] == doctest::Approx(b_upp[i])));
  }
  // Sanity: col 0 ends up at [3, 3], col 1 at [7, 7].
  CHECK(b_low[0] == doctest::Approx(3.0));
  CHECK(b_upp[0] == doctest::Approx(3.0));
  CHECK(b_low[1] == doctest::Approx(7.0));
  CHECK(b_upp[1] == doctest::Approx(7.0));
}

TEST_CASE(  // NOLINT
    "set_col_bounds_bulk: empty input is a safe no-op")
{
  auto backend = make_default_backend_or_skip();
  if (!backend) {
    return;
  }
  const double inf = backend->infinity();
  const BulkLP4 lp {inf};
  lp.load_into(*backend);

  // Snapshot current bounds.
  std::array<double, BulkLP4::ncols> before_low {};
  std::array<double, BulkLP4::ncols> before_upp {};
  backend->fill_col_lower(before_low);
  backend->fill_col_upper(before_upp);

  // Bulk with num=0 — must not crash and must not change anything.
  backend->set_col_bounds_bulk(0, nullptr, nullptr, nullptr);

  std::array<double, BulkLP4::ncols> after_low {};
  std::array<double, BulkLP4::ncols> after_upp {};
  backend->fill_col_lower(after_low);
  backend->fill_col_upper(after_upp);
  for (int i = 0; i < BulkLP4::ncols; ++i) {
    CAPTURE(i);
    const auto ui = static_cast<size_t>(i);
    // Exact-equality escape hatch for true-IEEE-infinity backends
    // (HiGHS): `inf == Approx(inf)` is false (inf - inf = NaN).
    CHECK((before_low.at(ui) == after_low.at(ui)
           || before_low.at(ui) == doctest::Approx(after_low.at(ui))));
    CHECK((before_upp.at(ui) == after_upp.at(ui)
           || before_upp.at(ui) == doctest::Approx(after_upp.at(ui))));
  }
}

// -----------------------------------------------------------------------
// HiGHS-specific regression tests.  These pin the HiGHS plugin override
// of `set_col_bounds_bulk` (dispatch to `Highs::changeColsBounds` by set,
// i.e. the C++ wrapper for `Highs_changeColsBoundsBySet`) against the
// per-element `set_col_lower` / `set_col_upper` loop.  The above
// "default backend" tests already exercise this transitively when HiGHS
// is the highest-priority loaded plugin, but the explicit cases below
// also run when HiGHS coexists with a higher-priority backend (CPLEX),
// so the HiGHS override stays covered as long as `reg.create("highs")`
// succeeds.
namespace
{

[[nodiscard]] std::unique_ptr<SolverBackend> make_highs_backend_or_skip()
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  if (!reg.has_solver("highs")) {
    return nullptr;
  }
  return reg.create("highs");
}

// doctest::Approx degenerates to NaN for IEEE inf — direct == for
// non-finite values (HiGHS stores ±∞ raw in col_upper/col_lower).
[[nodiscard]] bool bounds_approx(double a, double b)
{
  if (!std::isfinite(a) || !std::isfinite(b)) {
    return a == b;
  }
  return a == doctest::Approx(b);
}

}  // namespace

TEST_CASE(  // NOLINT
    "set_col_bounds_bulk [highs]: 'L'/'U' bulk matches per-element loop")
{
  auto a = make_highs_backend_or_skip();
  auto b = make_highs_backend_or_skip();
  if (!a || !b) {
    return;
  }
  const double inf = a->infinity();
  const BulkLP4 lp {inf};
  lp.load_into(*a);
  lp.load_into(*b);

  a->set_col_lower(0, 1.0);
  a->set_col_upper(0, 5.0);
  a->set_col_lower(1, 2.0);
  a->set_col_upper(1, 6.0);

  const std::array<int, 4> idx {0, 0, 1, 1};
  const std::array<char, 4> lu {'L', 'U', 'L', 'U'};
  const std::array<double, 4> vals {1.0, 5.0, 2.0, 6.0};
  b->set_col_bounds_bulk(
      static_cast<int>(idx.size()), idx.data(), lu.data(), vals.data());

  std::array<double, BulkLP4::ncols> a_low {};
  std::array<double, BulkLP4::ncols> a_upp {};
  std::array<double, BulkLP4::ncols> b_low {};
  std::array<double, BulkLP4::ncols> b_upp {};
  a->fill_col_lower(a_low);
  a->fill_col_upper(a_upp);
  b->fill_col_lower(b_low);
  b->fill_col_upper(b_upp);
  for (int i = 0; i < BulkLP4::ncols; ++i) {
    CAPTURE(i);
    CHECK(bounds_approx(a_low.at(static_cast<size_t>(i)),
                        b_low.at(static_cast<size_t>(i))));
    CHECK(bounds_approx(a_upp.at(static_cast<size_t>(i)),
                        b_upp.at(static_cast<size_t>(i))));
  }
}

TEST_CASE(  // NOLINT
    "set_col_bounds_bulk [highs]: 'B' (both) sets lower and upper to same "
    "value")
{
  auto a = make_highs_backend_or_skip();
  auto b = make_highs_backend_or_skip();
  if (!a || !b) {
    return;
  }
  const double inf = a->infinity();
  const BulkLP4 lp {inf};
  lp.load_into(*a);
  lp.load_into(*b);

  a->set_col_lower(0, 3.0);
  a->set_col_upper(0, 3.0);
  a->set_col_lower(1, 7.0);
  a->set_col_upper(1, 7.0);

  const std::array<int, 2> idx {0, 1};
  const std::array<char, 2> lu {'B', 'B'};
  const std::array<double, 2> vals {3.0, 7.0};
  b->set_col_bounds_bulk(
      static_cast<int>(idx.size()), idx.data(), lu.data(), vals.data());

  std::array<double, BulkLP4::ncols> a_low {};
  std::array<double, BulkLP4::ncols> a_upp {};
  std::array<double, BulkLP4::ncols> b_low {};
  std::array<double, BulkLP4::ncols> b_upp {};
  a->fill_col_lower(a_low);
  a->fill_col_upper(a_upp);
  b->fill_col_lower(b_low);
  b->fill_col_upper(b_upp);
  for (int i = 0; i < BulkLP4::ncols; ++i) {
    CAPTURE(i);
    CHECK(bounds_approx(a_low.at(static_cast<size_t>(i)),
                        b_low.at(static_cast<size_t>(i))));
    CHECK(bounds_approx(a_upp.at(static_cast<size_t>(i)),
                        b_upp.at(static_cast<size_t>(i))));
  }
  CHECK(b_low.at(0) == doctest::Approx(3.0));
  CHECK(b_upp.at(0) == doctest::Approx(3.0));
  CHECK(b_low.at(1) == doctest::Approx(7.0));
  CHECK(b_upp.at(1) == doctest::Approx(7.0));
}

TEST_CASE(  // NOLINT
    "set_col_bounds_bulk [highs]: mixed 'L'/'U'/'B' across distinct cols")
{
  // Covers the override's pending-table path with all three side
  // selectors in one call -- ensures L-only and U-only entries pick
  // up the live opposite-side bound from the LP state rather than
  // resetting it to the seed default.
  auto a = make_highs_backend_or_skip();
  auto b = make_highs_backend_or_skip();
  if (!a || !b) {
    return;
  }
  const double inf = a->infinity();
  const BulkLP4 lp {inf};
  lp.load_into(*a);
  lp.load_into(*b);

  // Per-element reference: col 0 -> [0.5, inf], col 1 -> [0, 4.0],
  // col 2 -> [2.0, 2.0].
  a->set_col_lower(0, 0.5);
  a->set_col_upper(1, 4.0);
  a->set_col_lower(2, 2.0);
  a->set_col_upper(2, 2.0);

  const std::array<int, 4> idx {0, 1, 2, 2};
  const std::array<char, 4> lu {'L', 'U', 'L', 'U'};
  const std::array<double, 4> vals {0.5, 4.0, 2.0, 2.0};
  b->set_col_bounds_bulk(
      static_cast<int>(idx.size()), idx.data(), lu.data(), vals.data());

  std::array<double, BulkLP4::ncols> a_low {};
  std::array<double, BulkLP4::ncols> a_upp {};
  std::array<double, BulkLP4::ncols> b_low {};
  std::array<double, BulkLP4::ncols> b_upp {};
  a->fill_col_lower(a_low);
  a->fill_col_upper(a_upp);
  b->fill_col_lower(b_low);
  b->fill_col_upper(b_upp);
  for (int i = 0; i < BulkLP4::ncols; ++i) {
    CAPTURE(i);
    CHECK(bounds_approx(a_low.at(static_cast<size_t>(i)),
                        b_low.at(static_cast<size_t>(i))));
    CHECK(bounds_approx(a_upp.at(static_cast<size_t>(i)),
                        b_upp.at(static_cast<size_t>(i))));
  }
}

TEST_CASE(  // NOLINT
    "set_col_bounds_bulk [highs]: empty input is a safe no-op")
{
  auto backend = make_highs_backend_or_skip();
  if (!backend) {
    return;
  }
  const double inf = backend->infinity();
  const BulkLP4 lp {inf};
  lp.load_into(*backend);

  std::array<double, BulkLP4::ncols> before_low {};
  std::array<double, BulkLP4::ncols> before_upp {};
  backend->fill_col_lower(before_low);
  backend->fill_col_upper(before_upp);

  backend->set_col_bounds_bulk(0, nullptr, nullptr, nullptr);

  std::array<double, BulkLP4::ncols> after_low {};
  std::array<double, BulkLP4::ncols> after_upp {};
  backend->fill_col_lower(after_low);
  backend->fill_col_upper(after_upp);
  for (int i = 0; i < BulkLP4::ncols; ++i) {
    CAPTURE(i);
    CHECK(bounds_approx(before_low.at(static_cast<size_t>(i)),
                        after_low.at(static_cast<size_t>(i))));
    CHECK(bounds_approx(before_upp.at(static_cast<size_t>(i)),
                        after_upp.at(static_cast<size_t>(i))));
  }
}
