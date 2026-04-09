/**
 * @file      memory_compress.cpp
 * @brief     In-memory compression/decompression for low_memory mode
 * @date      2026-04-07
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

#include <gtopt/low_memory_snapshot.hpp>
#include <gtopt/memory_compress.hpp>
#include <spdlog/spdlog.h>

// zstd and zlib are always available (REQUIRED in CMakeLists.txt)
#include <zlib.h>
#include <zstd.h>

#ifdef GTOPT_HAS_LZ4
#  include <lz4.h>
#endif

#ifdef GTOPT_HAS_SNAPPY
#  include <snappy.h>
#endif

namespace gtopt
{

bool is_codec_available(MemoryCodec codec) noexcept
{
  switch (codec) {
    case MemoryCodec::auto_select:
    case MemoryCodec::none:
    case MemoryCodec::zstd:
    case MemoryCodec::gzip:
      return true;  // always available
    case MemoryCodec::lz4:
#ifdef GTOPT_HAS_LZ4
      return true;
#else
      return false;
#endif
    case MemoryCodec::snappy:
#ifdef GTOPT_HAS_SNAPPY
      return true;
#else
      return false;
#endif
  }
  return false;
}

std::string_view codec_name(MemoryCodec codec) noexcept
{
  switch (codec) {
    case MemoryCodec::auto_select:
      return "auto";
    case MemoryCodec::none:
      return "none";
    case MemoryCodec::lz4:
      return "lz4";
    case MemoryCodec::snappy:
      return "snappy";
    case MemoryCodec::zstd:
      return "zstd";
    case MemoryCodec::gzip:
      return "gzip";
  }
  return "unknown";
}

MemoryCodec select_codec(MemoryCodec preferred) noexcept
{
  // auto_select: pick the fastest available codec
  if (preferred == MemoryCodec::auto_select) {
    // Priority: lz4 > snappy > zstd > gzip
    if (is_codec_available(MemoryCodec::lz4)) {
      return MemoryCodec::lz4;
    }
    if (is_codec_available(MemoryCodec::snappy)) {
      return MemoryCodec::snappy;
    }
    return MemoryCodec::zstd;  // always available
  }

  // Explicit codec: use it if available, else fall back
  if (is_codec_available(preferred)) {
    return preferred;
  }
  // Fallback: zstd (always available)
  return MemoryCodec::zstd;
}

namespace
{

// ── zstd (always available) ─────────────────────────────────────────────

std::vector<char> compress_zstd(std::span<const char> data)
{
  const auto bound = ZSTD_compressBound(data.size());
  std::vector<char> out(bound);
  const auto rc = ZSTD_compress(
      out.data(), out.size(), data.data(), data.size(), /*level=*/1);
  if (ZSTD_isError(rc) != 0) {
    throw std::runtime_error(std::string("zstd compress failed: ")
                             + ZSTD_getErrorName(rc));
  }
  out.resize(rc);
  return out;
}

std::vector<char> decompress_zstd(std::span<const char> compressed,
                                  size_t original_size)
{
  std::vector<char> out(original_size);
  const auto rc = ZSTD_decompress(
      out.data(), out.size(), compressed.data(), compressed.size());
  if (ZSTD_isError(rc) != 0) {
    throw std::runtime_error(std::string("zstd decompress failed: ")
                             + ZSTD_getErrorName(rc));
  }
  return out;
}

// ── gzip/zlib (always available) ────────────────────────────────────────

std::vector<char> compress_gzip(std::span<const char> data)
{
  auto bound = compressBound(static_cast<uLong>(data.size()));
  std::vector<char> out(bound);
  auto out_len = static_cast<uLongf>(bound);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto rc =
      ::compress2(reinterpret_cast<Bytef*>(out.data()),
                  &out_len,
                  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                  reinterpret_cast<const Bytef*>(data.data()),
                  static_cast<uLong>(data.size()),
                  /*level=*/1);
  if (rc != Z_OK) {
    throw std::runtime_error("gzip compress failed (zlib error "
                             + std::to_string(rc) + ")");
  }
  out.resize(static_cast<size_t>(out_len));
  return out;
}

std::vector<char> decompress_gzip(std::span<const char> compressed,
                                  size_t original_size)
{
  std::vector<char> out(original_size);
  auto out_len = static_cast<uLongf>(original_size);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto rc = ::uncompress(
      reinterpret_cast<Bytef*>(out.data()),
      &out_len,
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<const Bytef*>(compressed.data()),
      static_cast<uLong>(compressed.size()));
  if (rc != Z_OK) {
    throw std::runtime_error("gzip decompress failed (zlib error "
                             + std::to_string(rc) + ")");
  }
  return out;
}

// ── lz4 (optional) ─────────────────────────────────────────────────────

#ifdef GTOPT_HAS_LZ4

