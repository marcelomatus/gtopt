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

#include <span>
#include <vector>

#include <gtopt/linear_problem.hpp>
#include <gtopt/memory_compress.hpp>

namespace gtopt
{

/// Bundles a FlatLinearProblem with a cached solution (col_sol + row_dual)
/// and provides compress/decompress as a single unit.
///
/// The compressed buffers are kept as a **persistent cache** alongside the
/// expanded vectors.  `decompress()` expands without freeing compressed data;
/// `compress()` frees expanded vectors, re-compressing only what changed.
/// The flat LP is immutable after `save_snapshot()`, so its compressed form
/// is created once and never re-compressed.  Only the solution cache is
/// re-compressed when `dirty_sol_` is set.
struct LowMemorySnapshot
{
  FlatLinearProblem flat_lp {};
  std::vector<double> cached_col_sol {};
  std::vector<double> cached_row_dual {};

  /// Persistent compressed representations.
  CompressedBuffer compressed_lp {};
  CompressedBuffer compressed_sol {};

  /// True when cached_col_sol/cached_row_dual have been updated
  /// since the last compress().
  bool dirty_sol_ {false};

  /// True after the first solution compression has been logged.
  bool logged_sol_ {false};

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

  /// Collapse: free expanded vectors, keeping compressed data.
  /// Compresses flat LP on first call only (immutable).
  /// Re-compresses solution only when dirty_sol_ is set.
  void compress(MemoryCodec codec);

  /// Expand: restore vectors from compressed buffers.
  /// Keeps compressed buffers intact (persistent cache).
  void decompress();

  /// Update the cached solution and mark as dirty.
  void set_cached_solution(std::span<const double> col_sol,
                           std::span<const double> row_dual);
};

}  // namespace gtopt
