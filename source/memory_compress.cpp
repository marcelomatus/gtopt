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

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

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
  // The lz4/snappy case bodies collapse to `return true;` (matching the
  // always-available codecs) only when GTOPT_HAS_LZ4/GTOPT_HAS_SNAPPY are
  // defined; without them they return false.  The per-codec cases are kept
  // separate so the availability map stays readable across build configs.
  switch (codec) {
    // NOLINTNEXTLINE(bugprone-branch-clone)
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
  // zlib's C API works in `Bytef` (unsigned char); casting our char
  // buffers to it is the standard, unavoidable interop pattern.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* const out_ptr = reinterpret_cast<Bytef*>(out.data());
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* const in_ptr = reinterpret_cast<const Bytef*>(data.data());
  const auto rc = ::compress2(
      out_ptr, &out_len, in_ptr, static_cast<uLong>(data.size()), /*level=*/1);
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
  // zlib's C API works in `Bytef` (unsigned char); casting our char
  // buffers to it is the standard, unavoidable interop pattern.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* const out_ptr = reinterpret_cast<Bytef*>(out.data());
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* const in_ptr = reinterpret_cast<const Bytef*>(compressed.data());
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

// ── Per-codec aggregate statistics ──────────────────────────────────────────
//
// Process-global atomic counters per codec.  Updated silently on every
// `compress()` / `decompress()` call — no per-op logging.  Read out at
// end-of-run via `log_compression_stats()` (one INFO line per used codec).
namespace
{
struct CodecAccum
{
  std::atomic<std::uint64_t> n_compress {0};
  std::atomic<std::uint64_t> compress_us {0};
  std::atomic<std::uint64_t> compress_in_bytes {0};
  std::atomic<std::uint64_t> compress_out_bytes {0};
  std::atomic<std::uint64_t> n_decompress {0};
  std::atomic<std::uint64_t> decompress_us {0};
  std::atomic<std::uint64_t> decompress_in_bytes {0};
  std::atomic<std::uint64_t> decompress_out_bytes {0};
};

// One slot per CompressionCodec value.  10 slots covers all currently
// defined codec values; out-of-range falls into the last slot harmlessly.
constexpr std::size_t kCodecSlots = 10;

// Static-local so the array's destructor never runs during a static-dtor
// race with late stats reads.
CodecAccum& accum_for(CompressionCodec c) noexcept
{
  static std::array<CodecAccum, kCodecSlots> slots;
  const auto idx = std::min(static_cast<std::size_t>(c), kCodecSlots - 1);
  return slots.at(idx);
}

// Iterate every codec slot.  Used by get/log/reset.
template<typename F>
void for_each_codec(F f) noexcept
{
  // Codec values are sparse-ish — iterate the named values explicitly.
  static constexpr std::array all_codecs {
      CompressionCodec::uncompressed,
      CompressionCodec::lz4,
      CompressionCodec::snappy,
      CompressionCodec::zstd,
      CompressionCodec::gzip,
      CompressionCodec::bzip2,
      CompressionCodec::xz,
      CompressionCodec::brotli,
      CompressionCodec::lzo,
  };
  for (auto c : all_codecs) {
    f(c, accum_for(c));
  }
}

// Time + record one compression.  Single point of measurement so
// every backend codec gets timed identically.
template<typename Fn>
std::vector<char> timed_compress(CompressionCodec codec,
                                 std::span<const char> data,
                                 Fn&& fn)
{
  const auto t0 = std::chrono::steady_clock::now();
  auto result = std::forward<Fn>(fn)();
  const auto us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0)
          .count());
  auto& acc = accum_for(codec);
  acc.n_compress.fetch_add(1, std::memory_order_relaxed);
  acc.compress_us.fetch_add(us, std::memory_order_relaxed);
  acc.compress_in_bytes.fetch_add(data.size(), std::memory_order_relaxed);
  acc.compress_out_bytes.fetch_add(result.size(), std::memory_order_relaxed);
  return result;
}

template<typename Fn>
std::vector<char> timed_decompress(CompressionCodec codec,
                                   std::span<const char> compressed,
                                   std::size_t original_size,
                                   Fn&& fn)
{
  const auto t0 = std::chrono::steady_clock::now();
  auto result = std::forward<Fn>(fn)();
  const auto us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0)
          .count());
  auto& acc = accum_for(codec);
  acc.n_decompress.fetch_add(1, std::memory_order_relaxed);
  acc.decompress_us.fetch_add(us, std::memory_order_relaxed);
  acc.decompress_in_bytes.fetch_add(compressed.size(),
                                    std::memory_order_relaxed);
  acc.decompress_out_bytes.fetch_add(original_size, std::memory_order_relaxed);
  return result;
}
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
      return timed_compress(
          codec,
          data,
          [&] { return std::vector<char> {data.begin(), data.end()}; });
    case CompressionCodec::zstd:
      return timed_compress(codec, data, [&] { return compress_zstd(data); });
    case CompressionCodec::gzip:
      return timed_compress(codec, data, [&] { return compress_gzip(data); });
    case CompressionCodec::lz4:
