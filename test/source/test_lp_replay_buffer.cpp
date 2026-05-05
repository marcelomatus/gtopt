// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_lp_replay_buffer.cpp
 * @brief     Unit tests for LpReplayBuffer (Phase 2a of LinearInterface split)
 * @date      2026-05-04
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Pins the R1–R6 invariants from
 * ``include/gtopt/lp_replay_buffer.hpp`` — the contract that the
 * replay buffer must enforce so the LinearInterface facade can rely
 * on it as a pure encapsulation of the dynamic-cols / dynamic-rows /
 * active-cuts / pending-col-bounds / replaying-flag state.
 */

#include <array>

#include <doctest/doctest.h>
#include <gtopt/lp_replay_buffer.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-build-using-namespace)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ── R1: default state ────────────────────────────────────────────────────

TEST_CASE("LpReplayBuffer R1 — default-constructed state")  // NOLINT
{
  const LpReplayBuffer buf {};
  CHECK(buf.dynamic_cols().empty());
  CHECK(buf.dynamic_rows().empty());
  CHECK(buf.active_cuts().empty());
  CHECK(buf.pending_col_bounds().empty());
  CHECK_FALSE(buf.replaying());
  CHECK(buf.is_empty());
  CHECK(buf.dynamic_cols_size() == 0);
  CHECK(buf.dynamic_rows_size() == 0);
  CHECK(buf.active_cuts_size() == 0);
  CHECK(buf.pending_col_bounds_size() == 0);
}

// ── R2: record_*_if_tracked is no-op under LowMemoryMode::off ────────────

TEST_CASE("LpReplayBuffer R2 — off mode short-circuit")  // NOLINT
{
  LpReplayBuffer buf {};
  SparseCol col {};
  col.cost = 1.0;
  buf.record_dynamic_col_if_tracked(col, LowMemoryMode::off);
  CHECK(buf.dynamic_cols().empty());

  SparseRow row {};
  row.lowb = 0.0;
  row.uppb = 1.0;
  buf.record_dynamic_row_if_tracked(row, LowMemoryMode::off);
  CHECK(buf.dynamic_rows().empty());

  buf.record_cut_row_if_tracked(row, LowMemoryMode::off);
  CHECK(buf.active_cuts().empty());

  // Tracked modes do append.
  buf.record_dynamic_col_if_tracked(col, LowMemoryMode::compress);
  CHECK(buf.dynamic_cols_size() == 1);
  buf.record_dynamic_row_if_tracked(row, LowMemoryMode::compress);
  CHECK(buf.dynamic_rows_size() == 1);
  buf.record_cut_row_if_tracked(row, LowMemoryMode::compress);
  CHECK(buf.active_cuts_size() == 1);

  buf.record_dynamic_col_if_tracked(col, LowMemoryMode::rebuild);
  CHECK(buf.dynamic_cols_size() == 2);
}

// ── R3: take_* moves out and leaves empty ────────────────────────────────

TEST_CASE("LpReplayBuffer R3 — take_* moves out + clears")  // NOLINT
{
  LpReplayBuffer buf {};
  SparseCol col {};
  col.cost = 5.0;
  buf.record_dynamic_col_if_tracked(col, LowMemoryMode::compress);
  buf.record_dynamic_col_if_tracked(col, LowMemoryMode::compress);
  REQUIRE(buf.dynamic_cols_size() == 2);

  auto taken = buf.take_dynamic_cols();
  CHECK(taken.size() == 2);
  CHECK(taken[0].cost == doctest::Approx(5.0));
  CHECK(buf.dynamic_cols().empty());

  // active_cuts symmetry
  SparseRow row {};
  row.uppb = 1.0;
  buf.record_cut_row_if_tracked(row, LowMemoryMode::compress);
  buf.record_cut_row_if_tracked(row, LowMemoryMode::compress);
  buf.record_cut_row_if_tracked(row, LowMemoryMode::compress);
  REQUIRE(buf.active_cuts_size() == 3);
  auto taken_cuts = buf.take_active_cuts();
  CHECK(taken_cuts.size() == 3);
  CHECK(buf.active_cuts().empty());

  // restore_* round-trip
  buf.restore_dynamic_cols(std::move(taken));
  CHECK(buf.dynamic_cols_size() == 2);
  buf.restore_active_cuts(std::move(taken_cuts));
  CHECK(buf.active_cuts_size() == 3);
}

// ── R4: update_dynamic_col_lowb / _bounds ────────────────────────────────

TEST_CASE("LpReplayBuffer R4 — update_dynamic_col_lowb / _bounds")  // NOLINT
{
  LpReplayBuffer buf {};
  SparseCol c1 {};
  c1.class_name = "alpha";
  c1.variable_name = "alpha_s1_p2";
  c1.lowb = 0.0;
  c1.uppb = 1e30;
  buf.record_dynamic_col_if_tracked(c1, LowMemoryMode::compress);

  SparseCol c2 {};
  c2.class_name = "beta";
  c2.variable_name = "other";
  c2.lowb = -10.0;
  c2.uppb = 10.0;
  buf.record_dynamic_col_if_tracked(c2, LowMemoryMode::compress);

  // First match — lowb only.
  CHECK(buf.update_dynamic_col_lowb("alpha", "alpha_s1_p2", -1e30));
  CHECK(buf.dynamic_cols()[0].lowb == doctest::Approx(-1e30));
  CHECK(buf.dynamic_cols()[0].uppb == doctest::Approx(1e30));

  // No match — returns false.
  CHECK_FALSE(buf.update_dynamic_col_lowb("alpha", "missing", 1.0));
  CHECK_FALSE(buf.update_dynamic_col_bounds("missing", "alpha_s1_p2", 0, 1));

  // Two-sided bound update.
  CHECK(buf.update_dynamic_col_bounds("beta", "other", -5.0, 5.0));
  CHECK(buf.dynamic_cols()[1].lowb == doctest::Approx(-5.0));
  CHECK(buf.dynamic_cols()[1].uppb == doctest::Approx(5.0));
}

