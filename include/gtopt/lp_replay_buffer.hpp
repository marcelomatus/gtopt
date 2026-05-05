/**
 * @file      lp_replay_buffer.hpp
 * @brief     LpReplayBuffer — replay state for LowMemoryMode reconstruct
 * @date      2026-05-04
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `LpReplayBuffer` owns the 5 replay-on-reconstruct fields that
 * previously lived directly on `LinearInterface`:
 *
 *   * `m_dynamic_cols_`    — columns added after `load_flat` (typically α).
 *   * `m_dynamic_rows_`    — structural rows added after `load_flat`
 *                            (e.g. cascade elastic-target constraints).
 *   * `m_active_cuts_`     — net active Benders cuts.
 *   * `m_pending_col_bounds_` — overrides applied via
 *                              `set_col_low_raw`/`set_col_upp_raw` after
 *                              `load_flat` so they survive the
 *                              release→reconstruct cycle.
 *   * `m_replaying_`       — defence-in-depth flag set inside
 *                            `apply_post_load_replay`'s bulk replay so
 *                            the auto-record path on `add_col`/`add_row`
 *                            short-circuits.
 *
 * Extraction is Phase 2a of the `LinearInterface` split (B2) — see
 * `docs/linear_interface_split_plan.md` §4 for the broader plan.
 *
 * Lifecycle:
 *   * Default-constructed: all empty / false.
 *   * Under `LowMemoryMode::off` the buffer stays empty (the live
 *     backend is the source of truth).  All `record_*_if_tracked`
 *     methods take the current `LowMemoryMode` so they can short-
 *     circuit cleanly without forcing the caller to gate.
 *   * Under `compress`/`snapshot`/`rebuild`, every dynamic addition is
 *     mirrored here and replayed onto the freshly-loaded backend by
 *     `LinearInterface::apply_post_load_replay()`.
 *
 * Invariants (asserted in debug, see `validate_consistency()`):
 *   * **R1.**  Default-constructed `LpReplayBuffer{}` reports all
 *              vectors empty, `replaying() == false`, and zero
 *              pending col bounds.
 *   * **R2.**  `record_*_if_tracked(..., mode)` is a no-op when
 *              `mode == LowMemoryMode::off`.
 *   * **R3.**  `take_dynamic_cols()` / `take_active_cuts()` move-out
 *              the internal vector and leave the buffer in the
 *              default-constructed state for that field.
 *   * **R4.**  `update_dynamic_col_lowb(...)` /
 * `update_dynamic_col_bounds(...)` return `true` iff a matching `(class_name,
 *              variable_name)` entry was found and updated.  Linear
 *              search — replay buffers stay tiny (≤ a few alpha cols).
 *   * **R5.**  `record_cut_deletion(deleted, base_numrows, mode)` is a
 *              no-op when `mode == off` or `active_cuts` is empty.
 *              Otherwise it removes the cuts at the supplied global
 *              row indices (offset by `base_numrows` to get the
 *              `m_active_cuts_` index).
 *   * **R6.**  `set_replaying(true)` / `set_replaying(false)` flip the
 *              flag idempotently.  An RAII helper
 *              `ReplayGuard` is provided for scope-bound use inside
 *              `apply_post_load_replay`.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/sddp_enums.hpp>  // LowMemoryMode
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

namespace gtopt
{

/**
 * @brief Replay state for `LinearInterface` low-memory reconstruct.
 *
 * Owns the dynamic-cols / dynamic-rows / active-cuts vectors, the
 * pending-col-bounds map, and the in-replay flag.  See the file-
 * level comment for the contract.
 */
class LpReplayBuffer
{
public:
  // ── Read accessors (const, non-mutating) ────────────────────────────────

