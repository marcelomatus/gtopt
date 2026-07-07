// SPDX-License-Identifier: BSD-3-Clause
//
// Side-effect tests for `save_cuts_for_iteration`.  Pins two contracts:
//
//   1. **No state CSV is written, of either flavour.**  Pre-2026-05-14
//      `save_cuts_for_iteration` wrote `sddp_state.csv` (latest) every
//      iter and historically also `sddp_state_<iter>.csv` (versioned).
//      The versioned file was retired first; the latest-only file was
//      removed when an audit found zero readers anywhere — policy state
//      is reconstructed from the versioned cut files at recovery.  This
//      test pins the **deletion** of both, so a regression that re-
//      introduces either would trip CI immediately.
//
//   2. The per-scene cut writes work correctly under both the parallel
//      (pool != nullptr, num_scenes > 1) and sequential (pool == nullptr
//      OR num_scenes == 1) branches of `SDDPCutManager::save_cuts_for_
//      iteration`.  The parallel branch is exercised by the 2-scene
//      SDDP fixture; the sequential branch by the 1-scene fixture.

#include <filesystem>
#include <regex>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_types.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;

namespace
{

/// Resolve to a unique tmp directory and clean it on entry.  Each test
/// gets its own subdir so concurrent ctest runs (`ctest -j20`) cannot
/// race on shared filenames.
[[nodiscard]] std::filesystem::path make_tmp_subdir(std::string_view stem)
{
  const auto path = std::filesystem::temp_directory_path()
      / std::format("gtopt_test_save_cuts_{}", stem);
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  std::filesystem::create_directories(path, ec);
  return path;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 1. Versioned state file `sddp_state_<iter>.csv` is NOT created
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDPCutManager::save_cuts_for_iteration does NOT write any "
    "sddp_state CSV (latest or versioned)")
{
  // Pre-2026-05-14: both the versioned `sddp_state_<iter>.csv` AND
  // the latest-only `sddp_state.csv` were written every iter.  Both
  // were retired (no readers).  This test pins both deletions: walk
  // the cut directory after a 2-iter solve and assert no file
  // matches `sddp_state*.csv`.
  // The versioned cuts file (`sddp_cuts_<iter>.parquet` since C-2) is
  // unaffected and SHOULD be present.
  const auto tmp_dir = make_tmp_subdir("no_versioned_state");
  const auto cuts_file = (tmp_dir / "sddp_cuts.parquet").string();

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;
  sddp_opts.min_iterations = 2;  // force two iters
  sddp_opts.convergence_tol = 1e-12;  // unreachable, force max_iterations
  sddp_opts.cuts_output_file = cuts_file;
  sddp_opts.save_per_iteration = true;
  sddp_opts.cut_sharing = CutSharingMode::broadcast_mean;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE(results->size() >= 1);

  // Latest state file MUST NOT exist (writer removed 2026-05-14).
  CHECK_FALSE(std::filesystem::exists(tmp_dir / "sddp_state.csv"));

  // Versioned state files MUST NOT exist.
  std::vector<std::string> versioned_state_files;
  const std::regex versioned_state_re {R"(^sddp_state_\d+\.csv$)"};
  for (const auto& entry : std::filesystem::directory_iterator(tmp_dir)) {
    const auto name = entry.path().filename().string();
    if (std::regex_match(name, versioned_state_re)) {
      versioned_state_files.push_back(name);
    }
  }
  INFO("found versioned state file(s): "
       << (versioned_state_files.empty() ? std::string {"none"}
                                         : versioned_state_files.front()));
  CHECK(versioned_state_files.empty());

  // Versioned CUT files (the other family that DOES still get written)
  // must exist — this confirms our regex isn't accidentally matching
  // the wrong files.  Cuts moved from CSV to Parquet in C-2.
  CHECK(std::filesystem::exists(tmp_dir / "sddp_cuts_0.parquet"));
  CHECK(std::filesystem::exists(tmp_dir / "sddp_cuts_1.parquet"));
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. Sequential per-scene cut write fallback (num_scenes == 1)
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDPCutManager::save_cuts_for_iteration sequential fallback "
    "(num_scenes==1) produces all expected files")
{
  // The 1-scene fixture exercises the sequential branch in
  // `save_cuts_for_iteration` (pool != nullptr but num_scenes == 1
  // takes the else branch — same code path as pool == nullptr).
  // Any divergence between parallel and sequential would surface as
  // a missing file or empty content here.
  const auto tmp_dir = make_tmp_subdir("seq_fallback_1scene");
  const auto cuts_file = (tmp_dir / "sddp_cuts.parquet").string();

  auto planning = make_3phase_hydro_planning();  // 1 scene
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.convergence_tol = 1e-6;
  sddp_opts.cuts_output_file = cuts_file;
  sddp_opts.save_per_iteration = true;
  sddp_opts.cut_sharing = CutSharingMode::none;  // 1 scene → no sharing
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  // Combined cuts file (latest).  Cuts moved from CSV to Parquet in C-2.
  CHECK(std::filesystem::exists(tmp_dir / "sddp_cuts.parquet"));

  // Versioned cuts file for iter 0.
  CHECK(std::filesystem::exists(tmp_dir / "sddp_cuts_0.parquet"));

  // At least one per-scene cuts file (UID-named, fixture-dependent).
  // Globbed instead of hardcoded — `make_3phase_hydro_planning` does
  // not declare an explicit `scene_array` so the auto-generated UID
  // may not be 1.
  std::vector<std::string> scene_files;
  const std::regex scene_re {R"(^scene_\d+\.parquet$)"};
  for (const auto& entry : std::filesystem::directory_iterator(tmp_dir)) {
    const auto name = entry.path().filename().string();
    if (std::regex_match(name, scene_re)) {
      scene_files.push_back(name);
    }
  }
  CHECK(scene_files.size() == 1U);

  // Latest state file MUST NOT exist (writer removed 2026-05-14).
  CHECK_FALSE(std::filesystem::exists(tmp_dir / "sddp_state.csv"));

  // Versioned state file MUST NOT exist (retired earlier).
  CHECK_FALSE(std::filesystem::exists(tmp_dir / "sddp_state_0.csv"));
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. Parallel per-scene cut writes (num_scenes > 1) match output structure
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDPCutManager::save_cuts_for_iteration parallel branch "
    "(num_scenes>1) produces all expected files")
{
  // Counterpart to test #2: 2-scene fixture exercises the parallel
  // branch (pool != nullptr AND num_scenes > 1).  Same expected file
  // set, larger per-scene fan-out.
  const auto tmp_dir = make_tmp_subdir("parallel_2scene");
  const auto cuts_file = (tmp_dir / "sddp_cuts.parquet").string();

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.convergence_tol = 1e-6;
  sddp_opts.cuts_output_file = cuts_file;
  sddp_opts.save_per_iteration = true;
  sddp_opts.cut_sharing = CutSharingMode::broadcast_mean;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  CHECK(std::filesystem::exists(tmp_dir / "sddp_cuts.parquet"));
  CHECK(std::filesystem::exists(tmp_dir / "sddp_cuts_0.parquet"));
  // Latest state file MUST NOT exist (writer removed 2026-05-14).
  CHECK_FALSE(std::filesystem::exists(tmp_dir / "sddp_state.csv"));

  // Both per-scene files (UID-named, globbed for fixture independence).
  std::vector<std::string> scene_files;
  const std::regex scene_re {R"(^scene_\d+\.parquet$)"};
  for (const auto& entry : std::filesystem::directory_iterator(tmp_dir)) {
    const auto name = entry.path().filename().string();
    if (std::regex_match(name, scene_re)) {
      scene_files.push_back(name);
    }
  }
  CHECK(scene_files.size() == 2U);

  CHECK_FALSE(std::filesystem::exists(tmp_dir / "sddp_state_0.csv"));
}