std::vector<char> compress_lz4(std::span<const char> data)
{
  const auto bound = LZ4_compressBound(static_cast<int>(data.size()));
  std::vector<char> out(static_cast<size_t>(bound));
  const auto rc = LZ4_compress_default(
      data.data(), out.data(), static_cast<int>(data.size()), bound);
  if (rc <= 0) {
    throw std::runtime_error("lz4 compress failed");
  }
  out.resize(static_cast<size_t>(rc));
  return out;
}

std::vector<char> decompress_lz4(std::span<const char> compressed,
                                 size_t original_size)
{
  std::vector<char> out(original_size);
  const auto rc = LZ4_decompress_safe(compressed.data(),
                                      out.data(),
                                      static_cast<int>(compressed.size()),
                                      static_cast<int>(original_size));
  if (rc < 0) {
    throw std::runtime_error("lz4 decompress failed");
  }
  return out;
}

#endif  // GTOPT_HAS_LZ4

// ── snappy (optional) ──────────────────────────────────────────────────

#ifdef GTOPT_HAS_SNAPPY

std::vector<char> compress_snappy(std::span<const char> data)
{
  size_t out_len = snappy::MaxCompressedLength(data.size());
  std::vector<char> out(out_len);
  snappy::RawCompress(data.data(), data.size(), out.data(), &out_len);
  out.resize(out_len);
  return out;
}

std::vector<char> decompress_snappy(std::span<const char> compressed,
                                    size_t original_size)
{
  std::vector<char> out(original_size);
  if (!snappy::RawUncompress(compressed.data(), compressed.size(), out.data()))
  {
    throw std::runtime_error("snappy decompress failed");
  }
  return out;
}

#endif  // GTOPT_HAS_SNAPPY

}  // namespace

std::vector<char> compress(std::span<const char> data, MemoryCodec codec)
{
  // Resolve auto_select before dispatching (avoids recursion).
  if (codec == MemoryCodec::auto_select) {
    codec = select_codec(codec);
  }
  switch (codec) {
    case MemoryCodec::auto_select:
      break;  // already resolved above
    case MemoryCodec::none:
      return {data.begin(), data.end()};
    case MemoryCodec::zstd:
      return compress_zstd(data);
    case MemoryCodec::gzip:
      return compress_gzip(data);
    case MemoryCodec::lz4:
#ifdef GTOPT_HAS_LZ4
      return compress_lz4(data);
#else
      throw std::runtime_error("lz4 codec not available (liblz4 not found)");
#endif
    case MemoryCodec::snappy:
#ifdef GTOPT_HAS_SNAPPY
      return compress_snappy(data);
#else
      throw std::runtime_error(
          "snappy codec not available (libsnappy not found)");
#endif
  }
  throw std::runtime_error("unknown codec");
}

std::vector<char> decompress(std::span<const char> compressed,
                             size_t original_size,
                             MemoryCodec codec)
{
  // Resolve auto_select before dispatching (avoids recursion).
  if (codec == MemoryCodec::auto_select) {
    codec = select_codec(codec);
  }
  switch (codec) {
    case MemoryCodec::auto_select:
      break;  // already resolved above
    case MemoryCodec::none:
      return {compressed.begin(), compressed.end()};
    case MemoryCodec::zstd:
      return decompress_zstd(compressed, original_size);
    case MemoryCodec::gzip:
      return decompress_gzip(compressed, original_size);
    case MemoryCodec::lz4:
#ifdef GTOPT_HAS_LZ4
      return decompress_lz4(compressed, original_size);
#else
      throw std::runtime_error("lz4 codec not available (liblz4 not found)");
#endif
    case MemoryCodec::snappy:
#ifdef GTOPT_HAS_SNAPPY
      return decompress_snappy(compressed, original_size);
#else
      throw std::runtime_error(
          "snappy codec not available (libsnappy not found)");
#endif
  }
  throw std::runtime_error("unknown codec");
}

// ── FlatLinearProblem compression ──────────────────────────────────────────

CompressedBuffer compress_flat_lp(FlatLinearProblem& flp, MemoryCodec codec)
{
  const auto effective = select_codec(codec);
  if (effective == MemoryCodec::none) {
    return {};
  }

  auto bytes_of = [](const auto& vec) -> size_t
  {
    return vec.size()
        * sizeof(typename std::decay_t<decltype(vec)>::value_type);
  };

  const size_t total_bytes = bytes_of(flp.matbeg) + bytes_of(flp.matind)
      + bytes_of(flp.colint) + bytes_of(flp.matval) + bytes_of(flp.collb)
      + bytes_of(flp.colub) + bytes_of(flp.objval) + bytes_of(flp.rowlb)
      + bytes_of(flp.rowub) + bytes_of(flp.col_scales)
      + bytes_of(flp.row_scales);

  if (total_bytes == 0) {
    return {};
  }

  std::vector<char> buf(total_bytes);
  auto* dst = buf.data();

  auto append = [&dst](const auto& vec)
  {
    const auto sz =
        vec.size() * sizeof(typename std::decay_t<decltype(vec)>::value_type);
    std::memcpy(dst, vec.data(), sz);
    dst += sz;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  };

  append(flp.matbeg);
  append(flp.matind);
  append(flp.colint);
  append(flp.matval);
  append(flp.collb);
  append(flp.colub);
  append(flp.objval);
  append(flp.rowlb);
  append(flp.rowub);
  append(flp.col_scales);
  append(flp.row_scales);

  auto result = compress_buffer(buf, effective);

  // Free the large numeric vectors (metadata is preserved)
  flp.matbeg = {};
  flp.matind = {};
  flp.colint = {};
  flp.matval = {};
  flp.collb = {};
  flp.colub = {};
  flp.objval = {};
  flp.rowlb = {};
  flp.rowub = {};
  flp.col_scales = {};
  flp.row_scales = {};

  return result;
}

