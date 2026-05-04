/**
 * @file      test_lp_debug_writer.hpp
 * @brief     Unit tests for LpDebugWriter
 * @date      2026-03-12
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. Default-constructed writer is inactive
 *  2. Empty-directory writer is inactive
 *  3. Non-empty-directory writer is active
 *  4. compress_async with no compression leaves file unchanged
 *  5. compress_async with "gzip" creates .gz and removes original
 *  6. compress_async with "zstd" creates .zst and removes original
 *  7. drain() clears pending futures
 *  8. Unknown codec falls back to cascade
 *  9. Nonexistent file handled gracefully
 */

#include <filesystem>
#include <fstream>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/lp_debug_writer.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Write a small text file to tmp and return its path.
std::filesystem::path make_tmp_lp(const std::string& name)
{
  auto p = std::filesystem::temp_directory_path() / name;
  std::ofstream f(p);
  f << "\\Problem name: test\nMinimize\nobj: x\nBounds\n 0 <= x <= 1\nEnd\n";
  // Pad to make compression meaningful
  for (int i = 0; i < 50; ++i) {
    f << "\\ padding line " << i << " with some text to compress\n";
  }
  return p;
}

/// RAII helper for temporary files
struct TmpFile
{
  std::filesystem::path path;
  explicit TmpFile(const std::string& name)
      : path(make_tmp_lp(name))
  {
  }
  ~TmpFile()
  {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + ".gz", ec);
    std::filesystem::remove(path.string() + ".zst", ec);
    std::filesystem::remove(path.string() + ".lz4", ec);
    std::filesystem::remove(path.string() + ".bz2", ec);
    std::filesystem::remove(path.string() + ".xz", ec);
  }
  TmpFile(const TmpFile&) = delete;
  TmpFile& operator=(const TmpFile&) = delete;
  TmpFile(TmpFile&&) = delete;
  TmpFile& operator=(TmpFile&&) = delete;
};

}  // namespace

TEST_CASE("LpDebugWriter default-constructed is inactive")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const LpDebugWriter writer;
  CHECK_FALSE(writer.is_active());
}

TEST_CASE("LpDebugWriter with empty directory is inactive")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const LpDebugWriter writer("", "gzip");
  CHECK_FALSE(writer.is_active());
}

TEST_CASE("LpDebugWriter with non-empty directory is active")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const LpDebugWriter writer("/tmp/lp_dbg_active_test");
  CHECK(writer.is_active());
}

TEST_CASE(
    "LpDebugWriter compress_async no-op when compression is none")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_lp_nocompress.lp");
  {
    LpDebugWriter writer(tmp.path.parent_path().string(),
                         /*compression=*/"none",
                         /*pool=*/nullptr);
    writer.compress_async(tmp.path.string());
    writer.drain();
  }
  // "none" means no compression; file should still exist unchanged
  CHECK(std::filesystem::exists(tmp.path));
}

TEST_CASE("LpDebugWriter compress_async uncompressed is no-op")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_lp_uncompressed.lp");
  {
    LpDebugWriter writer(tmp.path.parent_path().string(),
                         "uncompressed",
                         /*pool=*/nullptr);
    writer.compress_async(tmp.path.string());
    writer.drain();
  }
  CHECK(std::filesystem::exists(tmp.path));
}

TEST_CASE(
    "LpDebugWriter compress_async gzip creates .gz and removes .lp")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_lp_gzip.lp");
  const auto gz = std::filesystem::path(tmp.path.string() + ".gz");
  std::filesystem::remove(gz);  // ensure clean state

  {
    LpDebugWriter writer(
        tmp.path.parent_path().string(), "gzip", /*pool=*/nullptr);
    writer.compress_async(tmp.path.string());
    writer.drain();
  }

  CHECK(std::filesystem::exists(gz));
  CHECK_FALSE(std::filesystem::exists(tmp.path));
  CHECK(std::filesystem::file_size(gz) > 0);
}