#ifdef GTOPT_HAS_LZ4
      return timed_compress(codec, data, [&] { return compress_lz4(data); });
#else
      throw std::runtime_error("lz4 codec not available (liblz4 not found)");
#endif
    case CompressionCodec::snappy:
#ifdef GTOPT_HAS_SNAPPY
      return timed_compress(codec, data, [&] { return compress_snappy(data); });
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
      return timed_decompress(
          codec,
          compressed,
          original_size,
          [&]
          { return std::vector<char> {compressed.begin(), compressed.end()}; });
    case CompressionCodec::zstd:
      return timed_decompress(
          codec,
          compressed,
          original_size,
          [&] { return decompress_zstd(compressed, original_size); });
    case CompressionCodec::gzip:
      return timed_decompress(
          codec,
          compressed,
          original_size,
          [&] { return decompress_gzip(compressed, original_size); });
    case CompressionCodec::lz4:
#ifdef GTOPT_HAS_LZ4
      return timed_decompress(
          codec,
          compressed,
          original_size,
          [&] { return decompress_lz4(compressed, original_size); });
#else
      throw std::runtime_error("lz4 codec not available (liblz4 not found)");
#endif
    case CompressionCodec::snappy:
#ifdef GTOPT_HAS_SNAPPY
      return timed_decompress(
          codec,
          compressed,
          original_size,
          [&] { return decompress_snappy(compressed, original_size); });
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
    dst += sz;
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
  // Preserve col_scales / row_scales sizes so decompress reads
  // exactly the bytes that were written.  These can be 0 when
  // ``--no-scale`` leaves the scaling vectors empty.
  result.flp_col_scales_size = flp.col_scales.size();
  result.flp_row_scales_size = flp.row_scales.size();

  // Free the large numeric vectors (metadata is preserved).  NB: `v = {}`
  // assigns an empty initializer_list and KEEPS the vector's capacity — the
  // buffer is NOT returned to the allocator.  This silently leaked ~half the
  // compress-mode flat-LP resident: each cell held BOTH the lz4 buffer AND its
  // still-allocated source arrays (~1.28 GiB on the 2y case).  swap-with-empty
  // actually releases the memory; `decompress_flat_lp` re-`resize`s on demand.
  const auto release = [](auto& v) noexcept
  { std::remove_cvref_t<decltype(v)> {}.swap(v); };
  release(flp.matbeg);
  release(flp.matind);
  release(flp.colint);
  release(flp.matval);
  release(flp.collb);
  release(flp.colub);
  release(flp.objval);
  release(flp.rowlb);
  release(flp.rowub);
  release(flp.col_scales);
  release(flp.row_scales);

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
  // col_scales / row_scales may have been EMPTY at compress time
  // (``--no-scale`` with equilibration=none).  Resize to the actual
  // compress-time size — NOT to ``ncols`` / ``nrows`` unconditionally.
  // The previous code resized to ncols/nrows and read that many bytes
  // from the buffer, overreading by ``ncols × 8`` past the end and
  // segfaulting in the next backend reconstruction step.
  flp.col_scales.resize(buf.flp_col_scales_size);
  flp.row_scales.resize(buf.flp_row_scales_size);

  auto raw = buf.decompress_data();
  const auto* src = raw.data();

  auto restore = [&src]<typename T>(std::vector<T>& vec)
  {
    const auto sz = vec.size() * sizeof(T);
    std::memcpy(vec.data(), src, sz);
    src += sz;
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
  // swap-with-empty to actually RELEASE capacity — `v = {}` keeps the buffer
  // allocated (see the note in `compress_flat_lp`).
  const auto release = [](auto& v) noexcept
  { std::remove_cvref_t<decltype(v)> {}.swap(v); };
  release(flp.matbeg);
  release(flp.matind);
  release(flp.colint);
  release(flp.matval);
  release(flp.collb);
  release(flp.colub);
  release(flp.objval);
  release(flp.rowlb);
  release(flp.rowub);
  release(flp.col_scales);
  release(flp.row_scales);
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

// ── Public stats API ────────────────────────────────────────────────────────

std::vector<CompressionStatsSample> get_compression_stats() noexcept
{
  std::vector<CompressionStatsSample> out;
  for_each_codec(
      [&](CompressionCodec c, CodecAccum& acc)
      {
        const auto nc = acc.n_compress.load(std::memory_order_relaxed);
        const auto nd = acc.n_decompress.load(std::memory_order_relaxed);
        if (nc == 0 && nd == 0) {
          return;  // skip codecs that were never used
        }
        out.push_back(CompressionStatsSample {
            .codec = c,
            .n_compress = nc,
            .compress_us = acc.compress_us.load(std::memory_order_relaxed),
            .compress_in_bytes =
                acc.compress_in_bytes.load(std::memory_order_relaxed),
            .compress_out_bytes =
                acc.compress_out_bytes.load(std::memory_order_relaxed),
            .n_decompress = nd,
            .decompress_us = acc.decompress_us.load(std::memory_order_relaxed),
            .decompress_in_bytes =
                acc.decompress_in_bytes.load(std::memory_order_relaxed),
            .decompress_out_bytes =
                acc.decompress_out_bytes.load(std::memory_order_relaxed),
        });
      });
  return out;
}

void reset_compression_stats() noexcept
{
  for_each_codec(
      [](CompressionCodec /*c*/, CodecAccum& acc)
      {
        acc.n_compress.store(0, std::memory_order_relaxed);
        acc.compress_us.store(0, std::memory_order_relaxed);
        acc.compress_in_bytes.store(0, std::memory_order_relaxed);
        acc.compress_out_bytes.store(0, std::memory_order_relaxed);
        acc.n_decompress.store(0, std::memory_order_relaxed);
        acc.decompress_us.store(0, std::memory_order_relaxed);
        acc.decompress_in_bytes.store(0, std::memory_order_relaxed);
        acc.decompress_out_bytes.store(0, std::memory_order_relaxed);
      });
}

namespace
{
// Pretty-print a byte count: "1.23 GB", "421 MB", "5 KB", "789 B".
// Not noexcept: std::format may throw on allocation failure.
std::string human_bytes(std::uint64_t bytes)
{
  static constexpr std::array<std::string_view, 5> units {
      "B",
      "KB",
      "MB",
      "GB",
      "TB",
  };
  auto v = static_cast<double>(bytes);
  std::size_t u = 0;
  while (v >= 1024.0 && u + 1 < units.size()) {
    v /= 1024.0;
    ++u;
  }
  // Fewer decimals at the higher end for readability.
  if (u == 0) {
    return std::format("{} {}", bytes, units.front());
  }
  return std::format("{:.1f} {}", v, units.at(u));
}

// Throughput in MB/s for a (bytes, microseconds) pair.  Returns 0 when
// either is zero (avoids div-by-zero and meaningless infinity).
double throughput_mb_per_s(std::uint64_t bytes, std::uint64_t us) noexcept
{
  if (bytes == 0 || us == 0) {
    return 0.0;
  }
  // bytes / s = (bytes / (us * 1e-6)).  In MB/s: divide by 1024^2.
  return static_cast<double>(bytes)
      / (static_cast<double>(us) * 1.048576);  // == 1e-6 * 1024 * 1024
}
}  // namespace

void log_compression_stats() noexcept
{
  try {
    const auto samples = get_compression_stats();
    if (samples.empty()) {
      return;  // nothing to report — silent
    }
    for (const auto& s : samples) {
      const double ratio = (s.compress_out_bytes > 0)
          ? static_cast<double>(s.compress_in_bytes)
              / static_cast<double>(s.compress_out_bytes)
          : 0.0;
      const double comp_s = static_cast<double>(s.compress_us) / 1e6;
      const double decomp_s = static_cast<double>(s.decompress_us) / 1e6;
      const double comp_mbs =
          throughput_mb_per_s(s.compress_in_bytes, s.compress_us);
      const double decomp_mbs =
          throughput_mb_per_s(s.decompress_out_bytes, s.decompress_us);

      // One line per codec.  Trade-off readout: ratio + compress speed
      // (`comp_mbs`) for sustained-write workloads, decompress speed
      // (`decomp_mbs`) for sustained-read workloads (SDDP reconstruct).
      spdlog::info(
          "compression[{}]: {} ops compressed {} -> {} (ratio {:.2f}x) "
          "in {:.2f}s ({:.0f} MB/s); {} ops decompressed {} -> {} in "
          "{:.2f}s ({:.0f} MB/s)",
          codec_name(s.codec),
          s.n_compress,
          human_bytes(s.compress_in_bytes),
          human_bytes(s.compress_out_bytes),
          ratio,
          comp_s,
          comp_mbs,
          s.n_decompress,
          human_bytes(s.decompress_in_bytes),
          human_bytes(s.decompress_out_bytes),
          decomp_s,
          decomp_mbs);
    }
  } catch (...) {
    // noexcept contract — best effort, swallow formatting / logger errors.
  }
}

}  // namespace gtopt
