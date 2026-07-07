// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_gtopt_json_io.cpp
 * @brief     Unit tests for `write_json_output` (`gtopt_json_io_write.cpp`)
 *            and `load_user_constraints` (`gtopt_json_io_uc.cpp`).
 *
 * Targets:
 *   * Round-trip: parse a JSON Planning, write it back via
 *     `write_json_output`, re-parse, fields preserved.
 *   * Error branches: filesystem failure, JSON serialization exception
 *     (best-effort — the exception path is hard to provoke from a
 *     well-formed Planning, so we exercise the file-open failure
 *     instead).
 *   * `load_user_constraints` happy paths (JSON array, .pampl) and
 *     error paths (missing file, malformed JSON).
 *
 * These two TUs are otherwise only exercised end-to-end via
 * `test_gtopt_main` (which forks the standalone binary).  Direct
 * coverage closes the corner-case gaps the integration test misses.
 */

#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning.hpp>

using namespace gtopt;
// NOLINTBEGIN(readability-trailing-comma)

namespace
{

[[nodiscard]] auto unique_tmpdir(std::string_view name) -> std::filesystem::path
{
  auto p = std::filesystem::temp_directory_path()
      / std::format("gtopt_test_json_io_{}_{}", name, ::getpid());
  std::filesystem::create_directories(p);
  return p;
}

}  // namespace

// ─── write_json_output ─────────────────────────────────────────────────────

TEST_CASE("write_json_output: round-trips a minimal Planning")  // NOLINT
{
  // Build a tiny but structurally complete Planning (single bus,
  // single demand, single block/stage), serialise via
  // `write_json_output`, re-parse via `parse_json_input`, and assert
  // the round-trip preserves the system identity and array sizes.
  PlanningOptions options {};
  options.input_directory = "input";
  options.output_directory = "output";
  options.model_options.demand_fail_cost = 1000.0;
  Planning planning {
      .options = options,
      .simulation =
          {
              .block_array = {{.uid = Uid {1}, .duration = 1.0}},
              .stage_array = {{.uid = Uid {1},
                               .first_block = 0,
                               .count_block = 1}},
              .scenario_array = {{.uid = Uid {0}}},
          },
      .system =
          {
              .name = "MINI_RT",
              .bus_array = {{.uid = Uid {1}, .name = "b1"}},
              .demand_array = {{
                  .uid = Uid {1},
                  .name = "d1",
                  .bus = Uid {1},
                  .forced = true,
                  .capacity = 100.0,
              }},
              .generator_array = {{
                  .uid = Uid {1},
                  .name = "g1",
                  .bus = Uid {1},
                  .gcost = 50.0,
                  .capacity = 1000.0,
              }},
          },
  };

  const auto dir = unique_tmpdir("rt_minimal");
  const auto out_path = (dir / "out.json").string();

  // Success path.
  const auto write_result = write_json_output(planning, out_path);
  REQUIRE(write_result.has_value());
  REQUIRE(std::filesystem::exists(dir / "out.json"));

  // Re-parse and verify identity preservation.
  const auto parse_result = parse_planning_files({(dir / "out.json").string()});
  REQUIRE(parse_result.has_value());
  const auto& roundtrip = *parse_result;
  CHECK(roundtrip.system.name == "MINI_RT");
  CHECK(roundtrip.system.bus_array.size() == 1);
  CHECK(roundtrip.system.bus_array.front().name == "b1");
  CHECK(roundtrip.system.demand_array.size() == 1);
  CHECK(roundtrip.system.generator_array.size() == 1);
  CHECK(roundtrip.simulation.block_array.size() == 1);

  std::filesystem::remove_all(dir);
}