void decompress_flat_lp(FlatLinearProblem& flp, const CompressedBuffer& buf)
{
  if (buf.empty()) {
    return;
  }

  // Already decompressed — matbeg is populated
  if (!flp.matbeg.empty()) {
    return;
  }

  auto raw = buf.decompress_data();
  const auto* src = raw.data();

  auto restore = [&src]<typename T>(std::vector<T>& vec, size_t count)
  {
    vec.resize(count);
    const auto sz = count * sizeof(T);
    std::memcpy(vec.data(), src, sz);
    src += sz;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  };

  const auto ncols = static_cast<size_t>(flp.ncols);
  const auto nrows = static_cast<size_t>(flp.nrows);

  restore(flp.matbeg, ncols + 1);
  const auto actual_nnz =
      static_cast<size_t>(flp.matbeg[static_cast<int32_t>(ncols)]);

  // Derive colint count from byte layout
  const auto total_double_count = actual_nnz + (4 * ncols) + (3 * nrows);
  const auto total_double_bytes = total_double_count * sizeof(double);
  const auto int_bytes_used = (ncols + 1 + actual_nnz) * sizeof(int32_t);
  const auto remaining_int_bytes =
      buf.original_size - int_bytes_used - total_double_bytes;
  const auto colint_count = remaining_int_bytes / sizeof(int32_t);

  restore(flp.matind, actual_nnz);
  restore(flp.colint, colint_count);
  restore(flp.matval, actual_nnz);
  restore(flp.collb, ncols);
  restore(flp.colub, ncols);
  restore(flp.objval, ncols);
  restore(flp.rowlb, nrows);
  restore(flp.rowub, nrows);
  restore(flp.col_scales, ncols);
  restore(flp.row_scales, nrows);
}

// ── Clear flat LP numeric vectors ─────────────────────────────────────────

void clear_flat_lp_vectors(FlatLinearProblem& flp)
{
  flp.matbeg = {};
  flp.matind = {};
  flp.colint = {};
  flp.matval = {};
  flp.collb = {};
  flp.colub = {};
  flp.objval = {};
  flp.rowlb = {};
  flp.rowub = {};
  flp.col_scales = {};
  flp.row_scales = {};
}

// ── LowMemorySnapshot ─────────────────────────────────────────────────────

void LowMemorySnapshot::compress(MemoryCodec codec)
{
  using clock = std::chrono::steady_clock;

  // Flat LP: compress only on first call (immutable after save_snapshot).
  // Solution vectors are NOT compressed here — they are cached directly
  // on LinearInterface (m_cached_col_sol_ etc.) and never need snapshot
  // compression/decompression.
  if (compressed_lp.empty() && !flat_lp.matbeg.empty()) {
    const auto t0 = clock::now();
    compressed_lp = compress_flat_lp(flat_lp, codec);
    // compress_flat_lp already clears the numeric vectors
    const auto dt_ms =
        std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    const auto orig = compressed_lp.original_size;
    const auto comp = compressed_lp.data.size();
    const auto ratio = orig > 0 ? static_cast<double>(comp) / orig : 0.0;
    const auto saved_mb = static_cast<double>(orig - comp) / (1024.0 * 1024.0);
    SPDLOG_INFO(
        "low_memory: LP compressed with {} in {:.1f} ms — "
        "{:.2f} MB → {:.2f} MB (ratio {:.2f}, saved {:.2f} MB)",
        codec_name(compressed_lp.codec),
        dt_ms,
        orig / (1024.0 * 1024.0),
        comp / (1024.0 * 1024.0),
        ratio,
        saved_mb);
  } else if (!compressed_lp.empty()) {
    // Already have compressed form — just free expanded vectors
    clear_flat_lp_vectors(flat_lp);
  }
}

void LowMemorySnapshot::decompress()
{
  // Expand flat LP vectors (keep compressed buffer as persistent cache).
  // Solution vectors are NOT decompressed — they are cached directly
  // on LinearInterface and passed to reconstruct_backend() explicitly.
  if (flat_lp.matbeg.empty() && !compressed_lp.empty()) {
    decompress_flat_lp(flat_lp, compressed_lp);
    // Keep compressed_lp intact
  }
}

}  // namespace gtopt
