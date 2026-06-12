// SPDX-License-Identifier: BSD-3-Clause
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/resolve_planning_args.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

/// RAII helper: create a temporary directory tree, remove on destruction.
struct TmpDir
{
  std::filesystem::path path;

  explicit TmpDir(const std::string& name)
      : path(std::filesystem::temp_directory_path() / name)
  {
    std::filesystem::create_directories(path);
  }

  ~TmpDir() { std::filesystem::remove_all(path); }

  TmpDir(const TmpDir&) = delete;
  TmpDir& operator=(const TmpDir&) = delete;
  TmpDir(TmpDir&&) = delete;
  TmpDir& operator=(TmpDir&&) = delete;
};

/// Create a minimal JSON file at the given path.
void touch_json(const std::filesystem::path& p)
{
  std::filesystem::create_directories(p.parent_path());
  std::ofstream ofs(p);
  ofs << "{}";
}

}  // namespace

TEST_CASE("resolve_planning_args — directory argument")  // NOLINT
{
  const TmpDir tmp("test_resolve_dir_arg");
  const auto case_dir = tmp.path / "my_case";
  std::filesystem::create_directories(case_dir);
  touch_json(case_dir / "my_case.json");

  const MainOptions opts {
      .planning_files = {case_dir.string()},
  };

  auto result = resolve_planning_args(opts);
  REQUIRE(result.has_value());
  const auto& resolved = *result;

  SUBCASE("planning file is resolved to dir/basename.json")
  {
    REQUIRE(resolved.planning_files.size() == 1);
    CHECK(resolved.planning_files[0] == (case_dir / "my_case.json").string());
  }

  SUBCASE("input_directory is set to the directory itself")
  {
    REQUIRE(resolved.input_directory.has_value());
    CHECK(resolved.input_directory.value_or("") == case_dir.string());
  }

  SUBCASE("output_directory is set to dir/results")
  {
    REQUIRE(resolved.output_directory.has_value());
    CHECK(resolved.output_directory.value_or("")
          == (case_dir / "results").string());
  }

  SUBCASE("log_directory is set to dir/results/logs")
  {
    REQUIRE(resolved.log_directory.has_value());
    CHECK(resolved.log_directory.value_or("")
          == (case_dir / "results" / "logs").string());
  }

  SUBCASE("cut_directory is set relative to dir")
  {
    REQUIRE(resolved.cut_directory.has_value());
    CHECK(resolved.cut_directory.value_or("") == (case_dir / "cuts").string());
  }

  SUBCASE("dirs_auto_resolved is true")
  {
    CHECK(resolved.dirs_auto_resolved);
  }
}

TEST_CASE("resolve_planning_args — directory missing json")  // NOLINT
{
  const TmpDir tmp("test_resolve_dir_no_json");
  const auto case_dir = tmp.path / "empty_case";
  std::filesystem::create_directories(case_dir);

  const MainOptions opts {
      .planning_files = {case_dir.string()},
  };

  auto result = resolve_planning_args(opts);
  CHECK_FALSE(result.has_value());
  CHECK(result.error().find("exist") != std::string::npos);
}

TEST_CASE("resolve_planning_args — bare stem fallback")  // NOLINT
{
  const TmpDir tmp("test_resolve_stem");
  touch_json(tmp.path / "my_case.json");

  // Pass "my_case" (no extension, not a directory) but the .json file exists
  const auto stem = (tmp.path / "my_case").string();
  const MainOptions opts {
      .planning_files = {stem},
  };

  auto result = resolve_planning_args(opts);
  REQUIRE(result.has_value());
  REQUIRE(result->planning_files.size() == 1);
  CHECK(result->planning_files[0] == (tmp.path / "my_case.json").string());
}

TEST_CASE("resolve_planning_args — existing file unchanged")  // NOLINT
{
  const TmpDir tmp("test_resolve_existing");
  const auto json_path = tmp.path / "plan.json";
  touch_json(json_path);

  const MainOptions opts {
      .planning_files = {json_path.string()},
  };

  auto result = resolve_planning_args(opts);
  REQUIRE(result.has_value());
  REQUIRE(result->planning_files.size() == 1);
  CHECK(result->planning_files[0] == json_path.string());
  // Directories should NOT be set (no directory inference for explicit files)
  CHECK_FALSE(result->input_directory.has_value());
  CHECK_FALSE(result->output_directory.has_value());
}

TEST_CASE(
    "resolve_planning_args — directory does not override explicit "
    "directories")  // NOLINT
{
  const TmpDir tmp("test_resolve_no_override");
  const auto case_dir = tmp.path / "my_case";
  std::filesystem::create_directories(case_dir);
  touch_json(case_dir / "my_case.json");

  const MainOptions opts {
      .planning_files = {case_dir.string()},
      .input_directory = "/custom/input",
      .output_directory = "/custom/output",
  };

  auto result = resolve_planning_args(opts);
  REQUIRE(result.has_value());
  CHECK(result->input_directory.value_or("") == "/custom/input");
  CHECK(result->output_directory.value_or("") == "/custom/output");
}

