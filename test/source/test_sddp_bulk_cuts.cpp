/**
 * @file      test_sddp_bulk_cuts.cpp
 * @brief     LP-semantic regression tests for the SDDP bulk-install path.
 * @copyright BSD-3-Clause
 *
 * Guards the migration of `SDDPMethod::forward_pass`'s multi-cut
 * feasibility-cut installer from a per-cut `add_cut_row` loop to a
 * single `add_rows` dispatch + per-cut bookkeeping (commit
 * 3781e496 "perf(sddp): bulk-install multi-cut feasibility cuts").
 *
 * The migration relies on three invariants:
 *
 *   1. `add_rows(span)` followed by per-row `record_cut_row(row)`
 *      produces the same LP-backend state and the same `m_active_cuts_`
 *      contents as a per-cut `add_cut_row(row)` loop.
 *
 *   2. The row indices returned by the bulk dispatch are
 *      `[first_row, first_row + N)` with `first_row = get_numrows()`
 *      captured before the bulk call — i.e. sequential, no holes.
 *
 *   3. Under `LowMemoryMode::compress`, the bulk-installed cuts survive
 *      a `release_backend` / `reconstruct_backend` round-trip via
 *      `replay_active_cuts`.
 *
 * Tests below cover each invariant with a synthetic 1-col / 1-row base
 * LP and 3 cut rows; the LP semantics are deliberately trivial so a
 * regression in the bulk path produces an obvious mismatch (cuts on
 * the wrong row, missing record, divergent coefficients).
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/solver_options.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// Build a 2-col / 1-row base LP, flatten, and load into the given
// LinearInterface so subsequent `add_row` / `add_rows` calls land on
// the post-flatten cut-installation path (which is what
// `SDDPMethod::forward_pass` exercises after structural build).
//
//   min  x + y
//   s.t. x + y >= 1,  0 <= x, y <= 10
void load_base_lp(LinearInterface& li)
{
  LinearProblem lp;
  const auto cx = lp.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });
  const auto cy = lp.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });
  const auto r0 = lp.add_row(SparseRow {
      .lowb = 1.0,
      .uppb = SparseRow::DblMax,
  });
  lp.set_coeff(r0, cx, 1.0);
  lp.set_coeff(r0, cy, 1.0);
  auto flat = lp.flatten({});
  li.load_flat(flat);
  li.save_base_numrows();
}

// 3 cut rows with distinct, non-trivial coefficients so a mis-indexing
// or coefficient swap in the bulk path produces an unambiguous mismatch.
[[nodiscard]] std::vector<SparseRow> make_three_cuts()
{
  const auto cx = ColIndex {0};
  const auto cy = ColIndex {1};

  SparseRow c0 {
      .lowb = 2.0,
      .uppb = SparseRow::DblMax,
  };
  c0[cx] = 3.0;
  c0[cy] = 0.5;

  SparseRow c1 {
      .lowb = 4.0,
      .uppb = SparseRow::DblMax,
  };
  c1[cx] = 1.0;
  c1[cy] = 7.0;

  SparseRow c2 {
      .lowb = -1.0,
      .uppb = 5.0,
  };  // two-sided
  c2[cx] = 2.0;
  c2[cy] = -1.0;

  return {
      std::move(c0),
      std::move(c1),
      std::move(c2),
  };
}

}  // namespace

// ---------------------------------------------------------------------------
// Invariant 1 + 2: bulk path matches per-cut path
// ---------------------------------------------------------------------------
TEST_CASE(
    "SDDP bulk cuts — bulk add_rows + record_cut_row matches per-cut "
    "add_cut_row")  // NOLINT
{
  // Both LIs need `LowMemoryMode::compress` so `record_cut_row` actually
  // pushes into `m_active_cuts_` (under `mode == off` the recorder is a
  // no-op and we couldn't compare cut registries).
  LinearInterface li_a;
  load_base_lp(li_a);
  li_a.set_low_memory(LowMemoryMode::compress);

  LinearInterface li_b;
  load_base_lp(li_b);
  li_b.set_low_memory(LowMemoryMode::compress);

  const auto cuts = make_three_cuts();

  // Path A: per-cut `add_cut_row` (the pre-migration loop shape).
  const auto first_a = li_a.get_numrows();
  for (const auto& cut : cuts) {
    li_a.add_cut_row(cut);
  }

  // Path B: bulk `add_rows` + per-cut `record_cut_row` (the post-migration
  // loop shape used by `SDDPMethod::forward_pass`).
  const auto first_b = li_b.get_numrows();
  li_b.add_rows(cuts);
  for (const auto& cut : cuts) {
    li_b.record_cut_row(cut);
  }

  // Final row count must match.
  CHECK(li_a.get_numrows() == li_b.get_numrows());
  CHECK(first_a == first_b);
  CHECK(li_a.get_numrows() == first_a + cuts.size());

  // Per-row coefficient comparison — cuts must land on the same row
  // indices with the same physical values.
  const auto cx = ColIndex {0};
  const auto cy = ColIndex {1};
  for (size_t k = 0; k < cuts.size(); ++k) {
    const auto row_a =
        RowIndex {static_cast<Index>(first_a) + static_cast<Index>(k)};
    const auto row_b =
        RowIndex {static_cast<Index>(first_b) + static_cast<Index>(k)};
    CHECK(li_a.get_coeff(row_a, cx)
          == doctest::Approx(li_b.get_coeff(row_b, cx)));
    CHECK(li_a.get_coeff(row_a, cy)
          == doctest::Approx(li_b.get_coeff(row_b, cy)));
  }

  // m_active_cuts_ must contain the same cut rows in the same order.
  // `take_active_cuts()` returns a vector of SparseRow — comparing sizes
  // catches a missed `record_cut_row` and comparing per-row [cx] / [cy]
  // values catches a dropped coefficient.
  const auto active_a = li_a.take_active_cuts();
  const auto active_b = li_b.take_active_cuts();
  REQUIRE(active_a.size() == cuts.size());
  REQUIRE(active_b.size() == cuts.size());
  for (size_t k = 0; k < cuts.size(); ++k) {
    CHECK(active_a[k].cmap.at(cx) == doctest::Approx(active_b[k].cmap.at(cx)));
    CHECK(active_a[k].cmap.at(cy) == doctest::Approx(active_b[k].cmap.at(cy)));
    CHECK(active_a[k].lowb == doctest::Approx(active_b[k].lowb));
    CHECK(active_a[k].uppb == doctest::Approx(active_b[k].uppb));
  }
}

// ---------------------------------------------------------------------------
// Invariant 3: bulk-installed cuts survive release / reconstruct
// ---------------------------------------------------------------------------
TEST_CASE(
    "SDDP bulk cuts — release/reconstruct round-trip preserves bulk "
    "cuts via replay_active_cuts")  // NOLINT
{
  LinearInterface li;
  load_base_lp(li);
  li.set_low_memory(LowMemoryMode::compress);

  const auto cuts = make_three_cuts();
  const auto first_row = li.get_numrows();

  // Drive the bulk path manually (mirrors `SDDPMethod::forward_pass`).
  li.add_rows(cuts);
  for (const auto& cut : cuts) {
    li.record_cut_row(cut);
  }
  REQUIRE(li.get_numrows() == first_row + cuts.size());

  // Capture pre-release coefficients for comparison.
  const auto cx = ColIndex {0};
  const auto cy = ColIndex {1};
  std::vector<std::pair<double, double>> pre_release;
  pre_release.reserve(cuts.size());
  for (size_t k = 0; k < cuts.size(); ++k) {
    const auto row =
        RowIndex {static_cast<Index>(first_row) + static_cast<Index>(k)};
    pre_release.emplace_back(li.get_coeff(row, cx), li.get_coeff(row, cy));
  }

  // Snapshot + release, then reconstruct.  The replay path
  // (`replay_active_cuts` → bulk `add_rows(m_active_cuts_)`) is what
  // restores the cut rows on the fresh backend.
  LinearProblem base;
  std::ignore = base.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });
  std::ignore = base.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
  });
  const auto r0 = base.add_row(SparseRow {
      .lowb = 1.0,
      .uppb = SparseRow::DblMax,
  });
  base.set_coeff(r0, ColIndex {0}, 1.0);
  base.set_coeff(r0, ColIndex {1}, 1.0);
  auto base_flat = base.flatten({});
  li.save_snapshot(FlatLinearProblem {base_flat});
  li.release_backend();
  REQUIRE(li.is_backend_released());

  li.reconstruct_backend();
  REQUIRE_FALSE(li.is_backend_released());

  // Row count must match: base (1) + cuts (3) = 4.
  CHECK(li.get_numrows() == first_row + cuts.size());

  // Coefficients on each cut row must round-trip exactly (the bulk
  // replay applies the same physical → LP transform as the original
  // installation).
  for (size_t k = 0; k < cuts.size(); ++k) {
    const auto row =
        RowIndex {static_cast<Index>(first_row) + static_cast<Index>(k)};
    CHECK(li.get_coeff(row, cx) == doctest::Approx(pre_release[k].first));
    CHECK(li.get_coeff(row, cy) == doctest::Approx(pre_release[k].second));
  }
}

// ---------------------------------------------------------------------------
// Defensive: empty span is a no-op
// ---------------------------------------------------------------------------
TEST_CASE("SDDP bulk cuts — empty span is a no-op")  // NOLINT
{
  LinearInterface li;
  load_base_lp(li);
  li.set_low_memory(LowMemoryMode::compress);

  const auto rows_before = li.get_numrows();

  // The `if (!mc_cuts.empty())` guard in `SDDPMethod::forward_pass`
  // prevents this in production, but the underlying primitive must
  // also be safe against an accidental empty span call.
  std::vector<SparseRow> empty;
  li.add_rows(empty);

  CHECK(li.get_numrows() == rows_before);

  // No `record_cut_row` calls in the empty case, so m_active_cuts_
  // must be empty.
  const auto active = li.take_active_cuts();
  CHECK(active.empty());
}

// ---------------------------------------------------------------------------
// eps filtering parity: bulk and singular paths drop tiny coefficients
// the same way (matters because `SDDPMethod::forward_pass` passes
// `m_options_.cut_coeff_eps` through to `add_rows`).
// ---------------------------------------------------------------------------
TEST_CASE("SDDP bulk cuts — eps filter matches singular add_cut_row")  // NOLINT
{
  LinearInterface li_a;
  load_base_lp(li_a);
  li_a.set_low_memory(LowMemoryMode::compress);

  LinearInterface li_b;
  load_base_lp(li_b);
  li_b.set_low_memory(LowMemoryMode::compress);

  // Cut whose y-coefficient is below the eps filter.  Both paths must
  // drop it identically (no entry on column 1 in either backend).
  const double eps = 1e-6;
  const auto cx = ColIndex {0};
  const auto cy = ColIndex {1};

  SparseRow tiny {
      .lowb = 1.0,
      .uppb = SparseRow::DblMax,
  };
  tiny[cx] = 1.0;
  tiny[cy] = 1e-12;  // below eps → must be filtered

  const auto first_a = li_a.get_numrows();
  li_a.add_cut_row(tiny, eps);

  const auto first_b = li_b.get_numrows();
  std::array<SparseRow, 1> tiny_arr {tiny};
  li_b.add_rows(tiny_arr, eps);
  li_b.record_cut_row(tiny);

  const auto row_a = RowIndex {static_cast<Index>(first_a)};
  const auto row_b = RowIndex {static_cast<Index>(first_b)};

  // The y-coefficient must be 0 in both backends (filtered out).
  CHECK(li_a.get_coeff(row_a, cy) == doctest::Approx(0.0));
  CHECK(li_b.get_coeff(row_b, cy) == doctest::Approx(0.0));

  // The x-coefficient (well above eps) must survive identically.
  CHECK(li_a.get_coeff(row_a, cx)
        == doctest::Approx(li_b.get_coeff(row_b, cx)));
}