TEST_CASE(
    "LpDebugWriter compress_async zstd creates .zst and removes .lp")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_lp_zstd.lp");
  const auto zst = std::filesystem::path(tmp.path.string() + ".zst");
  std::filesystem::remove(zst);  // ensure clean state

  {
    LpDebugWriter writer(
        tmp.path.parent_path().string(), "zstd", /*pool=*/nullptr);
    writer.compress_async(tmp.path.string());
    writer.drain();
  }

  CHECK(std::filesystem::exists(zst));
  CHECK_FALSE(std::filesystem::exists(tmp.path));
  CHECK(std::filesystem::file_size(zst) > 0);
}

TEST_CASE("LpDebugWriter drain clears futures and is re-entrant")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto lp1 = make_tmp_lp("test_drain1.lp");
  const auto lp2 = make_tmp_lp("test_drain2.lp");
  const auto gz1 = std::filesystem::path(lp1.string() + ".gz");
  const auto gz2 = std::filesystem::path(lp2.string() + ".gz");

  LpDebugWriter writer(lp1.parent_path().string(), "gzip", nullptr);
  writer.compress_async(lp1.string());
  writer.drain();  // first drain
  CHECK(std::filesystem::exists(gz1));

  writer.compress_async(lp2.string());
  writer.drain();  // second drain after first
  CHECK(std::filesystem::exists(gz2));

  std::filesystem::remove(gz1);
  std::filesystem::remove(gz2);
}

TEST_CASE("LpDebugWriter compress_async empty compression is no-op")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_lp_empty_codec.lp");
  {
    LpDebugWriter writer(tmp.path.parent_path().string(),
                         /*compression=*/"",
                         /*pool=*/nullptr);
    writer.compress_async(tmp.path.string());
    writer.drain();
  }
  // Empty compression string means no compression
  CHECK(std::filesystem::exists(tmp.path));
}

TEST_CASE("LpDebugWriter compress_async with nonexistent file")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto bogus_path =
      std::filesystem::temp_directory_path() / "nonexistent_lp_file.lp";
  std::filesystem::remove(bogus_path);  // ensure it doesn't exist

  // Should not throw — compression silently fails
  LpDebugWriter writer(bogus_path.parent_path().string(), "gzip", nullptr);
  writer.compress_async(bogus_path.string());
  writer.drain();

  // No .gz file should have been created
  CHECK_FALSE(std::filesystem::exists(bogus_path.string() + ".gz"));
}

TEST_CASE("LpDebugWriter compress_async zstd with nonexistent file")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto bogus_path =
      std::filesystem::temp_directory_path() / "nonexistent_zstd_file.lp";
  std::filesystem::remove(bogus_path);

  LpDebugWriter writer(bogus_path.parent_path().string(), "zstd", nullptr);
  writer.compress_async(bogus_path.string());
  writer.drain();

  CHECK_FALSE(std::filesystem::exists(bogus_path.string() + ".zst"));
}

TEST_CASE(
    "LpDebugWriter compress_async with unknown codec "
    "falls back")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_lp_unknown_codec.lp");
  const auto zst = std::filesystem::path(tmp.path.string() + ".zst");
  const auto gz = std::filesystem::path(tmp.path.string() + ".gz");
  std::filesystem::remove(zst);
  std::filesystem::remove(gz);

  {
    // "brotli" is not in the codec table — should fall back to zstd or gzip
    LpDebugWriter writer(
        tmp.path.parent_path().string(), "brotli", /*pool=*/nullptr);
    writer.compress_async(tmp.path.string());
    writer.drain();
  }

  // The file should have been compressed via the cascade fallback
  // (either zstd or gzip, depending on what's available)
  const bool compressed = std::filesystem::exists(zst)
      || std::filesystem::exists(gz) || !std::filesystem::exists(tmp.path);
  CHECK(compressed);
}

TEST_CASE("LpDebugWriter drain on inactive writer is safe")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  LpDebugWriter writer;
  CHECK_FALSE(writer.is_active());
  writer.drain();  // should not crash or throw
}

