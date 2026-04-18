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

bool is_codec_available(CompressionCodec codec) noexcept
{
  // NOLINTBEGIN(bugprone-branch-clone) — lz4/snappy bodies may be identical
  // depending on which optional codec libraries are compiled in.
  switch (codec) {
    case CompressionCodec::auto_select:
    case CompressionCodec::uncompressed:
    case CompressionCodec::zstd:
    case CompressionCodec::gzip:
      return true;  // always available
    case CompressionCodec::lz4:
#ifdef GTOPT_HAS_LZ4
      return true;
#else
      return false;
#endif
    case CompressionCodec::snappy:
#ifdef GTOPT_HAS_SNAPPY
      return true;
#else
      return false;
#endif
    case CompressionCodec::bzip2:
    case CompressionCodec::xz:
    case CompressionCodec::brotli:
    case CompressionCodec::lzo:
      return false;  // file-only codecs, not for in-memory use
  }
  return false;
  // NOLINTEND(bugprone-branch-clone)
}

std::string_view codec_name(CompressionCodec codec) noexcept
{
  switch (codec) {
    case CompressionCodec::auto_select:
      return "auto";
    case CompressionCodec::uncompressed:
      return "none";
    case CompressionCodec::lz4:
      return "lz4";
    case CompressionCodec::snappy:
      return "snappy";
    case CompressionCodec::zstd:
      return "zstd";
    case CompressionCodec::gzip:
      return "gzip";
    case CompressionCodec::bzip2:
      return "bzip2";
    case CompressionCodec::xz:
      return "xz";
    case CompressionCodec::brotli:
      return "brotli";
    case CompressionCodec::lzo:
      return "lzo";
  }
  return "unknown";
}

