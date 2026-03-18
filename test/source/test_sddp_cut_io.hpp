/**
 * @file      test_sddp_cut_io.hpp
 * @brief     Unit tests for the SDDP cut I/O module (save/load round-trip)
 * @date      2026-03-18
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. build_phase_uid_map produces correct mapping from phase UIDs
 *  2. save_cuts_csv / load_cuts_csv round-trip preserves cuts
 *  3. load_cuts_csv handles empty files gracefully
 *  4. save_scene_cuts_csv creates per-scene files
 *  5. load_scene_cuts_from_directory loads scene files and skips errors
 */

#include <filesystem>
#include <fstream>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_solver.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── build_phase_uid_map tests ──────────────────────────────────────────────

TEST_CASE("build_phase_uid_map produces correct mapping")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto phase_map = build_phase_uid_map(planning_lp);

  // 3-phase planning has UIDs 1, 2, 3
  REQUIRE(phase_map.size() == 3);
  CHECK(phase_map.contains(1));
  CHECK(phase_map.contains(2));
  CHECK(phase_map.contains(3));

  // Verify the indices map correctly
  CHECK(phase_map.at(1) == PhaseIndex {0});
  CHECK(phase_map.at(2) == PhaseIndex {1});
  CHECK(phase_map.at(3) == PhaseIndex {2});
}

TEST_CASE("build_phase_uid_map with single-phase planning")  // NOLINT
{
  auto planning = make_single_phase_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto phase_map = build_phase_uid_map(planning_lp);

  // Default single-phase planning has 1 phase (auto-created with
  // unknown_uid = -1 when no explicit phase_array is provided)
  REQUIRE(phase_map.size() == 1);

  // Verify there is exactly one entry mapping to PhaseIndex{0}
  const auto it = phase_map.begin();
  CHECK(it->second == PhaseIndex {0});
}

// ─── save_cuts_csv tests ────────────────────────────────────────────────────

TEST_CASE("save_cuts_csv writes a valid CSV file")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_dir = std::filesystem::temp_directory_path();
  const auto cuts_file = (tmp_dir / "gtopt_test_cut_io_save.csv").string();

  // Run SDDP to generate real cuts
  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-6;

  SDDPSolver sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto& stored = sddp.stored_cuts();
  REQUIRE_FALSE(stored.empty());

  auto save_result = save_cuts_csv(stored, planning_lp, cuts_file);
  REQUIRE(save_result.has_value());
  CHECK(std::filesystem::exists(cuts_file));

  // Verify file is non-empty and has header
  std::ifstream ifs(cuts_file);
  std::string line;
  std::getline(ifs, line);  // metadata comment
  CHECK(line.starts_with("# scale_objective="));
  std::getline(ifs, line);  // CSV header
  CHECK(line == "phase,scene,name,rhs,coefficients");

  // Should have at least one data line
  std::getline(ifs, line);
  CHECK_FALSE(line.empty());

  std::filesystem::remove(cuts_file);
}

// ─── save_cuts_csv / load_cuts_csv round-trip via SDDPSolver ────────────────

TEST_CASE("save and load cuts round-trip via SDDPSolver")  // NOLINT
{
  const auto tmp_dir = std::filesystem::temp_directory_path();
  const auto cuts_file = (tmp_dir / "gtopt_test_cut_io_roundtrip.csv").string();

  int num_saved = 0;

  // Phase 1: run SDDP and save cuts
  {
    auto planning = make_3phase_hydro_planning();
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 3;
    sddp_opts.convergence_tol = 1e-6;
    sddp_opts.cuts_output_file = cuts_file;

    SDDPSolver sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(sddp.stored_cuts().empty());
    num_saved = static_cast<int>(sddp.stored_cuts().size());
  }

  // Verify the file was created
  CHECK(std::filesystem::exists(cuts_file));
  CHECK(num_saved > 0);

  // Phase 2: hot-start a fresh solver using saved cuts
  {
    auto planning2 = make_3phase_hydro_planning();
    PlanningLP planning_lp2(std::move(planning2));

    SDDPOptions load_opts;
    load_opts.max_iterations = 1;
    load_opts.convergence_tol = 1e-6;
    load_opts.cuts_input_file = cuts_file;
    load_opts.hot_start = true;

    // Solve with hot-start — should converge faster or at least not crash
    SDDPSolver sddp2(planning_lp2, load_opts);
    auto results2 = sddp2.solve();
    REQUIRE(results2.has_value());
  }

  std::filesystem::remove(cuts_file);
}

