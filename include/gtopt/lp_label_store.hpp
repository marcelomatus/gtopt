/**
 * @file      lp_label_store.hpp
 * @brief     Name / label subsystem of an LP matrix
 * @date      2026-07-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Value aggregate holding the name-to-index maps and the structural
 * label metadata captured from a `FlatLinearProblem` after flatten,
 * plus the compressed backups and string pool used by the low-memory
 * path.  Extracted from `LinearInterface` as step 3 of decomposing that
 * class (mirrors the `MatrixStats` precedent).
 *
 * Behaviour is preserved verbatim — this is a pure data grouping:
 *   * The COW-shared maps / frozen flatten-time metadata are
 *     `shared_ptr`-wrapped and `mutable` so shallow clones share state
 *     via atomic incref and const lazy-materialisation caches can write.
 *   * `post_flatten_col_labels_meta` / `post_flatten_row_labels_meta`
 *     are PER-INSTANCE plain value vectors — never `shared_ptr`, never
 *     `mutable`.  A copy of `LpLabelStore` value-copies them, so each
 *     clone owns its own post-flatten history independently of the
 *     source, matching the aperture-clone semantics.
 */

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtopt/memory_compress.hpp>  // CompressedBuffer
#include <gtopt/sparse_col.hpp>  // SparseColLabel, ColIndex
#include <gtopt/sparse_row.hpp>  // SparseRowLabel, RowIndex
#include <gtopt/strong_index_vector.hpp>  // StrongIndexVector

namespace gtopt
{

class LpNameSpillStore;  // async label-metadata spill store (non-owning)

/// Column (variable) name → strong column index map.
using col_name_map_t = std::unordered_map<std::string, ColIndex>;
/// Row (constraint) name → strong row index map.
using row_name_map_t = std::unordered_map<std::string, RowIndex>;

/// Name / label subsystem of a `LinearInterface`.  Groups the
/// name-to-index maps, the frozen flatten-time label metadata, the
/// per-instance post-flatten metadata, the compressed backups and the
/// string pool.  Extracted verbatim from `LinearInterface` — every
/// `mutable` qualifier and default initializer is preserved so the
/// COW / clone semantics are bit-identical to the pre-extraction code.
struct LpLabelStore
{
  /// Non-owning back-pointer to the run-lifetime async metadata store and this
  /// cell's spill key.  When set (names kept + non-monolithic), the structural
  /// label metadata was spilled to the store and dropped from the snapshot;
  /// `generate_labels_from_maps` reloads it from the store (cached) on demand.
  /// Null when spilling is disabled (the live label vectors are authoritative).
  LpNameSpillStore* name_store {nullptr};
  std::string spill_key {};

  /// Name-to-index maps for duplicate detection and later lookup.
  /// Populated when names are enabled.
  // Mutable for the lazy-materialisation path: caches populated by
  // `generate_labels_from_maps` (logically const) live here too.
  mutable std::shared_ptr<row_name_map_t> row_names {
      std::make_shared<row_name_map_t>(),
  };  ///< Row (constraint) name → idx
  mutable std::shared_ptr<col_name_map_t> col_names {
      std::make_shared<col_name_map_t>(),
  };  ///< Column (variable) name → idx
  // Mutable so `generate_labels_from_maps` (logically const — it
  // returns new vectors; the state update is a caching detail) can
  // persist freshly-formatted labels for reuse on subsequent calls.
  mutable std::shared_ptr<StrongIndexVector<ColIndex, std::string>>
      col_index_to_name {
          std::make_shared<StrongIndexVector<ColIndex, std::string>>(),
      };
  mutable std::shared_ptr<StrongIndexVector<RowIndex, std::string>>
      row_index_to_name {
          std::make_shared<StrongIndexVector<RowIndex, std::string>>(),
      };

  /// Label-only metadata for the **frozen** flatten-time portion of the
  /// LP — set ONCE by `load_flat` from `flat_lp.col_labels_meta` /
  /// `row_labels_meta`, then never resized.  Indexed by the structural
  /// `[0, flatten_col_count())` / `[0, flatten_row_count())` half of
  /// the LP.  Post-flatten additions (cuts, slacks, alpha, cascade
  /// elastic constraints) live in `post_flatten_col_labels_meta` /
  /// `post_flatten_row_labels_meta` instead.
  ///
  /// Sharing model:
  ///   * `shared_ptr<T>` so `clone(CloneKind::shallow)` hands the
  ///     vector out to aperture clones via atomic incref — zero copy.
  ///   * Because the vector is never resized post-`load_flat`, no
  ///     mutating site detaches it; clones and source share the same
  ///     storage forever (no copy-on-write churn on the first
  ///     post-flatten add).
  ///
  /// Under `LowMemoryMode::compress`, `release_backend` compresses
  /// these vectors into `col_labels_meta_compressed` /
  /// `row_labels_meta_compressed` and clears the live copies.  The
  /// decompressed strings live in `label_string_pool` — the pool is
  /// never cleared while `col_labels_meta` references it.  `mutable`
  /// because the lazy decompression flow is triggered from const methods.
  mutable std::shared_ptr<std::vector<SparseColLabel>> col_labels_meta {
      std::make_shared<std::vector<SparseColLabel>>(),
  };
  mutable std::shared_ptr<std::vector<SparseRowLabel>> row_labels_meta {
      std::make_shared<std::vector<SparseRowLabel>>(),
  };

  /// Label-only metadata for the **post-flatten** portion of the LP —
  /// extended by every `add_col(SparseCol)` / `add_row(SparseRow)`
  /// that runs after `load_flat` has installed the structural matrix.
  /// Hosts cut rows, alpha column, cascade elastic-target slacks +
  /// constraints, and any other dynamic addition.
  ///
  /// Per-instance — never wrapped in `shared_ptr`, never shared with
  /// clones.  A freshly-cloned `LinearInterface` starts with empty
  /// post-flatten vectors regardless of the source's history; the
  /// clone's own post-flatten additions land here independently of
  /// the source's, which is the correct semantics for aperture clones
  /// (each clone's elastic-filter slacks belong only to that clone).
  ///
  /// Not compressed: the post-flatten vector is small in practice
  /// (alpha + a bounded set of cut rows / cascade elastic slacks) and
  /// is mutated frequently.
  std::vector<SparseColLabel> post_flatten_col_labels_meta {};
  std::vector<SparseRowLabel> post_flatten_row_labels_meta {};

  /// Compressed backups of the metadata vectors — populated on
  /// `release_backend` under `compress` mode, drained on the first
  /// label-metadata read after reload.
  ///
  /// Intentionally NOT `shared_ptr`-wrapped: compression / decompression
  /// only ever runs on the source LP (via `release_backend` and
  /// `ensure_backend`); clones never compress.  The
  /// `DecompressionGuard` around the aperture pass ensures the source
  /// is in the decompressed state for the lifetime of any shallow
  /// clone, so the source uniquely owns these buffers throughout.
  mutable CompressedBuffer col_labels_meta_compressed {};
  mutable CompressedBuffer row_labels_meta_compressed {};
  mutable std::size_t col_labels_meta_count {0};
  mutable std::size_t row_labels_meta_count {0};

  /// Stable string storage backing decompressed `string_view`s in
  /// `col_labels_meta` / `row_labels_meta`.  Reserved ahead of
  /// decompression so `push_back` doesn't invalidate the views.
  mutable std::vector<std::string> label_string_pool {};
};

}  // namespace gtopt