TEST_CASE("write_json_output: extension is normalised to .json")  // NOLINT
{
  // The implementation calls `jpath.replace_extension(".json")`, so
  // any user-supplied extension (or none) gets normalised.  Verify
  // both branches produce the canonical `.json` file on disk.
  Planning planning {.system = {.name = "EXT_NORM"}};
  const auto dir = unique_tmpdir("ext_norm");

  SUBCASE("no extension")
  {
    const auto out_path = (dir / "no_ext").string();
    REQUIRE(write_json_output(planning, out_path).has_value());
    CHECK(std::filesystem::exists(dir / "no_ext.json"));
  }

  SUBCASE("non-canonical extension")
  {
    const auto out_path = (dir / "weird.txt").string();
    REQUIRE(write_json_output(planning, out_path).has_value());
    CHECK(std::filesystem::exists(dir / "weird.json"));
    CHECK_FALSE(std::filesystem::exists(dir / "weird.txt"));
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("write_json_output: returns error when target dir is missing")
// NOLINT
{
  // Trying to write into a nonexistent directory hits the
  // file-open-failure branch (`if (!jfile)`).  This exercises the
  // error path that the success-only test above can't.
  Planning planning {.system = {.name = "ERR"}};
  const auto bogus =
      std::filesystem::path {"/this/dir/does/not/exist/out.json"}.string();

  const auto result = write_json_output(planning, bogus);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().contains("Failed to create JSON output file"));
}

// ─── load_user_constraints ─────────────────────────────────────────────────

TEST_CASE("load_user_constraints: empty when no UC file configured")  // NOLINT
{
  // No `user_constraint_file` and no `user_constraint_files` — the
  // loader is a no-op and must succeed without touching the
  // user-constraint array.
  Planning planning {};
  REQUIRE(planning.system.user_constraint_array.empty());

  const auto result = load_user_constraints(planning);
  REQUIRE(result.has_value());
  CHECK(planning.system.user_constraint_array.empty());
}

TEST_CASE("load_user_constraints: JSON array file appends constraints")
// NOLINT
{
  // Create a JSON array of UserConstraint objects, point Planning at
  // it, run the loader, and verify the constraints land in the
  // planning's array.  This covers the JSON branch (ext != ".pampl").
  const auto dir = unique_tmpdir("uc_json");
  const auto uc_path = (dir / "uc.json").string();
  {
    std::ofstream ofs(uc_path);
    ofs << R"([
      {"uid": 10, "name": "ucA", "expression": "0 <= 100"},
      {"uid": 11, "name": "ucB", "expression": "0 <= 200"}
    ])";
  }

  Planning planning {};
  planning.system.user_constraint_file = uc_path;
  const auto result = load_user_constraints(planning);
  REQUIRE(result.has_value());
  CHECK(planning.system.user_constraint_array.size() == 2);

  std::filesystem::remove_all(dir);
}

TEST_CASE("load_user_constraints: missing file returns error")  // NOLINT
{
  Planning planning {};
  planning.system.user_constraint_file = "/no/such/uc/path.json";

  const auto result = load_user_constraints(planning);
  REQUIRE_FALSE(result.has_value());
  // Implementation reports either "Cannot read" (regular JSON path) or
  // "Error loading" (catch handler).  Accept either prefix to match
  // the actual error string daw::read_file produces on missing file.
  CHECK((result.error().contains("user_constraint_file")
         || result.error().contains("Cannot read")));
}

TEST_CASE("load_user_constraints: malformed JSON returns error")  // NOLINT
{
  // Garbage JSON triggers the from_json exception, caught by the
  // outer try/catch and returned as an error string.
  const auto dir = unique_tmpdir("uc_bad_json");
  const auto uc_path = (dir / "bad.json").string();
  {
    std::ofstream ofs(uc_path);
    ofs << "{ this is not valid JSON }";
  }

  Planning planning {};
  planning.system.user_constraint_file = uc_path;
  const auto result = load_user_constraints(planning);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().contains("user_constraint_file"));

  std::filesystem::remove_all(dir);
}

TEST_CASE(
    "load_user_constraints: multiple files via "
    "user_constraint_files")  // NOLINT
{
  // The loader iterates BOTH `user_constraint_file` (singular) AND
  // `user_constraint_files` (vector).  Verify both arrays are walked
  // and constraints from each are merged into the planning.
  const auto dir = unique_tmpdir("uc_multi");
  const auto uc_a = (dir / "a.json").string();
  const auto uc_b = (dir / "b.json").string();
  {
    std::ofstream ofs(uc_a);
    ofs << R"([{"uid": 1, "name": "ucA", "expression": "0 <= 1"}])";
  }
  {
    std::ofstream ofs(uc_b);
    ofs << R"([{"uid": 2, "name": "ucB", "expression": "0 <= 2"}])";
  }

  Planning planning {};
  planning.system.user_constraint_file = uc_a;
  planning.system.user_constraint_files = {uc_b};

  const auto result = load_user_constraints(planning);
  REQUIRE(result.has_value());
  CHECK(planning.system.user_constraint_array.size() == 2);

  std::filesystem::remove_all(dir);
}