TEST_CASE("LpDebugWriter multiple compress_async + drain cycle")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  constexpr int kCount = 5;
  std::vector<std::filesystem::path> lp_files;
  std::vector<std::filesystem::path> gz_files;

  for (int i = 0; i < kCount; ++i) {
    auto name = std::string("test_lp_multi_") + std::to_string(i) + ".lp";
    lp_files.push_back(make_tmp_lp(name));
    gz_files.emplace_back(lp_files.back().string() + ".gz");
    std::filesystem::remove(gz_files.back());  // ensure clean state
  }

  {
    LpDebugWriter writer(
        lp_files[0].parent_path().string(), "gzip", /*pool=*/nullptr);

    // Submit all compressions before draining
    for (int i = 0; i < kCount; ++i) {
      writer.compress_async(lp_files[i].string());
    }
    writer.drain();
  }

  for (int i = 0; i < kCount; ++i) {
    CHECK(std::filesystem::exists(gz_files[i]));
    CHECK_FALSE(std::filesystem::exists(lp_files[i]));
    CHECK(std::filesystem::file_size(gz_files[i]) > 0);
    std::filesystem::remove(gz_files[i]);
  }
}

TEST_CASE("LpDebugWriter compress_async zstd then gzip in sequence")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Test switching codecs between writer instances
  const TmpFile tmp1("test_lp_zstd_seq.lp");
  const TmpFile tmp2("test_lp_gzip_seq.lp");
  const auto zst = std::filesystem::path(tmp1.path.string() + ".zst");
  const auto gz = std::filesystem::path(tmp2.path.string() + ".gz");
  std::filesystem::remove(zst);
  std::filesystem::remove(gz);

  {
    LpDebugWriter w1(tmp1.path.parent_path().string(), "zstd", nullptr);
    w1.compress_async(tmp1.path.string());
    w1.drain();
  }
  CHECK(std::filesystem::exists(zst));
  CHECK_FALSE(std::filesystem::exists(tmp1.path));

  {
    LpDebugWriter w2(tmp2.path.parent_path().string(), "gzip", nullptr);
    w2.compress_async(tmp2.path.string());
    w2.drain();
  }
  CHECK(std::filesystem::exists(gz));
  CHECK_FALSE(std::filesystem::exists(tmp2.path));
}

TEST_CASE("LpDebugWriter compress with lz4 codec")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_lp_lz4.lp");
  const auto lz4 = std::filesystem::path(tmp.path.string() + ".lz4");
  const auto zst = std::filesystem::path(tmp.path.string() + ".zst");
  const auto gz = std::filesystem::path(tmp.path.string() + ".gz");
  std::filesystem::remove(lz4);
  std::filesystem::remove(zst);
  std::filesystem::remove(gz);

  {
    LpDebugWriter writer(
        tmp.path.parent_path().string(), "lz4", /*pool=*/nullptr);
    writer.compress_async(tmp.path.string());
    writer.drain();
  }

  // lz4 may not be installed; the cascade should produce some compression
  const bool compressed = std::filesystem::exists(lz4)
      || std::filesystem::exists(zst) || std::filesystem::exists(gz)
      || !std::filesystem::exists(tmp.path);
  CHECK(compressed);
}

TEST_CASE("LpDebugWriter compress with bzip2 codec")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_lp_bzip2.lp");
  const auto bz2 = std::filesystem::path(tmp.path.string() + ".bz2");
  const auto zst = std::filesystem::path(tmp.path.string() + ".zst");
  const auto gz = std::filesystem::path(tmp.path.string() + ".gz");
  std::filesystem::remove(bz2);
  std::filesystem::remove(zst);
  std::filesystem::remove(gz);

  {
    LpDebugWriter writer(
        tmp.path.parent_path().string(), "bzip2", /*pool=*/nullptr);
    writer.compress_async(tmp.path.string());
    writer.drain();
  }

  // bzip2 may or may not be installed; cascade covers it
  const bool compressed = std::filesystem::exists(bz2)
      || std::filesystem::exists(zst) || std::filesystem::exists(gz)
      || !std::filesystem::exists(tmp.path);
  CHECK(compressed);
}

