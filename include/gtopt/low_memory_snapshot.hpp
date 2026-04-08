/**
 * @file      low_memory_snapshot.hpp
 * @brief     Bundled FlatLinearProblem + solution cache with compression
 * @date      2026-04-07
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Combines a FlatLinearProblem with cached primal/dual solution vectors
 * and provides compress()/decompress() as a single unit.  Used by the
 * low-memory SDDP mode to save and restore the complete LP state.
 */

#pragma once

#include <vector>

#include <gtopt/linear_problem.hpp>
#include <gtopt/memory_compress.hpp>

namespace gtopt
{

/// Bundles a FlatLinearProblem with a cached solution (col_sol + row_dual)
/// and provides compress/decompress as a single unit.
struct LowMemorySnapshot
{
  FlatLinearProblem flat_lp {};
  std::vector<double> cached_col_sol {};
  std::vector<double> cached_row_dual {};

  /// Compressed representation (populated after compress(), cleared on
  /// decompress()).
  CompressedBuffer compressed_lp {};
  CompressedBuffer compressed_sol {};

  /// True when the numeric vectors have been compressed away.
  [[nodiscard]] bool is_compressed() const noexcept
  {
    return !compressed_lp.empty();
  }

  /// True when a snapshot has been saved (metadata present).
  [[nodiscard]] bool has_data() const noexcept
  {
    return flat_lp.ncols > 0 || !compressed_lp.empty();
  }

  /// Compress the flat LP and solution vectors.  No-op if already compressed.
  void compress(MemoryCodec codec);

  /// Decompress and restore the flat LP and solution vectors.
  /// No-op if already decompressed.
  void decompress();
};

}  // namespace gtopt
