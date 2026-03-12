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
 *  6. drain() clears pending futures
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
  return p;
}

}  // namespace

TEST_CASE("LpDebugWriter default-constructed is inactive")  // NOLINT
{
  LpDebugWriter writer;
  CHECK_FALSE(writer.is_active());
}

TEST_CASE("LpDebugWriter with empty directory is inactive")  // NOLINT
{
  LpDebugWriter writer("", "gzip");
  CHECK_FALSE(writer.is_active());
}

TEST_CASE("LpDebugWriter with non-empty directory is active")  // NOLINT
{
  LpDebugWriter writer("/tmp/lp_dbg_active_test");
  CHECK(writer.is_active());
}

TEST_CASE(
    "LpDebugWriter compress_async no-op when compression empty")  // NOLINT
{
  const auto lp = make_tmp_lp("test_lp_nocompress.lp");
  {
    LpDebugWriter writer(lp.parent_path().string(),
                         /*compression=*/"",
                         /*pool=*/nullptr);
    writer.compress_async(lp.string());
    writer.drain();
  }
  // File should still exist (no compression requested)
  CHECK(std::filesystem::exists(lp));
  std::filesystem::remove(lp);
}

TEST_CASE("LpDebugWriter compress_async uncompressed is no-op")  // NOLINT
{
  const auto lp = make_tmp_lp("test_lp_uncompressed.lp");
  {
    LpDebugWriter writer(lp.parent_path().string(),
                         "uncompressed",
                         /*pool=*/nullptr);
    writer.compress_async(lp.string());
    writer.drain();
  }
  CHECK(std::filesystem::exists(lp));
  std::filesystem::remove(lp);
}

TEST_CASE(
    "LpDebugWriter compress_async gzip creates .gz and removes .lp")  // NOLINT
{
  const auto lp = make_tmp_lp("test_lp_gzip.lp");
  const auto gz = std::filesystem::path(lp.string() + ".gz");
  std::filesystem::remove(gz);  // ensure clean state

  {
    LpDebugWriter writer(lp.parent_path().string(), "gzip", /*pool=*/nullptr);
    writer.compress_async(lp.string());
    writer.drain();
  }

  CHECK(std::filesystem::exists(gz));
  CHECK_FALSE(std::filesystem::exists(lp));
  CHECK(std::filesystem::file_size(gz) > 0);
  std::filesystem::remove(gz);
}

TEST_CASE("LpDebugWriter drain clears futures and is re-entrant")  // NOLINT
{
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