TEST_CASE("LpDebugWriter compress with xz codec")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_lp_xz.lp");
  const auto xz = std::filesystem::path(tmp.path.string() + ".xz");
  const auto zst = std::filesystem::path(tmp.path.string() + ".zst");
  const auto gz = std::filesystem::path(tmp.path.string() + ".gz");
  std::filesystem::remove(xz);
  std::filesystem::remove(zst);
  std::filesystem::remove(gz);

  {
    LpDebugWriter writer(
        tmp.path.parent_path().string(), "xz", /*pool=*/nullptr);
    writer.compress_async(tmp.path.string());
    writer.drain();
  }

  const bool compressed = std::filesystem::exists(xz)
      || std::filesystem::exists(zst) || std::filesystem::exists(gz)
      || !std::filesystem::exists(tmp.path);
  CHECK(compressed);
}

// ── Direct tests for inline compression functions ──

TEST_CASE("gzip_lp_file_inline compresses and removes original")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_gzip_inline.lp");
  const auto result = gzip_lp_file_inline(tmp.path.string());

  REQUIRE_FALSE(result.empty());
  CHECK(result == tmp.path.string() + ".gz");
  CHECK(std::filesystem::exists(result));
  CHECK(std::filesystem::file_size(result) > 0);
  CHECK_FALSE(std::filesystem::exists(tmp.path));
}

TEST_CASE("gzip_lp_file_inline with nonexistent file returns empty")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto result = gzip_lp_file_inline("/tmp/nonexistent_gzip_inline.lp");
  CHECK(result.empty());
}

TEST_CASE("zstd_lp_file_inline compresses and removes original")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_zstd_inline.lp");
  const auto result = zstd_lp_file_inline(tmp.path.string());

  REQUIRE_FALSE(result.empty());
  CHECK(result == tmp.path.string() + ".zst");
  CHECK(std::filesystem::exists(result));
  CHECK(std::filesystem::file_size(result) > 0);
  CHECK_FALSE(std::filesystem::exists(tmp.path));
}

TEST_CASE("zstd_lp_file_inline with nonexistent file returns empty")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto result = zstd_lp_file_inline("/tmp/nonexistent_zstd_inline.lp");
  CHECK(result.empty());
}

TEST_CASE("compress_lp_file with none returns src_path")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_compress_none.lp");
  const auto result = compress_lp_file(tmp.path.string(), "none");
  CHECK(result == tmp.path.string());
  CHECK(std::filesystem::exists(tmp.path));
}

TEST_CASE("compress_lp_file with auto cascade")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_compress_auto.lp");
  // Empty string triggers auto-cascade
  const auto result = compress_lp_file(tmp.path.string(), "");

  // Should have compressed via one of the available methods
  CHECK_FALSE(result.empty());
  CHECK(std::filesystem::exists(result));
  CHECK_FALSE(std::filesystem::exists(tmp.path));
}

TEST_CASE("compress_lp_file with explicit gzip")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_compress_gzip_direct.lp");
  const auto result = compress_lp_file(tmp.path.string(), "gzip");

  CHECK_FALSE(result.empty());
  CHECK(std::filesystem::exists(result));
  // Should end with .gz
  CHECK(result.ends_with(".gz"));
}

TEST_CASE("compress_lp_file with explicit zstd")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_compress_zstd_direct.lp");
  const auto result = compress_lp_file(tmp.path.string(), "zstd");

  CHECK_FALSE(result.empty());
  CHECK(std::filesystem::exists(result));
  // Should end with .zst
  CHECK(result.ends_with(".zst"));
}

TEST_CASE("compress_lp_file with unknown codec falls back")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_compress_unknown.lp");
  const auto result = compress_lp_file(tmp.path.string(), "brotli");

  // Should fall back to zstd or gzip cascade
  CHECK_FALSE(result.empty());
  CHECK(std::filesystem::exists(result));
}

TEST_CASE("compress_lp_file with nonexistent file")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // compress_lp_file should handle gracefully
  const auto result =
      compress_lp_file("/tmp/nonexistent_compress_test.lp", "zstd");
  // Falls through entire cascade; returns the original path
  CHECK_FALSE(result.empty());
}

// ─── LpDebugWriter class tests ──────────────────────────────────────────────