TEST_CASE("resolve_planning_args — auto-detect CWD")  // NOLINT
{
  const TmpDir tmp("test_resolve_cwd");
  const auto case_dir = tmp.path / "auto_case";
  std::filesystem::create_directories(case_dir);
  touch_json(case_dir / "auto_case.json");

  // Change to the case directory, then call with empty planning_files
  const auto original_cwd = std::filesystem::current_path();
  std::filesystem::current_path(case_dir);

  const MainOptions opts {};
  auto result = resolve_planning_args(opts);

  // Restore CWD before any CHECK (to avoid leaving CWD in tmp on failure)
  std::filesystem::current_path(original_cwd);

  REQUIRE(result.has_value());
  REQUIRE(result->planning_files.size() == 1);
  CHECK(result->planning_files[0] == (case_dir / "auto_case.json").string());
  // No directory overrides — JSON relative paths resolve against CWD
  CHECK_FALSE(result->input_directory.has_value());
}

TEST_CASE("resolve_planning_args — trailing-slash directory")  // NOLINT
{
  const TmpDir tmp("test_resolve_trailing_slash");
  const auto case_dir = tmp.path / "slash_case";
  std::filesystem::create_directories(case_dir);
  touch_json(case_dir / "slash_case.json");

  // Pass the path WITH a trailing slash
  const MainOptions opts {
      .planning_files = {(case_dir.string() + "/")},
  };

  auto result = resolve_planning_args(opts);
  REQUIRE(result.has_value());
  const auto& resolved = *result;

  SUBCASE("planning file is resolved to dir/basename.json")
  {
    REQUIRE(resolved.planning_files.size() == 1);
    CHECK(resolved.planning_files[0]
          == (case_dir / "slash_case.json").string());
  }

  SUBCASE("input_directory is set")
  {
    REQUIRE(resolved.input_directory.has_value());
    // The trailing-slash path is kept as-is by std::filesystem
    CHECK(resolved.input_directory.value_or("").find("slash_case")
          != std::string::npos);
  }

  SUBCASE("output_directory is set to dir/results")
  {
    REQUIRE(resolved.output_directory.has_value());
    CHECK(resolved.output_directory.value_or("").find("results")
          != std::string::npos);
  }
}

TEST_CASE("resolve_planning_args — multiple planning files")  // NOLINT
{
  const TmpDir tmp("test_resolve_multi");

  // First argument: a directory (triggers directory overrides)
  const auto case_dir = tmp.path / "dir_case";
  std::filesystem::create_directories(case_dir);
  touch_json(case_dir / "dir_case.json");

  // Second argument: an explicit .json file (no directory overrides)
  const auto extra_json = tmp.path / "extra.json";
  touch_json(extra_json);

  const MainOptions opts {
      .planning_files = {case_dir.string(), extra_json.string()},
  };

  auto result = resolve_planning_args(opts);
  REQUIRE(result.has_value());
  const auto& resolved = *result;

  SUBCASE("both planning files are resolved")
  {
    REQUIRE(resolved.planning_files.size() == 2);
    CHECK(resolved.planning_files[0] == (case_dir / "dir_case.json").string());
    CHECK(resolved.planning_files[1] == extra_json.string());
  }

  SUBCASE("directories are set from the directory argument")
  {
    REQUIRE(resolved.input_directory.has_value());
    CHECK(resolved.input_directory.value_or("") == case_dir.string());
    REQUIRE(resolved.output_directory.has_value());
    CHECK(resolved.output_directory.value_or("")
          == (case_dir / "results").string());
  }
}

TEST_CASE("resolve_planning_args — no files, no auto-detect")  // NOLINT
{
  const TmpDir tmp("test_resolve_empty");
  const auto empty_dir = tmp.path / "no_json_here";
  std::filesystem::create_directories(empty_dir);

  const auto original_cwd = std::filesystem::current_path();
  std::filesystem::current_path(empty_dir);

  const MainOptions opts {};
  auto result = resolve_planning_args(opts);

  std::filesystem::current_path(original_cwd);

  CHECK_FALSE(result.has_value());
  CHECK(result.error().find("no planning files given") != std::string::npos);
}

TEST_CASE(
    "resolve_planning_args — file in CWD has no directory "
    "overrides")  // NOLINT
{
  // When the planning file is specified without a directory prefix
  // (just "case.json"), no directory overrides should be set because
  // relative paths in the JSON already resolve against CWD correctly.
  const TmpDir tmp("test_resolve_cwd_file");
  const auto case_dir = tmp.path / "my_case";
  std::filesystem::create_directories(case_dir);
  touch_json(case_dir / "my_case.json");

  const auto original_cwd = std::filesystem::current_path();
  std::filesystem::current_path(case_dir);

  const MainOptions opts {
      .planning_files = {"my_case.json"},
  };

  auto result = resolve_planning_args(opts);
  std::filesystem::current_path(original_cwd);

  REQUIRE(result.has_value());
  REQUIRE(result->planning_files.size() == 1);
  CHECK(result->planning_files[0] == "my_case.json");
  CHECK_FALSE(result->input_directory.has_value());
  CHECK_FALSE(result->output_directory.has_value());
}
