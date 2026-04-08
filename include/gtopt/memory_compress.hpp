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

#include <gtopt/sddp_enums.hpp>

namespace gtopt
{

/// Check whether a given codec is available at runtime.
[[nodiscard]] bool is_codec_available(MemoryCodec codec) noexcept;

/// Return the name of the codec as a string.
[[nodiscard]] std::string_view codec_name(MemoryCodec codec) noexcept;

/// Select the best available codec, preferring the requested one.
/// Falls back to zstd (always available), then none.
[[nodiscard]] MemoryCodec select_codec(MemoryCodec preferred) noexcept;

/// Compress a byte buffer using the specified codec.
/// @param data  Input data to compress.
/// @param codec Compression algorithm (must be available).
/// @return Compressed bytes.
/// @throws std::runtime_error if codec is unavailable or compression fails.
[[nodiscard]] std::vector<char> compress(std::span<const char> data,
                                         MemoryCodec codec);

/// Decompress a buffer previously compressed with the same codec.
/// @param compressed  Compressed data.
/// @param original_size  Original uncompressed size (must be known).
/// @param codec  Algorithm used for compression.
/// @return Decompressed bytes of exactly original_size.
/// @throws std::runtime_error if decompression fails.
[[nodiscard]] std::vector<char> decompress(std::span<const char> compressed,
                                           size_t original_size,
                                           MemoryCodec codec);

/// Compressed buffer with metadata for round-trip compression.
struct CompressedBuffer
{
  std::vector<char> data {};
  size_t original_size {0};
  MemoryCodec codec {MemoryCodec::none};

  [[nodiscard]] bool empty() const noexcept { return data.empty(); }

  /// Decompress and return the original data.
  [[nodiscard]] std::vector<char> decompress_data() const
  {
    return gtopt::decompress(data, original_size, codec);
  }
};

/// Compress a byte span into a CompressedBuffer.
[[nodiscard]] inline CompressedBuffer compress_buffer(
    std::span<const char> data, MemoryCodec codec)
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
                                                MemoryCodec codec);

/// Decompress a buffer previously created by compress_flat_lp() and
/// restore the numeric vectors of @p flp.  No-op when @p buf is empty.
void decompress_flat_lp(FlatLinearProblem& flp, const CompressedBuffer& buf);

/// Clear only the large numeric vectors of a FlatLinearProblem,
/// preserving metadata (ncols, nrows, nnz, names, stats, etc.).
void clear_flat_lp_vectors(FlatLinearProblem& flp);

/// Compress primal (col_sol) and dual (row_dual) solution vectors.
/// The buffer layout is: [col_size:size_t][col_sol data][row_dual data].
[[nodiscard]] CompressedBuffer compress_solution(
    std::span<const double> col_sol,
    std::span<const double> row_dual,
    MemoryCodec codec);

/// Decompress a buffer previously created by compress_solution().
void decompress_solution(std::vector<double>& col_sol,
                         std::vector<double>& row_dual,
                         const CompressedBuffer& buf);

}  // namespace gtopt