TEST_CASE("LpDebugWriter inactive when directory empty")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  LpDebugWriter writer("", "", nullptr);
  CHECK_FALSE(writer.is_active());
}

TEST_CASE("LpDebugWriter active when directory set")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp = std::filesystem::temp_directory_path() / "lp_debug_test";
  LpDebugWriter writer(tmp.string(), "none", nullptr);
  CHECK(writer.is_active());
  std::filesystem::remove_all(tmp);
}

TEST_CASE("LpDebugWriter drain with no futures is safe")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  LpDebugWriter writer("", "", nullptr);
  writer.drain();  // should not throw
}

TEST_CASE("compress_lp_file with codec none returns path")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_codec_none.lp");
  const auto result = compress_lp_file(tmp.path.string(), "none");
  CHECK(result == tmp.path.string());
}

TEST_CASE("gzip_lp_file_inline compresses and removes source")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_gzip_inline.lp");
  const auto result = gzip_lp_file_inline(tmp.path.string());
  CHECK_FALSE(result.empty());
  CHECK(result.ends_with(".gz"));
  CHECK(std::filesystem::exists(result));
  CHECK_FALSE(std::filesystem::exists(tmp.path));
  std::filesystem::remove(result);
}

TEST_CASE("zstd_lp_file_inline compresses and removes source")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const TmpFile tmp("test_zstd_inline.lp");
  const auto result = zstd_lp_file_inline(tmp.path.string());
  CHECK_FALSE(result.empty());
  CHECK(result.ends_with(".zst"));
  CHECK(std::filesystem::exists(result));
  CHECK_FALSE(std::filesystem::exists(tmp.path));
  std::filesystem::remove(result);
}

TEST_CASE("lp_debug_passes_includes — pass-list selector")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("empty string → legacy default forward+aperture, NOT backward")
  {
    CHECK(lp_debug_passes_includes("", LpDebugPass::forward));
    CHECK(lp_debug_passes_includes("", LpDebugPass::aperture));
    CHECK_FALSE(lp_debug_passes_includes("", LpDebugPass::backward));
    // Whitespace-only counts as empty.
    CHECK(lp_debug_passes_includes(" \t ", LpDebugPass::forward));
    CHECK_FALSE(lp_debug_passes_includes(" \t ", LpDebugPass::backward));
  }

  SUBCASE("single token matches only itself")
  {
    CHECK(lp_debug_passes_includes("backward", LpDebugPass::backward));
    CHECK_FALSE(lp_debug_passes_includes("backward", LpDebugPass::forward));
    CHECK_FALSE(lp_debug_passes_includes("backward", LpDebugPass::aperture));
  }

  SUBCASE("comma-separated list with spaces")
  {
    CHECK(lp_debug_passes_includes("forward, backward", LpDebugPass::forward));
    CHECK(lp_debug_passes_includes("forward, backward", LpDebugPass::backward));
    CHECK_FALSE(
        lp_debug_passes_includes("forward, backward", LpDebugPass::aperture));
  }

  SUBCASE("'all' matches every pass")
  {
    CHECK(lp_debug_passes_includes("all", LpDebugPass::forward));
    CHECK(lp_debug_passes_includes("all", LpDebugPass::backward));
    CHECK(lp_debug_passes_includes("all", LpDebugPass::aperture));
    // Mixed case
    CHECK(lp_debug_passes_includes("ALL", LpDebugPass::backward));
    CHECK(lp_debug_passes_includes("All", LpDebugPass::backward));
  }

  SUBCASE("unknown tokens are silently skipped")
  {
    CHECK_FALSE(lp_debug_passes_includes("nonsense", LpDebugPass::forward));
    CHECK(
        lp_debug_passes_includes("nonsense, backward", LpDebugPass::backward));
  }

  SUBCASE("case-insensitive token match")
  {
    CHECK(lp_debug_passes_includes("BACKWARD", LpDebugPass::backward));
    CHECK(lp_debug_passes_includes("Backward", LpDebugPass::backward));
    CHECK(lp_debug_passes_includes("APERTURE", LpDebugPass::aperture));
  }
}