// ─── parse_planning_json / parse_planning_files extra coverage ─────────────

TEST_CASE("parse_planning_json: direct string_view entry point")  // NOLINT
{
  // `parse_planning_json` is the single instantiation point for
  // `daw::json::from_json<Planning>` with StrictParsePolicy.  All the
  // file-based call sites delegate through `parse_planning_files`, so
  // this is the only direct coverage of the string-view overload.
  constexpr std::string_view json = R"(
    {
      "options": {
        "model_options": {
          "demand_fail_cost": 1234.0
        }
      },
      "simulation": {
        "block_array": [
          {
            "uid": 1,
            "duration": 1.0
          }
        ],
        "stage_array": [
          {
            "uid": 1,
            "first_block": 0,
            "count_block": 1
          }
        ],
        "scenario_array": [
          {
            "uid": 0
          }
        ]
      },
      "system": {
        "name": "FROM_STRING",
        "bus_array": [
          {
            "uid": 1,
            "name": "b1"
          }
        ]
      }
    }
  )";

  const Planning planning = parse_planning_json(json);
  CHECK(planning.system.name == "FROM_STRING");
  REQUIRE(planning.system.bus_array.size() == 1);
  CHECK(planning.system.bus_array.front().name == "b1");
  CHECK(planning.options.model_options.demand_fail_cost.value_or(0.0)
        == doctest::Approx(1234.0));
}

TEST_CASE("parse_planning_files: empty file list returns default Planning")
// NOLINT
{
  // The loop body never executes, so a default-constructed Planning is
  // returned.  This exercises the "no inputs" branch which the
  // end-to-end driver short-circuits.
  const auto result = parse_planning_files({});
  REQUIRE(result.has_value());
  CHECK(result->system.name.empty());
  CHECK(result->system.bus_array.empty());
}

TEST_CASE("parse_planning_files: missing file returns error")  // NOLINT
{
  // No fallback `input_directory` provided, so the "does not exist"
  // branch fires immediately.
  const auto result =
      parse_planning_files({"/nonexistent/dir/no_such_file.json"});
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().contains("does not exist"));
}

TEST_CASE("parse_planning_files: input_directory fallback resolves file")
// NOLINT
{
  // Place the file in a directory that's NOT the current working
  // directory; pass only the file's basename and rely on the
  // input_directory fallback to find it.
  const auto dir = unique_tmpdir("parse_indir");
  const auto basename = std::string {"resolved.json"};
  const auto full = (dir / basename).string();
  {
    std::ofstream ofs(full);
    ofs << R"({"system": {"name": "RESOLVED"}})";
  }

  // Pass `basename` (does NOT exist in cwd) plus the directory as
  // `input_directory` — the loader must locate the file via fallback.
  const auto result = parse_planning_files({basename}, dir.string());
  REQUIRE(result.has_value());
  CHECK(result->system.name == "RESOLVED");

  std::filesystem::remove_all(dir);
}

TEST_CASE(
    "parse_planning_files: missing file with input_directory fallback also "
    "missing")  // NOLINT
{
  // Both the primary path and the input_directory fallback miss the
  // file — the loader must return an error.
  const auto dir = unique_tmpdir("parse_indir_miss");
  const auto result =
      parse_planning_files({"/no/such/primary.json"}, dir.string());
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().contains("does not exist"));

  std::filesystem::remove_all(dir);
}

TEST_CASE("parse_planning_files: extension is normalised to .json")  // NOLINT
{
  // The implementation calls `replace_extension(".json")` on the
  // user-supplied path before checking existence.  Supplying a
  // user-extension-less path should still locate `<stem>.json`.
  const auto dir = unique_tmpdir("parse_ext");
  const auto json_path = (dir / "norm.json").string();
  {
    std::ofstream ofs(json_path);
    ofs << R"({"system": {"name": "EXT_OK"}})";
  }

  // Pass the stem WITHOUT the .json extension.
  const auto stem = (dir / "norm").string();
  const auto result = parse_planning_files({stem});
  REQUIRE(result.has_value());
  CHECK(result->system.name == "EXT_OK");

  std::filesystem::remove_all(dir);
}