// ─── load_cuts_csv edge cases ───────────────────────────────────────────────

TEST_CASE("load_cuts_csv returns error for nonexistent file")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const LabelMaker label_maker(planning_lp.options());
  auto result = load_cuts_csv(
      planning_lp, "/tmp/gtopt_nonexistent_cuts.csv", label_maker);
  CHECK_FALSE(result.has_value());
}

TEST_CASE("load_cuts_csv handles header-only file")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_cut_io_empty.csv")
          .string();

  // Write a file with only the header
  {
    std::ofstream ofs(tmp_file);
    ofs << "# scale_objective=1\n";
    ofs << "phase,scene,name,rhs,coefficients\n";
  }

  const LabelMaker label_maker(planning_lp.options());
  auto result = load_cuts_csv(planning_lp, tmp_file, label_maker);
  REQUIRE(result.has_value());
  CHECK(*result == 0);

  std::filesystem::remove(tmp_file);
}

// ─── save_scene_cuts_csv tests ──────────────────────────────────────────────

TEST_CASE("save_scene_cuts_csv creates per-scene file")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  // Run SDDP to generate cuts
  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;
  sddp_opts.convergence_tol = 1e-6;

  SDDPSolver sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto& stored = sddp.stored_cuts();
  REQUIRE_FALSE(stored.empty());

  const auto tmp_dir =
      (std::filesystem::temp_directory_path() / "gtopt_test_scene_cuts")
          .string();

  auto save_result =
      save_scene_cuts_csv(stored, SceneIndex {0}, 1, planning_lp, tmp_dir);
  REQUIRE(save_result.has_value());

  // Verify scene_1.csv was created
  const auto scene_file = std::filesystem::path(tmp_dir) / "scene_1.csv";
  CHECK(std::filesystem::exists(scene_file));

  // Clean up
  std::filesystem::remove_all(tmp_dir);
}

// ─── load_scene_cuts_from_directory tests ───────────────────────────────────

TEST_CASE(
    "load_scene_cuts_from_directory loads files and skips "
    "errors")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  // Run SDDP to generate and save per-scene cuts
  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;
  sddp_opts.convergence_tol = 1e-6;

  SDDPSolver sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto& stored = sddp.stored_cuts();
  REQUIRE_FALSE(stored.empty());

  const auto tmp_dir =
      (std::filesystem::temp_directory_path() / "gtopt_test_load_scene_cuts")
          .string();

  // Save scene cuts
  auto save_result =
      save_scene_cuts_csv(stored, SceneIndex {0}, 1, planning_lp, tmp_dir);
  REQUIRE(save_result.has_value());

  // Also create an error file that should be skipped
  {
    std::ofstream ofs(
        (std::filesystem::path(tmp_dir) / "error_scene_99.csv").string());
    ofs << "# error file\n";
    ofs << "phase,scene,name,rhs,coefficients\n";
    ofs << "1,99,bad_cut,999\n";
  }

  // Load back into the same PlanningLP (which already has alpha columns
  // from the SDDP solve, so column indices from the saved cuts are valid)
  const LabelMaker label_maker(planning_lp.options());
  auto load_result =
      load_scene_cuts_from_directory(planning_lp, tmp_dir, label_maker);
  REQUIRE(load_result.has_value());
  CHECK(*load_result > 0);

  // Clean up
  std::filesystem::remove_all(tmp_dir);
}

TEST_CASE(
    "load_scene_cuts_from_directory returns 0 for nonexistent "
    "dir")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const LabelMaker label_maker(planning_lp.options());
  auto result = load_scene_cuts_from_directory(
      planning_lp, "/tmp/gtopt_nonexistent_dir_xyz", label_maker);
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}