// ── R5: record_cut_deletion ──────────────────────────────────────────────

TEST_CASE("LpReplayBuffer R5 — record_cut_deletion")  // NOLINT
{
  LpReplayBuffer buf {};
  SparseRow row {};
  row.uppb = 1.0;
  for (int i = 0; i < 5; ++i) {
    buf.record_cut_row_if_tracked(row, LowMemoryMode::compress);
  }
  REQUIRE(buf.active_cuts_size() == 5);

  // Delete cuts at global indices 12 and 14 with base_numrows=10
  // (offsets 2 and 4 in the active_cuts vector).
  const std::array<int, 2> deleted = {12, 14};
  buf.record_cut_deletion(
      deleted, /*base_numrows=*/10, LowMemoryMode::compress);
  CHECK(buf.active_cuts_size() == 3);

  // Off mode: no-op.
  buf.record_cut_deletion(deleted, /*base_numrows=*/10, LowMemoryMode::off);
  CHECK(buf.active_cuts_size() == 3);

  // Out-of-range indices silently skipped.
  const std::array<int, 1> oob = {999};
  buf.record_cut_deletion(oob, /*base_numrows=*/10, LowMemoryMode::compress);
  CHECK(buf.active_cuts_size() == 3);

  // Empty active_cuts: also no-op.
  (void)buf.take_active_cuts();
  CHECK(buf.active_cuts_size() == 0);
  buf.record_cut_deletion(
      deleted, /*base_numrows=*/10, LowMemoryMode::compress);
  CHECK(buf.active_cuts_size() == 0);
}

// ── R6: ReplayGuard RAII ─────────────────────────────────────────────────

TEST_CASE("LpReplayBuffer R6 — ReplayGuard sets + clears flag")  // NOLINT
{
  LpReplayBuffer buf {};
  CHECK_FALSE(buf.replaying());
  {
    const LpReplayBuffer::ReplayGuard guard {buf};
    CHECK(buf.replaying());
  }
  CHECK_FALSE(buf.replaying());

  // Manual set is also supported.
  buf.set_replaying(/*v=*/true);
  CHECK(buf.replaying());
  buf.set_replaying(/*v=*/false);
  CHECK_FALSE(buf.replaying());
}

// ── pending_col_bounds API ───────────────────────────────────────────────

TEST_CASE("LpReplayBuffer set_pending_col_lower / _upper")  // NOLINT
{
  LpReplayBuffer buf {};
  const auto idx = ColIndex {3};

  // Insert with lower side first; upper-now seeds the upper.
  buf.set_pending_col_lower(idx, /*new_lower=*/2.0, /*current_upper=*/100.0);
  CHECK(buf.pending_col_bounds_size() == 1);
  CHECK(buf.pending_col_bounds().at(idx).first == doctest::Approx(2.0));
  CHECK(buf.pending_col_bounds().at(idx).second == doctest::Approx(100.0));

  // Update lower; supplied upper-now is ignored when entry exists.
  buf.set_pending_col_lower(idx, /*new_lower=*/4.0, /*current_upper=*/9999.0);
  CHECK(buf.pending_col_bounds().at(idx).first == doctest::Approx(4.0));
  CHECK(buf.pending_col_bounds().at(idx).second == doctest::Approx(100.0));

  // set_pending_col_upper updates only the upper.
  buf.set_pending_col_upper(idx, /*current_lower=*/9999.0, /*new_upper=*/50.0);
  CHECK(buf.pending_col_bounds().at(idx).first == doctest::Approx(4.0));
  CHECK(buf.pending_col_bounds().at(idx).second == doctest::Approx(50.0));

  // Insert via upper-only path (different ColIndex).
  const auto idx2 = ColIndex {7};
  buf.set_pending_col_upper(idx2, /*current_lower=*/-3.0, /*new_upper=*/8.0);
  CHECK(buf.pending_col_bounds_size() == 2);
  CHECK(buf.pending_col_bounds().at(idx2).first == doctest::Approx(-3.0));
  CHECK(buf.pending_col_bounds().at(idx2).second == doctest::Approx(8.0));
}

// ── reserve_active_cuts ──────────────────────────────────────────────────

TEST_CASE("LpReplayBuffer reserve_active_cuts is a no-op size-wise")
{  // NOLINT
  LpReplayBuffer buf {};
  buf.reserve_active_cuts(128);
  CHECK(buf.active_cuts_size() == 0);  // capacity-only reservation
  CHECK(buf.active_cuts().empty());
}

}  // namespace