CompressionCodec select_codec(CompressionCodec preferred) noexcept
{
  // auto_select: pick the fastest available codec
  if (preferred == CompressionCodec::auto_select) {
    // Priority: lz4 > snappy > zstd > gzip
    if (is_codec_available(CompressionCodec::lz4)) {
      return CompressionCodec::lz4;
    }
    if (is_codec_available(CompressionCodec::snappy)) {
      return CompressionCodec::snappy;
    }
    return CompressionCodec::zstd;  // always available
  }

  // Explicit codec: use it if available, else fall back
  if (is_codec_available(preferred)) {
    return preferred;
  }
  // Fallback: zstd (always available)
  return CompressionCodec::zstd;
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
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto rc = ::compress2(reinterpret_cast<Bytef*>(out.data()),
                              &out_len,
                              reinterpret_cast<const Bytef*>(data.data()),
                              static_cast<uLong>(data.size()),
                              /*level=*/1);
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
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
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* const out_ptr = reinterpret_cast<Bytef*>(out.data());
  const auto* const in_ptr = reinterpret_cast<const Bytef*>(compressed.data());
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto rc = ::uncompress(
      out_ptr, &out_len, in_ptr, static_cast<uLong>(compressed.size()));
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
  if (data.size() > static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) {
    throw std::runtime_error("lz4 compress: input exceeds LZ4_MAX_INPUT_SIZE");
  }
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

std::vector<char> compress(std::span<const char> data, CompressionCodec codec)
{
  // Resolve auto_select before dispatching (avoids recursion).
  if (codec == CompressionCodec::auto_select) {
    codec = select_codec(codec);
  }
  switch (codec) {
    case CompressionCodec::auto_select:
      break;  // already resolved above
    case CompressionCodec::uncompressed:
      return {data.begin(), data.end()};
    case CompressionCodec::zstd:
      return compress_zstd(data);
    case CompressionCodec::gzip:
      return compress_gzip(data);
    case CompressionCodec::lz4:
#ifdef GTOPT_HAS_LZ4
      return compress_lz4(data);
#else
      throw std::runtime_error("lz4 codec not available (liblz4 not found)");
#endif
    case CompressionCodec::snappy:
#ifdef GTOPT_HAS_SNAPPY
      return compress_snappy(data);
#else
      throw std::runtime_error(
          "snappy codec not available (libsnappy not found)");
#endif
    case CompressionCodec::bzip2:
    case CompressionCodec::xz:
    case CompressionCodec::brotli:
    case CompressionCodec::lzo:
      throw std::runtime_error(
          std::string("codec '") + std::string(codec_name(codec))
          + "' is not available for in-memory compression");
  }
  throw std::runtime_error("unknown codec");
}

std::vector<char> decompress(std::span<const char> compressed,
                             size_t original_size,
                             CompressionCodec codec)
{
  // Resolve auto_select before dispatching (avoids recursion).
  if (codec == CompressionCodec::auto_select) {
    codec = select_codec(codec);
  }
  switch (codec) {
    case CompressionCodec::auto_select:
      break;  // already resolved above
    case CompressionCodec::uncompressed:
      return {compressed.begin(), compressed.end()};
    case CompressionCodec::zstd:
      return decompress_zstd(compressed, original_size);
    case CompressionCodec::gzip:
      return decompress_gzip(compressed, original_size);
    case CompressionCodec::lz4:
#ifdef GTOPT_HAS_LZ4
      return decompress_lz4(compressed, original_size);
#else
      throw std::runtime_error("lz4 codec not available (liblz4 not found)");
#endif
    case CompressionCodec::snappy:
#ifdef GTOPT_HAS_SNAPPY
      return decompress_snappy(compressed, original_size);
#else
      throw std::runtime_error(
          "snappy codec not available (libsnappy not found)");
#endif
    case CompressionCodec::bzip2:
    case CompressionCodec::xz:
    case CompressionCodec::brotli:
    case CompressionCodec::lzo:
      throw std::runtime_error(
          std::string("codec '") + std::string(codec_name(codec))
          + "' is not available for in-memory decompression");
  }
  throw std::runtime_error("unknown codec");
}

// ── FlatLinearProblem compression ──────────────────────────────────────────

CompressedBuffer compress_flat_lp(FlatLinearProblem& flp,
                                  CompressionCodec codec)
{
  const auto effective = select_codec(codec);
  if (effective == CompressionCodec::uncompressed) {
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

  // Capture per-vector sizes now so decompress can pre-reserve upfront
  // without byte-arithmetic inference.  matval.size() == matbeg[ncols].
  const size_t nnz = flp.matval.size();
  const size_t colint_count = flp.colint.size();

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
  result.flp_nnz = nnz;
  result.flp_colint_count = colint_count;

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

  const auto ncols = static_cast<size_t>(flp.ncols);
  const auto nrows = static_cast<size_t>(flp.nrows);
  const auto nnz = buf.flp_nnz;
  const auto colint_count = buf.flp_colint_count;

  // Pre-reserve all 11 numeric vectors to their exact sizes before the
  // decompress/memcpy loop runs.  Sizes come from `buf.flp_nnz` and
  // `buf.flp_colint_count` (captured at compress time) plus the static
  // metadata fields `ncols`/`nrows` — no byte-arithmetic inference.
  flp.matbeg.resize(ncols + 1);
  flp.matind.resize(nnz);
  flp.colint.resize(colint_count);
  flp.matval.resize(nnz);
  flp.collb.resize(ncols);
  flp.colub.resize(ncols);
  flp.objval.resize(ncols);
  flp.rowlb.resize(nrows);
  flp.rowub.resize(nrows);
  flp.col_scales.resize(ncols);
  flp.row_scales.resize(nrows);

  auto raw = buf.decompress_data();
  const auto* src = raw.data();

  auto restore = [&src]<typename T>(std::vector<T>& vec)
  {
    const auto sz = vec.size() * sizeof(T);
    std::memcpy(vec.data(), src, sz);
    src += sz;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  };

  restore(flp.matbeg);
  restore(flp.matind);
  restore(flp.colint);
  restore(flp.matval);
  restore(flp.collb);
  restore(flp.colub);
  restore(flp.objval);
  restore(flp.rowlb);
  restore(flp.rowub);
  restore(flp.col_scales);
  restore(flp.row_scales);
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
  // NOTE: name vectors (colnm, rownm, colmp, rowmp) are NOT cleared here
  // because they are not part of the compressed buffer and cannot be
  // restored during decompression.  They must survive across
  // release/reconstruct cycles so that load_flat() can rebuild name maps.
}

// ── LowMemorySnapshot ─────────────────────────────────────────────────────

void LowMemorySnapshot::compress(CompressionCodec codec)
{
  // Flat LP: compress only on first call (immutable after save_snapshot).
  // Solution vectors are NOT compressed here — they are cached directly
  // on LinearInterface (m_cached_col_sol_ etc.) and never need snapshot
  // compression/decompression.
  if (compressed_lp.empty() && !flat_lp.matbeg.empty()) {
    compressed_lp = compress_flat_lp(flat_lp, codec);
    // compress_flat_lp already clears the numeric vectors
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
