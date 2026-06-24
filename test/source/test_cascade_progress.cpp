/**
 * @file      test_cascade_progress.cpp
 * @brief     Round-trip + atomic-write tests for cascade_progress.
 * @copyright BSD-3-Clause
 */

// SPDX-License-Identifier: BSD-3-Clause

#include <filesystem>
#include <fstream>
#include <random>

#include <doctest/doctest.h>
#include <gtopt/cascade_progress.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// One-shot temp-directory scope.  Removes itself on destruction so each
/// test gets a clean slate; the doctest harness already isolates tests
/// in process scope, this just keeps the filesystem tidy.
struct TempDir
{
  std::filesystem::path path;

  TempDir()
  {
    namespace fs = std::filesystem;
    auto p = fs::temp_directory_path() / "gtopt_cascade_progress_test";
    p /= std::to_string(static_cast<std::int64_t>(std::random_device {}()));
    fs::create_directories(p);
    path = p;
  }

  ~TempDir()
  {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  auto operator=(const TempDir&) -> TempDir& = delete;
  auto operator=(TempDir&&) -> TempDir& = delete;
};

}  // namespace

TEST_CASE("CascadeProgress JSON round-trip")  // NOLINT
{
  TempDir tmp;
  const auto file = tmp.path / "cascade_progress.json";

  CascadeProgress p;
  p.schema_version = 1;
  p.run_id = "test-run-2026-05-20";
  p.levels.resize(3);

  p.levels[0].index = 0;
  p.levels[0].name = "uninodal";
  p.levels[0].status = CascadeLevelStatus::done;
  p.levels[0].converged = true;
  p.levels[0].iters = 4;
  p.levels[0].global_iter_after = 4;
  p.levels[0].cuts_file = "uninodal/cuts.parquet";

  p.levels[1].index = 1;
  p.levels[1].name = "transport";
  p.levels[1].status = CascadeLevelStatus::done;
  p.levels[1].converged = false;
  p.levels[1].iters = 12;
  p.levels[1].global_iter_after = 16;
  p.levels[1].cuts_file = "transport/cuts.parquet";

  p.levels[2].index = 2;
  p.levels[2].name = "full_network";
  p.levels[2].status = CascadeLevelStatus::in_progress;
  p.levels[2].converged = false;
  p.levels[2].iters = 5;
  p.levels[2].global_iter_after = 21;
  p.levels[2].cuts_file = "cuts.parquet";

  auto save_res = save_cascade_progress(p, file);
  REQUIRE(save_res.has_value());

  CHECK(std::filesystem::exists(file));
  // .tmp must not linger after a successful rename.
  CHECK_FALSE(std::filesystem::exists(file.string() + ".tmp"));

  auto loaded = load_cascade_progress(file);
  REQUIRE(loaded.has_value());

  CHECK(loaded->schema_version == 1);
  CHECK(loaded->run_id == p.run_id);
  REQUIRE(loaded->levels.size() == p.levels.size());

  for (std::size_t i = 0; i < p.levels.size(); ++i) {
    CHECK(loaded->levels[i].index == p.levels[i].index);
    CHECK(loaded->levels[i].name == p.levels[i].name);
    CHECK(loaded->levels[i].status == p.levels[i].status);
    CHECK(loaded->levels[i].converged == p.levels[i].converged);
    CHECK(loaded->levels[i].iters == p.levels[i].iters);
    CHECK(loaded->levels[i].global_iter_after == p.levels[i].global_iter_after);
    CHECK(loaded->levels[i].cuts_file == p.levels[i].cuts_file);
  }
}

TEST_CASE(
    "CascadeProgress: load() on missing file returns FileIOError")  // NOLINT
{
  TempDir tmp;
  auto res = load_cascade_progress(tmp.path / "nonexistent.json");
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error().code == ErrorCode::FileIOError);
}

TEST_CASE("CascadeProgress: malformed JSON returns InvalidInput")  // NOLINT
{
  TempDir tmp;
  const auto file = tmp.path / "bad.json";
  {
    std::ofstream out(file);
    out << "{ this is not JSON ";
  }
  auto res = load_cascade_progress(file);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error().code == ErrorCode::InvalidInput);
}

TEST_CASE("CascadeProgress: empty levels round-trip")  // NOLINT
{
  TempDir tmp;
  const auto file = tmp.path / "empty.json";

  CascadeProgress p;
  p.run_id = "empty";

  REQUIRE(save_cascade_progress(p, file).has_value());
  auto loaded = load_cascade_progress(file);
  REQUIRE(loaded.has_value());
  CHECK(loaded->run_id == "empty");
  CHECK(loaded->levels.empty());
}

TEST_CASE("save_cascade_progress: directory is created on demand")  // NOLINT
{
  TempDir tmp;
  const auto file = tmp.path / "nested" / "deeper" / "progress.json";
  CHECK_FALSE(std::filesystem::exists(file.parent_path()));

  CascadeProgress p;
  p.run_id = "deep";

  REQUIRE(save_cascade_progress(p, file).has_value());
  CHECK(std::filesystem::exists(file));
  CHECK(std::filesystem::is_directory(file.parent_path()));
}

TEST_CASE(
    "CascadeProgress: pre-existing .tmp does not break atomic write")  // NOLINT
{
  TempDir tmp;
  const auto file = tmp.path / "progress.json";
  const auto tmp_file = file.string() + ".tmp";

  // Stale .tmp from a previous crashed run.
  {
    std::ofstream out(tmp_file);
    out << "garbage-from-previous-run";
  }
  CHECK(std::filesystem::exists(tmp_file));

  CascadeProgress p;
  p.run_id = "x";
  REQUIRE(save_cascade_progress(p, file).has_value());

  CHECK(std::filesystem::exists(file));
  CHECK_FALSE(std::filesystem::exists(tmp_file));
}