  [[nodiscard]] auto dynamic_cols() const noexcept -> std::span<const SparseCol>
  {
    return m_dynamic_cols_;
  }
  [[nodiscard]] auto dynamic_rows() const noexcept -> std::span<const SparseRow>
  {
    return m_dynamic_rows_;
  }
  [[nodiscard]] auto active_cuts() const noexcept -> std::span<const SparseRow>
  {
    return m_active_cuts_;
  }
  [[nodiscard]] auto pending_col_bounds() const noexcept
      -> const std::map<ColIndex, std::pair<double, double>>&
  {
    return m_pending_col_bounds_;
  }
  [[nodiscard]] constexpr auto replaying() const noexcept -> bool
  {
    return m_replaying_;
  }
  [[nodiscard]] auto active_cuts_size() const noexcept -> std::size_t
  {
    return m_active_cuts_.size();
  }
  [[nodiscard]] auto dynamic_cols_size() const noexcept -> std::size_t
  {
    return m_dynamic_cols_.size();
  }
  [[nodiscard]] auto dynamic_rows_size() const noexcept -> std::size_t
  {
    return m_dynamic_rows_.size();
  }
  [[nodiscard]] auto pending_col_bounds_size() const noexcept -> std::size_t
  {
    return m_pending_col_bounds_.size();
  }

  // ── Mutable spans (used by replay loop in apply_post_load_replay) ───────

  /// Mutable reference to the dynamic-cols vector — needed by the
  /// `add_cols(span)` call in `apply_post_load_replay` which consumes
  /// the span by value (and may extend `m_col_scales_` based on
  /// `col.scale != 1.0` entries).
  [[nodiscard]] auto dynamic_cols_mut() noexcept -> std::vector<SparseCol>&
  {
    return m_dynamic_cols_;
  }
  [[nodiscard]] auto dynamic_rows_mut() noexcept -> std::vector<SparseRow>&
  {
    return m_dynamic_rows_;
  }
  [[nodiscard]] auto active_cuts_mut() noexcept -> std::vector<SparseRow>&
  {
    return m_active_cuts_;
  }

  // ── Conditional record (R2) — no-op under LowMemoryMode::off ────────────

  void record_dynamic_col_if_tracked(SparseCol col, LowMemoryMode mode)
  {
    if (mode != LowMemoryMode::off) {
      m_dynamic_cols_.push_back(std::move(col));
    }
  }

  void record_dynamic_row_if_tracked(SparseRow row, LowMemoryMode mode)
  {
    if (mode != LowMemoryMode::off) {
      m_dynamic_rows_.push_back(std::move(row));
    }
  }

  void record_cut_row_if_tracked(SparseRow row, LowMemoryMode mode)
  {
    if (mode != LowMemoryMode::off) {
      m_active_cuts_.push_back(std::move(row));
    }
  }

  /// **R5** — drop cut entries at global row indices `deleted_indices`,
  /// offset by `base_numrows` to get the `active_cuts` index.  Indices
  /// outside `[base_numrows, base_numrows + active_cuts.size())` are
  /// silently skipped (defence against caller bugs).  No-op under
  /// `LowMemoryMode::off` or when `active_cuts` is empty.
  void record_cut_deletion(std::span<const int> deleted_indices,
                           int base_numrows,
                           LowMemoryMode mode)
  {
    if (mode == LowMemoryMode::off || m_active_cuts_.empty()) {
      return;
    }
    std::vector<std::size_t> offsets;
    offsets.reserve(deleted_indices.size());
    for (const auto idx : deleted_indices) {
      const auto off = static_cast<std::size_t>(idx - base_numrows);
      if (off < m_active_cuts_.size()) {
        offsets.push_back(off);
      }
    }
    std::ranges::sort(offsets, std::greater {});
    for (const auto off : offsets) {
      m_active_cuts_.erase(m_active_cuts_.begin()
                           + static_cast<std::ptrdiff_t>(off));
    }
  }

  // ── Reservations ────────────────────────────────────────────────────────

  void reserve_active_cuts(std::size_t n) { m_active_cuts_.reserve(n); }

  // ── Targeted updates of dynamic columns (R4) ────────────────────────────

  /// Update the lower bound of the first matching dynamic column.
  /// Returns true iff the entry was found.  Linear search — buffer
  /// stays tiny in practice (≤ a few alpha cols).
  [[nodiscard]] bool update_dynamic_col_lowb(std::string_view class_name,
                                             std::string_view variable_name,
                                             double new_lowb) noexcept
  {
    for (auto& col : m_dynamic_cols_) {
      if (col.class_name == class_name && col.variable_name == variable_name) {
        col.lowb = new_lowb;
        return true;
      }
    }
    return false;
  }

  /// Update both bounds of the first matching dynamic column.
  /// Returns true iff the entry was found.
  [[nodiscard]] bool update_dynamic_col_bounds(std::string_view class_name,
                                               std::string_view variable_name,
                                               double new_lowb,
                                               double new_uppb) noexcept
  {
    for (auto& col : m_dynamic_cols_) {
      if (col.class_name == class_name && col.variable_name == variable_name) {
        col.lowb = new_lowb;
        col.uppb = new_uppb;
        return true;
      }
    }
    return false;
  }

