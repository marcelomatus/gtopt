// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      lp_name_spill.hpp
 * @brief     Async, Parquet-backed store for LP col/row label metadata.
 * @date      2026-06-28
 * @copyright BSD-3-Clause
 *
 * The label metadata (`SparseColLabel` / `SparseRowLabel` — class name,
 * variable / constraint name, uid, context) is needed ONLY to write a
 * readable `.lp` dump (the `lp_error` / `lp_debug` error/debug LPs), but
 * holding it resident across a full SDDP run (it lives in every cell's
 * `FlatLP.label_meta`) costs hundreds of MB at CEN scale.  This store lets a
 * cell hand its metadata off to a BACKGROUND worker thread that writes it to a
 * small Parquet file in a temp directory (so the caller can immediately free
 * its copy and drop `FlatLP.label_meta`), then reads it back on demand —
 * cached, so the per-(aperture / forward / backward) error-LP writes for one
 * cell only touch disk once.
 *
 * Persistence reuses gtopt's existing Apache Arrow / Parquet stack (the same
 * `parquet::arrow::WriteTable` / `parquet_read_table` helpers the solution and
 * cut I/O use).  The metadata is stored DECOMPOSED as a long-format table so
 * its low-cardinality fields dictionary-compress to almost nothing:
 *   `kind: int8` (0 = column, 1 = row),
 *   `class_ptr: int64`, `class_len: uint8`,
 *   `name_ptr: int64`, `name_len: uint8` (variable_name for cols,
 *   constraint_name for rows),
 *   `uid: int32`, `context: fixed_size_binary(sizeof(LpContext))`
 *   (the trivially-copyable `LpContext` variant, memcpy'd opaque).
 * `write_lp` re-synthesises the formatted names from this metadata via
 * `LabelMaker` on load.
 *
 * @note **The name string_views are stored as raw `(pointer, length)`, not as
 * their bytes.**  Every `class_name` / `variable_name` / `constraint_name` in
 * the label metadata is a `static constexpr std::string_view` (an element
 * class name like `Bus::class_name.full_name()` or a constexpr LP-name
 * constant like `BalanceName`) — static storage whose address is fixed for the
 * whole program.  The spill file is a temp file written AND read by the same
 * running process, so the round-tripped pointer is always valid on load.  This
 * keeps the store self-contained without copying or re-interning name bytes.
 * If a non-static name view is ever added to the LP layer, this assumption
 * breaks — but `write_lp` already dereferences these same views from
 * `FlatLP.label_meta` across the run, so run-stability is already required.
 *
 * Gated entirely by `lp_error || lp_debug`: when both are off no store is
 * created and no label metadata is generated, saved or loaded.
 */
#pragma once

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

namespace gtopt
{

/// Decomposed col/row label metadata for one LP cell (the structural fields
/// `write_lp` re-formats into readable names).
///
/// The `string_view`s inside `col_labels` / `row_labels` point at the static
/// `constexpr` name constants of the LP layer (see the file header), so they
/// stay valid for the program's lifetime — no backing pool is needed and the
/// struct is freely movable.
struct LpLabelMeta
{
  std::vector<SparseColLabel> col_labels {};
  std::vector<SparseRowLabel> row_labels {};

  [[nodiscard]] bool empty() const noexcept
  {
    return col_labels.empty() && row_labels.empty();
  }
};

/// Async, Parquet-backed store for LP col/row label metadata.
///
/// Thread-safe.  `spill` enqueues an off-thread Parquet write and returns at
/// once; `load` blocks only until the matching spill has been flushed, then
/// caches the loaded metadata so repeated writes for the same key are free.
class LpNameSpillStore
{
public:
  /// Metadata Parquet files land under `dir` (created if missing) and the
  /// whole directory is removed on destruction.
  explicit LpNameSpillStore(std::filesystem::path dir);
  ~LpNameSpillStore();

  LpNameSpillStore(const LpNameSpillStore&) = delete;
  LpNameSpillStore& operator=(const LpNameSpillStore&) = delete;
  LpNameSpillStore(LpNameSpillStore&&) = delete;
  LpNameSpillStore& operator=(LpNameSpillStore&&) = delete;

  /// Enqueue an async Parquet write of `meta` under `key`.  Returns
  /// immediately; the worker thread builds the Arrow table and writes the
  /// Parquet file.  `meta` is moved into the queue, so the caller can drop its
  /// copy.
  void spill(std::string key, LpLabelMeta meta);

  /// Load (cached) the metadata for `key`, blocking until any still-pending
  /// spill for `key` has been written.  Returns nullptr if `key` was never
  /// spilled.  The returned pointer is stable for the lifetime of the store.
  [[nodiscard]] const LpLabelMeta* load(const std::string& key);

  /// Block until every queued spill has been flushed to disk (test/teardown
  /// helper).
  void drain();

private:
  void worker_loop(const std::stop_token& stop);
  /// Parquet stem (no extension) for `key`; `parquet_read_table` appends it.
  [[nodiscard]] std::filesystem::path stem_for(const std::string& key) const;

  std::filesystem::path m_dir_;

  mutable std::mutex m_mtx_;
  std::condition_variable m_cv_;
  std::deque<std::pair<std::string, LpLabelMeta>> m_queue_;
  std::set<std::string> m_pending_;  ///< keys queued/in-flight, not yet on disk
  std::set<std::string> m_known_;  ///< every key ever spilled (for load miss)
  std::map<std::string, LpLabelMeta> m_cache_;  ///< loaded metadata
  std::jthread m_worker_;
};

}  // namespace gtopt
