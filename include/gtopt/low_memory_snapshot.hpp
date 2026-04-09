/**
 * @file      low_memory_snapshot.hpp
 * @brief     FlatLinearProblem snapshot with persistent compression
 * @date      2026-04-07
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Stores a FlatLinearProblem with a persistent compressed representation.
 * The flat LP is compressed once after the first solve and never
 * re-compressed.  Subsequent cycles only decompress → load into backend
 * → free decompressed vectors.
 *
 * Solution vectors (col_sol, row_dual, col_cost) are cached directly
 * on LinearInterface, not here.
 */

#pragma once

#include <gtopt/linear_problem.hpp>
#include <gtopt/memory_compress.hpp>

namespace gtopt
{

/// Stores a FlatLinearProblem with persistent zstd compression.
///
/// The compressed buffer is created once during the first `compress()` call
/// and kept as a persistent cache.  `decompress()` expands the vectors;
/// callers free them via `clear_flat_lp_vectors()` after loading into the
/// backend.  The flat LP is immutable after `save_snapshot()`.
struct LowMemorySnapshot
{
  FlatLinearProblem flat_lp {};

  /// Persistent compressed representation of the flat LP.
  /// Created once during the first compress() call; never re-compressed.
  CompressedBuffer compressed_lp {};

  /// True when the flat LP vectors have been compressed away.
  [[nodiscard]] bool is_compressed() const noexcept
  {
    return !compressed_lp.empty();
  }

  /// True when a snapshot has been saved (metadata present).
  [[nodiscard]] bool has_data() const noexcept
  {
    return flat_lp.ncols > 0 || !compressed_lp.empty();
  }

  /// Compress the flat LP (first call only) and free expanded vectors.
  /// Subsequent calls just free the expanded vectors.
  void compress(CompressionCodec codec);

  /// Decompress the flat LP vectors from the compressed buffer.
  /// Keeps the compressed buffer intact (persistent cache).
  void decompress();
};

}  // namespace gtopt
