// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_memory_compress.cpp
 * @brief     Unit tests for in-memory compress/decompress round-trips
 * @date      2026-04-23
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/memory_compress.hpp>

using namespace gtopt;

namespace  // NOLINT
{
// Build a deterministic byte payload of a given size.
auto make_payload(std::size_t n) -> std::vector<char>
{
  std::vector<char> buf(n);
  for (std::size_t i = 0; i < n; ++i) {
    buf[i] = static_cast<char>(i & 0xFF);
  }
  return buf;
}
}  // namespace

TEST_CASE("is_codec_available — uncompressed is always true")  // NOLINT
{
  CHECK(is_codec_available(CompressionCodec::uncompressed));
}

TEST_CASE("is_codec_available — every enum value classified")  // NOLINT
{
  // The dispatcher's switch covers every `CompressionCodec` value;
  // exhaustive enum coverage prevents a future addition from
  // silently falling through the default `return false`.
  // Always-available codecs:
  CHECK(is_codec_available(CompressionCodec::auto_select));
  CHECK(is_codec_available(CompressionCodec::uncompressed));
  CHECK(is_codec_available(CompressionCodec::zstd));
  CHECK(is_codec_available(CompressionCodec::gzip));
  // Optional codecs (compile-time gated): just assert the call is
  // total — true or false depending on `GTOPT_HAS_LZ4` /
  // `GTOPT_HAS_SNAPPY`, but the call must not throw or assert.
  (void)is_codec_available(CompressionCodec::lz4);
  (void)is_codec_available(CompressionCodec::snappy);
  // File-only codecs (not for in-memory use):
  CHECK_FALSE(is_codec_available(CompressionCodec::bzip2));
  CHECK_FALSE(is_codec_available(CompressionCodec::xz));
  CHECK_FALSE(is_codec_available(CompressionCodec::brotli));
  CHECK_FALSE(is_codec_available(CompressionCodec::lzo));
}

TEST_CASE("codec_name returns non-empty string for known codecs")  // NOLINT
{
  CHECK_FALSE(codec_name(CompressionCodec::uncompressed).empty());
  CHECK_FALSE(codec_name(CompressionCodec::zstd).empty());
  CHECK_FALSE(codec_name(CompressionCodec::lz4).empty());
}

TEST_CASE("codec_name — exact spellings for every enum value")  // NOLINT
{
  // Lock the canonical lower-case names.  Downstream tools (logs,
  // JSON output, the planning JSON parser's `memory_codec` /
  // `output_compression` field) consume these strings verbatim, so a
  // typo here would silently break configuration round-trip.
  CHECK(codec_name(CompressionCodec::auto_select) == "auto");
  CHECK(codec_name(CompressionCodec::uncompressed) == "none");
  CHECK(codec_name(CompressionCodec::lz4) == "lz4");
  CHECK(codec_name(CompressionCodec::snappy) == "snappy");
  CHECK(codec_name(CompressionCodec::zstd) == "zstd");
  CHECK(codec_name(CompressionCodec::gzip) == "gzip");
  CHECK(codec_name(CompressionCodec::bzip2) == "bzip2");
  CHECK(codec_name(CompressionCodec::xz) == "xz");
  CHECK(codec_name(CompressionCodec::brotli) == "brotli");
  CHECK(codec_name(CompressionCodec::lzo) == "lzo");
}

TEST_CASE("select_codec falls back to an available codec")  // NOLINT
{
  const auto c = select_codec(CompressionCodec::zstd);
  CHECK(is_codec_available(c));
}

TEST_CASE("select_codec — auto_select picks fastest available")  // NOLINT
{
  // Priority order in source: lz4 > snappy > zstd.  On a build with
  // lz4 we expect lz4; without lz4 but with snappy we expect snappy;
  // otherwise zstd (always available).  Test the actual policy
  // expressed by the source by chaining the priority check.
  const auto c = select_codec(CompressionCodec::auto_select);
  CHECK(is_codec_available(c));
  if (is_codec_available(CompressionCodec::lz4)) {
    CHECK(c == CompressionCodec::lz4);
  } else if (is_codec_available(CompressionCodec::snappy)) {
    CHECK(c == CompressionCodec::snappy);
  } else {
    CHECK(c == CompressionCodec::zstd);
  }
}

TEST_CASE("select_codec — file-only codec falls back to zstd")  // NOLINT
{
  // Asking for a file-only codec (bzip2/xz/brotli/lzo) for in-memory
  // use must downgrade to the always-available zstd so the caller
  // never gets a non-functional codec.
  CHECK(select_codec(CompressionCodec::bzip2) == CompressionCodec::zstd);
  CHECK(select_codec(CompressionCodec::xz) == CompressionCodec::zstd);
  CHECK(select_codec(CompressionCodec::brotli) == CompressionCodec::zstd);
  CHECK(select_codec(CompressionCodec::lzo) == CompressionCodec::zstd);
}

TEST_CASE(
    "select_codec returns uncompressed when requested and available")  // NOLINT
{
  const auto c = select_codec(CompressionCodec::uncompressed);
  CHECK(c == CompressionCodec::uncompressed);
}

TEST_CASE("compress/decompress round-trip — uncompressed")  // NOLINT
{
  const auto payload = make_payload(256);
  const auto compressed = compress(payload, CompressionCodec::uncompressed);
  const auto restored =
      decompress(compressed, payload.size(), CompressionCodec::uncompressed);
  CHECK(restored == payload);
}

