/**
 * @file      memory_compress.hpp
 * @brief     In-memory compression/decompression for low_memory mode
 * @date      2026-04-07
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides a unified API for compressing/decompressing byte buffers
 * using zstd, lz4, or snappy — whichever are available at build time.
 * Used by the low_memory SDDP mode to compress saved FlatLinearProblem
 * data in memory.
 */

#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include <gtopt/planning_enums.hpp>

namespace gtopt
{

/// Check whether a given codec is available at runtime.
[[nodiscard]] bool is_codec_available(CompressionCodec codec) noexcept;

/// Return the name of the codec as a string.
[[nodiscard]] std::string_view codec_name(CompressionCodec codec) noexcept;

/// Select the best available codec, preferring the requested one.
/// Falls back to zstd (always available), then none.
[[nodiscard]] CompressionCodec select_codec(
    CompressionCodec preferred) noexcept;

/// Compress a byte buffer using the specified codec.
/// @param data  Input data to compress.
/// @param codec Compression algorithm (must be available).
/// @return Compressed bytes.
/// @throws std::runtime_error if codec is unavailable or compression fails.
[[nodiscard]] std::vector<char> compress(std::span<const char> data,
                                         CompressionCodec codec);

/// Decompress a buffer previously compressed with the same codec.
/// @param compressed  Compressed data.
/// @param original_size  Original uncompressed size (must be known).
/// @param codec  Algorithm used for compression.
/// @return Decompressed bytes of exactly original_size.
/// @throws std::runtime_error if decompression fails.
[[nodiscard]] std::vector<char> decompress(std::span<const char> compressed,
                                           size_t original_size,
                                           CompressionCodec codec);

/// Compressed buffer with metadata for round-trip compression.
struct CompressedBuffer
{
  std::vector<char> data {};
  size_t original_size {0};
  CompressionCodec codec {CompressionCodec::uncompressed};

  /// FLP-specific layout metadata, populated by `compress_flat_lp` so that
  /// `decompress_flat_lp` can pre-reserve all 11 numeric vectors upfront
  /// without byte-arithmetic inference.  Zero for non-FLP buffers.
  size_t flp_nnz {0};  ///< matbeg[ncols] — total non-zeros
  size_t flp_colint_count {0};  ///< colint.size() — integer variable count
  /// col_scales / row_scales actual sizes at compress time.  May be 0
  /// (empty) when ``--no-scale`` forces ``equilibration=none`` and no
  /// per-column/row scaling is in effect — under those conditions
  /// ``flatten()`` leaves the col_scales/row_scales vectors empty and
  /// ``compress_flat_lp`` appends 0 bytes for them.  Decompress must
  /// not unconditionally resize to ``ncols``/``nrows`` and ``memcpy``
  /// ``ncols``/``nrows × sizeof(double)`` bytes — that would overread
  /// (segfault).  Storing the actual compress-time sizes here keeps
  /// decompress byte-symmetric with compress.
  size_t flp_col_scales_size {0};
  size_t flp_row_scales_size {0};

  [[nodiscard]] bool empty() const noexcept { return data.empty(); }

  /// Decompress and return the original data.
  [[nodiscard]] std::vector<char> decompress_data() const
  {
    return gtopt::decompress(data, original_size, codec);
  }
};

/// Compress a byte span into a CompressedBuffer.
[[nodiscard]] inline CompressedBuffer compress_buffer(
    std::span<const char> data, CompressionCodec codec)
{
  return CompressedBuffer {
      .data = compress(data, codec),
      .original_size = data.size(),
      .codec = codec,
  };
}

// Forward declaration — avoids pulling linear_problem.hpp into this header.
struct FlatLinearProblem;

/// Serialize the numeric vectors of a FlatLinearProblem into a contiguous
/// buffer, compress it, and clear the source vectors (keeping metadata
/// like ncols/nrows, names, stats, etc.).
/// No-op and returns an empty buffer when @p codec resolves to `none`.
[[nodiscard]] CompressedBuffer compress_flat_lp(FlatLinearProblem& flp,
                                                CompressionCodec codec);

/// Decompress a buffer previously created by compress_flat_lp() and
/// restore the numeric vectors of @p flp.  No-op when @p buf is empty.
void decompress_flat_lp(FlatLinearProblem& flp, const CompressedBuffer& buf);

/// Clear only the large numeric vectors of a FlatLinearProblem,
/// preserving metadata (ncols, nrows, nnz, names, stats, etc.).
void clear_flat_lp_vectors(FlatLinearProblem& flp);

// ── Aggregate compression statistics ────────────────────────────────────────
//
// `compress()` and `decompress()` silently accumulate per-codec counters
// (number of operations, wall time, uncompressed and compressed bytes) into
// process-global atomics — no per-call logging.  Call
// `log_compression_stats()` once at end-of-run to emit a single INFO line per
// codec actually used, e.g.:
//
//   compression[zstd]: 1234 ops compressed 5.4 GB → 421 MB (12.8x ratio)
//                      in 18.2s  (304 MB/s); 567 ops decompressed 421 MB →
//                      5.4 GB in 1.1s (4.9 GB/s).
//
// Use this to compare lz4 vs. zstd trade-offs (decompress-throughput vs.
// compression-ratio) on a real workload by running the same case twice with
// different `memory_codec` settings and diffing the two summary lines.

/// Snapshot of per-codec compression / decompression accumulators.
/// Returned by `get_compression_stats()` for programmatic inspection.
struct CompressionStatsSample
{
  CompressionCodec codec {CompressionCodec::uncompressed};

  std::uint64_t n_compress {0};
  std::uint64_t compress_us {0};
  std::uint64_t compress_in_bytes {0};
  std::uint64_t compress_out_bytes {0};

  std::uint64_t n_decompress {0};
  std::uint64_t decompress_us {0};
  std::uint64_t decompress_in_bytes {0};
  std::uint64_t decompress_out_bytes {0};
};

/// Read out the per-codec accumulators (snapshot, no clear).  Codecs with
/// zero ops in both directions are omitted.
[[nodiscard]] std::vector<CompressionStatsSample>
get_compression_stats() noexcept;

/// Reset all per-codec accumulators to zero.  Useful between runs in a
/// long-lived process (e.g. cascade levels) to attribute stats per level.
void reset_compression_stats() noexcept;

/// Emit one INFO log line per codec that has been used since startup or
/// since the last `reset_compression_stats()`.  Idempotent and cheap when
/// no codec was used (no log lines emitted).
void log_compression_stats() noexcept;

}  // namespace gtopt