  // ── Pending col-bound tracking ──────────────────────────────────────────
  //
  // The lower / upper-bound setters on LinearInterface need to read the
  // backend's current "other side" bound when seeding a fresh entry,
  // so the body of those methods stays on LinearInterface.  The map
  // operations themselves live here.

  /// Insert or update the lower bound for `idx`.  When inserting a
  /// fresh entry, the caller supplies the current upper bound from the
  /// live backend so a single-sided update doesn't reset the unset
  /// side to zero on replay.
  void set_pending_col_lower(ColIndex idx,
                             double new_lower,
                             double current_upper) noexcept
  {
    auto it = m_pending_col_bounds_.find(idx);
    if (it == m_pending_col_bounds_.end()) {
      m_pending_col_bounds_.emplace(
          idx, std::pair<double, double> {new_lower, current_upper});
    } else {
      it->second.first = new_lower;
    }
  }

  /// Insert or update the upper bound for `idx`.  When inserting a
  /// fresh entry, the caller supplies the current lower bound from the
  /// live backend.
  void set_pending_col_upper(ColIndex idx,
                             double current_lower,
                             double new_upper) noexcept
  {
    auto it = m_pending_col_bounds_.find(idx);
    if (it == m_pending_col_bounds_.end()) {
      m_pending_col_bounds_.emplace(
          idx, std::pair<double, double> {current_lower, new_upper});
    } else {
      it->second.second = new_upper;
    }
  }

  // ── Take + restore (rebuild path) (R3) ──────────────────────────────────

  /// Move out the dynamic columns and leave the internal vector empty.
  [[nodiscard]] auto take_dynamic_cols() noexcept -> std::vector<SparseCol>
  {
    return std::exchange(m_dynamic_cols_, {});
  }

  /// Move out the active cuts and leave the internal vector empty.
  [[nodiscard]] auto take_active_cuts() noexcept -> std::vector<SparseRow>
  {
    return std::exchange(m_active_cuts_, {});
  }

  void restore_dynamic_cols(std::vector<SparseCol> cols) noexcept
  {
    m_dynamic_cols_ = std::move(cols);
  }

  void restore_active_cuts(std::vector<SparseRow> cuts) noexcept
  {
    m_active_cuts_ = std::move(cuts);
  }

  // ── Replay flag (R6) + RAII guard ───────────────────────────────────────

  void set_replaying(bool v) noexcept { m_replaying_ = v; }

  /// RAII helper for scope-bound use inside `apply_post_load_replay`.
  /// Prefer this over manual `set_replaying(true)` / `set_replaying(false)`
  /// to ensure the flag is always cleared even on exceptions.
  class [[nodiscard]] ReplayGuard
  {
  public:
    explicit ReplayGuard(LpReplayBuffer& buf) noexcept
        : m_buf_(buf)
    {
      m_buf_.set_replaying(/*v=*/true);
    }
    ~ReplayGuard() noexcept { m_buf_.set_replaying(/*v=*/false); }
    ReplayGuard(const ReplayGuard&) = delete;
    ReplayGuard(ReplayGuard&&) = delete;
    ReplayGuard& operator=(const ReplayGuard&) = delete;
    ReplayGuard& operator=(ReplayGuard&&) = delete;

  private:
    LpReplayBuffer& m_buf_;
  };

  // ── Diagnostic ──────────────────────────────────────────────────────────

  /// **R1** debug self-check.  Returns true iff the buffer is in the
  /// default-constructed state.
  [[nodiscard]] auto is_empty() const noexcept -> bool
  {
    return m_dynamic_cols_.empty() && m_dynamic_rows_.empty()
        && m_active_cuts_.empty() && m_pending_col_bounds_.empty()
        && !m_replaying_;
  }

private:
  std::vector<SparseCol> m_dynamic_cols_ {};
  std::vector<SparseRow> m_dynamic_rows_ {};
  std::vector<SparseRow> m_active_cuts_ {};
  std::map<ColIndex, std::pair<double, double>> m_pending_col_bounds_ {};
  bool m_replaying_ {false};
};

}  // namespace gtopt