TEST_CASE("compress/decompress round-trip — zstd")  // NOLINT
{
  if (!is_codec_available(CompressionCodec::zstd)) {
    MESSAGE("zstd not available, skipping");
    return;
  }
  const auto payload = make_payload(4096);
  const auto compressed = compress(payload, CompressionCodec::zstd);
  const auto restored =
      decompress(compressed, payload.size(), CompressionCodec::zstd);
  CHECK(restored == payload);
  // zstd should actually compress a repetitive payload
  CHECK(compressed.size() < payload.size());
}

TEST_CASE("compress/decompress round-trip — lz4")  // NOLINT
{
  if (!is_codec_available(CompressionCodec::lz4)) {
    MESSAGE("lz4 not available, skipping");
    return;
  }
  const auto payload = make_payload(4096);
  const auto compressed = compress(payload, CompressionCodec::lz4);
  const auto restored =
      decompress(compressed, payload.size(), CompressionCodec::lz4);
  CHECK(restored == payload);
}

TEST_CASE("CompressedBuffer default is empty")  // NOLINT
{
  const CompressedBuffer buf {};
  CHECK(buf.empty());
  CHECK(buf.original_size == 0);
  CHECK(buf.codec == CompressionCodec::uncompressed);
}

TEST_CASE("compress_buffer / decompress_data round-trip")  // NOLINT
{
  const auto payload = make_payload(512);
  const auto codec = select_codec(CompressionCodec::zstd);
  const auto cbuf = compress_buffer(payload, codec);
  CHECK_FALSE(cbuf.empty());
  CHECK(cbuf.original_size == payload.size());
  CHECK(cbuf.codec == codec);

  const auto restored = cbuf.decompress_data();
  CHECK(restored == payload);
}

TEST_CASE("compress_buffer with small payload")  // NOLINT
{
  const std::vector<char> payload {'h', 'e', 'l', 'l', 'o'};
  const auto codec = select_codec(CompressionCodec::uncompressed);
  const auto cbuf = compress_buffer(payload, codec);
  CHECK(cbuf.original_size == 5);
  const auto restored = cbuf.decompress_data();
  CHECK(restored == payload);
}

// ── Compression-stats accumulators ──────────────────────────────────────────

TEST_CASE(  // NOLINT
    "compression stats — empty after reset, populated after one round-trip")
{
  reset_compression_stats();
  const auto fresh = get_compression_stats();
  CHECK(fresh.empty());

  // One zstd compress + decompress round-trip.
  const std::vector<char> payload(4096, 'a');
  const auto comp =
      compress({payload.data(), payload.size()}, CompressionCodec::zstd);
  const auto decomp = decompress(
      {comp.data(), comp.size()}, payload.size(), CompressionCodec::zstd);
  CHECK(decomp == payload);

  const auto stats = get_compression_stats();
  REQUIRE(stats.size() == 1);
  CHECK(stats[0].codec == CompressionCodec::zstd);
  CHECK(stats[0].n_compress == 1);
  CHECK(stats[0].n_decompress == 1);
  CHECK(stats[0].compress_in_bytes == payload.size());
  CHECK(stats[0].compress_out_bytes == comp.size());
  CHECK(stats[0].decompress_in_bytes == comp.size());
  CHECK(stats[0].decompress_out_bytes == payload.size());

  // Cleanup so subsequent test cases don't see leftover counters.
  reset_compression_stats();
}

TEST_CASE(  // NOLINT
    "compression stats — separate codecs accumulate independently")
{
  reset_compression_stats();
  const std::vector<char> payload(2048, 'x');

  // zstd round-trip
  const auto cz =
      compress({payload.data(), payload.size()}, CompressionCodec::zstd);
  std::ignore = decompress(
      {cz.data(), cz.size()}, payload.size(), CompressionCodec::zstd);

  // Uncompressed round-trip (always available)
  const auto cu = compress({payload.data(), payload.size()},
                           CompressionCodec::uncompressed);
  std::ignore = decompress(
      {cu.data(), cu.size()}, payload.size(), CompressionCodec::uncompressed);

  const auto stats = get_compression_stats();
  REQUIRE(stats.size() == 2);
  bool saw_zstd = false;
  bool saw_uncomp = false;
  for (const auto& s : stats) {
    if (s.codec == CompressionCodec::zstd) {
      saw_zstd = true;
      CHECK(s.n_compress == 1);
      CHECK(s.n_decompress == 1);
    } else if (s.codec == CompressionCodec::uncompressed) {
      saw_uncomp = true;
      CHECK(s.n_compress == 1);
      CHECK(s.n_decompress == 1);
    }
  }
  CHECK(saw_zstd);
  CHECK(saw_uncomp);

  reset_compression_stats();
}

TEST_CASE(  // NOLINT
    "compression stats — log_compression_stats is silent when nothing used")
{
  reset_compression_stats();
  // Must not throw and must not segfault on empty state.
  log_compression_stats();
  CHECK(get_compression_stats().empty());
}

TEST_CASE(  // NOLINT
    "compression stats — reset clears counters back to zero")
{
  reset_compression_stats();
  const std::vector<char> payload(1024, 'q');
  const auto c =
      compress({payload.data(), payload.size()}, CompressionCodec::zstd);
  std::ignore =
      decompress({c.data(), c.size()}, payload.size(), CompressionCodec::zstd);
  REQUIRE_FALSE(get_compression_stats().empty());

  reset_compression_stats();
  CHECK(get_compression_stats().empty());
}