TEST_CASE("parse_planning_files: malformed JSON returns error")  // NOLINT
{
  // The DAW JSON parser throws on malformed input; the loader catches
  // and converts it to an `unexpected` with the filename embedded.
  const auto dir = unique_tmpdir("parse_bad");
  const auto bad_path = (dir / "bad.json").string();
  {
    std::ofstream ofs(bad_path);
    ofs << "{ this is not valid JSON }";
  }

  const auto result = parse_planning_files({bad_path});
  REQUIRE_FALSE(result.has_value());
  // Error message mentions the offending file path.
  CHECK(result.error().contains("bad.json"));

  std::filesystem::remove_all(dir);
}

TEST_CASE("parse_planning_files: multiple files are merged")  // NOLINT
{
  // Two files contribute different parts of the same Planning.  The
  // loader calls `Planning::merge` once per file; the final result
  // should contain entries from both.
  const auto dir = unique_tmpdir("parse_merge");
  const auto p1 = (dir / "p1.json").string();
  const auto p2 = (dir / "p2.json").string();
  {
    std::ofstream ofs(p1);
    ofs << R"({"system": {"name": "MERGED",
                          "bus_array": [{"uid": 1, "name": "b1"}]}})";
  }
  {
    std::ofstream ofs(p2);
    ofs << R"({"system": {"bus_array": [{"uid": 2, "name": "b2"}]}})";
  }

  const auto result = parse_planning_files({p1, p2});
  REQUIRE(result.has_value());
  // Both buses should be present after the merge.
  CHECK(result->system.bus_array.size() == 2);

  std::filesystem::remove_all(dir);
}

// ─── load_user_constraints extra coverage ──────────────────────────────────

TEST_CASE("load_user_constraints: PAMPL extension dispatches to PamplParser")
// NOLINT
{
  // A `.pampl` extension routes through `PamplParser::parse_file`
  // instead of the JSON path.  Verify the constraints land in the
  // planning's array with names taken from the PAMPL `constraint NAME`
  // headers.
  const auto dir = unique_tmpdir("uc_pampl");
  const auto pampl_path = (dir / "rules.pampl").string();
  {
    std::ofstream ofs(pampl_path);
    ofs << "constraint pampl_a: generator('G1').generation <= 100;\n"
        << "constraint pampl_b: generator('G2').generation <= 200;\n";
  }

  Planning planning {};
  planning.system.user_constraint_file = pampl_path;
  const auto result = load_user_constraints(planning);
  REQUIRE(result.has_value());
  REQUIRE(planning.system.user_constraint_array.size() == 2);
  // Names come from the PAMPL `constraint <name>:` headers.
  CHECK(planning.system.user_constraint_array[0].name == "pampl_a");
  CHECK(planning.system.user_constraint_array[1].name == "pampl_b");

  std::filesystem::remove_all(dir);
}

TEST_CASE(
    "load_user_constraints: relative path resolves against "
    "input_directory")  // NOLINT
{
  // The loader resolves relative `user_constraint_file` paths against
  // `planning.options.input_directory`.  Drop a UC file in a subdir,
  // refer to it by basename, and rely on this resolution.
  const auto dir = unique_tmpdir("uc_relpath");
  const auto uc_path = (dir / "uc.json").string();
  {
    std::ofstream ofs(uc_path);
    ofs << R"([{"uid": 7, "name": "rel_uc", "expression": "0 <= 1"}])";
  }

  Planning planning {};
  planning.options.input_directory = dir.string();
  // Reference by basename only — relative path resolution kicks in.
  planning.system.user_constraint_file = std::string {"uc.json"};

  const auto result = load_user_constraints(planning);
  REQUIRE(result.has_value());
  REQUIRE(planning.system.user_constraint_array.size() == 1);
  CHECK(planning.system.user_constraint_array.front().name == "rel_uc");

  std::filesystem::remove_all(dir);
}

TEST_CASE("load_user_constraints: PAMPL file missing returns error")  // NOLINT
{
  // The catch handler converts PamplParser::parse_file failures into
  // an error string carrying the file path.
  Planning planning {};
  planning.system.user_constraint_file = std::string {"/no/such/file.pampl"};

  const auto result = load_user_constraints(planning);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().contains("user_constraint_file"));
}

// NOLINTEND(readability-trailing-comma)