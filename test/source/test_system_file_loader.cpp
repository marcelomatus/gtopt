// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_system_file_loader.cpp
 * @brief     Unit tests for gtopt/system_file_loader.hpp.
 *
 * Covers the path-resolution and external-Planning-JSON loading used by the
 * cascade `system_file` and SDDP `aperture_system_file` features:
 *   - resolve_system_file: absolute / CWD-relative / input_directory fallback
 *     / not-found fall-through (no throw);
 *   - load_system_with_model_options: extracts `.system` + `.options.
 *     model_options`, ignores `.simulation`, throws on a missing file.
 */

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

#include <doctest/doctest.h>
#include <gtopt/system_file_loader.hpp>
#include <unistd.h>  // getpid

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// Unique-prefixed names so the anonymous-namespace helpers don't collide
// with other test files when CMake unity-batches them.
namespace
{
/// Honour the project $TMPDIR convention (never hardcode /tmp); fall back to
/// the std temp dir only when TMPDIR is unset.
[[nodiscard]] std::filesystem::path sfl_tmp_base()
{
  const char* const td =
      std::getenv("TMPDIR");  // NOLINT(concurrency-mt-unsafe)
  return (td != nullptr && *td != '\0')
      ? std::filesystem::path(td)
      : std::filesystem::temp_directory_path();
}

[[nodiscard]] std::filesystem::path sfl_make_unique_dir(std::string_view tag)
{
  // PID-qualify so the dir is genuinely unique per process — distinct tags
  // already avoid collisions WITHIN one ctest run, but without the pid two
  // concurrent runs of the suite (CI shards, a developer re-running) would
  // share `gtopt_sfl_<tag>` and the remove_all() below could delete the
  // other run's files mid-test.
  auto dir = sfl_tmp_base()
      / std::filesystem::path(std::string("gtopt_sfl_") + std::string(tag) + "_"
                              + std::to_string(::getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

constexpr std::string_view kSflPlanningJson = R"({
  "options": {
    "model_options": {"use_single_bus": true, "demand_fail_cost": 999.0}
  },
  "simulation": {
    "scenario_array": [{"uid": 42, "probability_factor": 1.0}]
  },
  "system": {"name": "reduced_sys"}
})";
}  // namespace

TEST_CASE("resolve_system_file: absolute path is returned as-is")
{
  const auto dir = sfl_make_unique_dir("abs");
  const auto file = dir / "sys.json";
  {
    std::ofstream out(file);
    out << "{}";
  }
  REQUIRE(file.is_absolute());

  // Absolute path wins regardless of input_directory.
  CHECK(resolve_system_file(file.string(), "") == file);
  CHECK(resolve_system_file(file.string(), "/some/other/dir") == file);

  std::filesystem::remove_all(dir);
}

TEST_CASE(
    "resolve_system_file: CWD-relative miss falls back to input_directory")
{
  const auto dir = sfl_make_unique_dir("fallback");
  const auto file = dir / "reduced.json";
  {
    std::ofstream out(file);
    out << "{}";
  }

  // The bare filename does not exist relative to CWD, so resolution falls
  // back to <input_directory>/<filename>.
  const auto resolved = resolve_system_file("reduced.json", dir.string());
  CHECK(resolved == dir / "reduced.json");
  CHECK(std::filesystem::exists(resolved));

  std::filesystem::remove_all(dir);
}

TEST_CASE("resolve_system_file: not found anywhere → original path, no throw")
{
  const auto dir = sfl_make_unique_dir("missing");
  // Neither CWD nor input_directory contains the file → the original
  // (relative) path is returned and the caller decides how to surface it.
  const auto resolved = resolve_system_file("ghost.json", dir.string());
  CHECK(resolved.string() == "ghost.json");
  CHECK_FALSE(std::filesystem::exists(resolved));

  std::filesystem::remove_all(dir);
}

TEST_CASE("load_system_with_model_options: throws on a missing file")
{
  REQUIRE_THROWS_AS(
      std::ignore = load_system_with_model_options("/nonexistent/x.json", ""),
      std::runtime_error);

  // The error message names the offending file.
  try {
    std::ignore = load_system_with_model_options("ghost.json", "/nonexistent");
  } catch (const std::runtime_error& e) {
    const std::string_view msg {e.what()};
    CHECK(msg.find("ghost.json") != std::string_view::npos);
  }
}

TEST_CASE("load_system_with_model_options: extracts system + model_options")
{
  const auto dir = sfl_make_unique_dir("extract");
  const auto file = dir / "planning.json";
  {
    std::ofstream out(file);
    out << kSflPlanningJson;
  }

  const auto loaded = load_system_with_model_options(file.string(), "");

  SUBCASE("system block is loaded")
  {
    CHECK(loaded.system.name == "reduced_sys");
  }

  SUBCASE("model_options come from .options.model_options")
  {
    CHECK((loaded.model_options.use_single_bus.has_value()
           && *loaded.model_options.use_single_bus));  // NOLINT
    CHECK(loaded.model_options.demand_fail_cost.value_or(0.0)
          == doctest::Approx(999.0));
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("load_system_from_file: delegates and drops model_options")
{
  const auto dir = sfl_make_unique_dir("delegate");
  const auto file = dir / "planning.json";
  {
    std::ofstream out(file);
    out << kSflPlanningJson;
  }

  const auto system = load_system_from_file(file.string(), "");
  CHECK(system.name == "reduced_sys");

  std::filesystem::remove_all(dir);
}
