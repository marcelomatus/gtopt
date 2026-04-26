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

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

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

TEST_CASE("codec_name returns non-empty string for known codecs")  // NOLINT
{
  CHECK_FALSE(codec_name(CompressionCodec::uncompressed).empty());
  CHECK_FALSE(codec_name(CompressionCodec::zstd).empty());
  CHECK_FALSE(codec_name(CompressionCodec::lz4).empty());
}

TEST_CASE("select_codec falls back to an available codec")  // NOLINT
{
  const auto c = select_codec(CompressionCodec::zstd);
  CHECK(is_codec_available(c));
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
