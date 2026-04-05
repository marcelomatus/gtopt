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
 *  6. save_cuts_csv with empty cuts vector
 *  7. save_cuts_csv with unknown phase UID (unscaled fallback)
 *  8. load_cuts_csv with comment lines and blank lines
 *  9. load_cuts_csv with unknown phase UID skips cut
 * 10. load_cuts_csv with coefficient parsing
 * 11. save_scene_cuts_csv error on invalid path
 * 12. load_scene_cuts_from_directory skips non-scene/non-csv files
 * 13. load_boundary_cuts_csv noload mode returns 0
 * 14. load_boundary_cuts_csv error on nonexistent file
 * 15. load_boundary_cuts_csv invalid header
 * 16. load_boundary_cuts_csv separated mode
 * 17. load_boundary_cuts_csv legacy format (no iteration column)
 * 18. load_boundary_cuts_csv iteration filtering
 * 19. load_named_cuts_csv error on nonexistent file
 * 20. load_named_cuts_csv invalid header (too few columns)
 * 21. load_named_cuts_csv wrong phase column name
 * 22. load_named_cuts_csv loads cuts for all phases
 * 23. load_named_cuts_csv skips unknown phase UIDs
 */

#include <filesystem>
#include <fstream>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_method.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Helper to create scene_phase_states matching a PlanningLP.
auto make_scene_phase_states(const PlanningLP& planning_lp)
    -> StrongIndexVector<SceneIndex,
                         StrongIndexVector<PhaseIndex, PhaseStateInfo>>
{
  const auto& sim = planning_lp.simulation();
  const auto num_scenes = static_cast<Index>(sim.scenes().size());
  const auto num_phases = static_cast<Index>(sim.phases().size());

  StrongIndexVector<SceneIndex, StrongIndexVector<PhaseIndex, PhaseStateInfo>>
      result(num_scenes,
             StrongIndexVector<PhaseIndex, PhaseStateInfo>(num_phases,
                                                           PhaseStateInfo {}));
  return result;
}

/// Unique temp dir for a test to avoid collisions.
auto make_test_dir(const std::string& test_name) -> std::filesystem::path
{
  auto dir = std::filesystem::temp_directory_path()
      / ("gtopt_test_cut_io_" + test_name);
  std::filesystem::create_directories(dir);
  return dir;
}

}  // namespace

// ─── build_phase_uid_map tests ──────────────────────────────────────────────

TEST_CASE("build_phase_uid_map produces correct mapping")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  const PlanningLP planning_lp(std::move(planning));

  const auto phase_map = build_phase_uid_map(planning_lp);

  // 3-phase planning has UIDs 1, 2, 3
  REQUIRE(phase_map.size() == 3);
  CHECK(phase_map.contains(PhaseUid {1}));
  CHECK(phase_map.contains(PhaseUid {2}));
  CHECK(phase_map.contains(PhaseUid {3}));

  // Verify the indices map correctly
  CHECK(phase_map.at(PhaseUid {1}) == PhaseIndex {0});
  CHECK(phase_map.at(PhaseUid {2}) == PhaseIndex {1});
  CHECK(phase_map.at(PhaseUid {3}) == PhaseIndex {2});
}

TEST_CASE("build_phase_uid_map with single-phase planning")  // NOLINT
{
  auto planning = make_single_phase_planning();
  const PlanningLP planning_lp(std::move(planning));

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

  SDDPMethod sddp(planning_lp, sddp_opts);
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
  CHECK(line == "type,phase,scene,name,rhs,dual,coefficients");

  // Should have at least one data line
  std::getline(ifs, line);
  CHECK_FALSE(line.empty());

  std::filesystem::remove(cuts_file);
}

TEST_CASE("save_cuts_csv with empty cuts vector writes header only")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  const PlanningLP planning_lp(std::move(planning));

  const auto tmp_dir = std::filesystem::temp_directory_path();
  const auto cuts_file =
      (tmp_dir / "gtopt_test_cut_io_empty_save.csv").string();

  // Save an empty span of cuts
  std::vector<StoredCut> empty_cuts;
  auto save_result = save_cuts_csv(
      std::span<const StoredCut>(empty_cuts), planning_lp, cuts_file);
  REQUIRE(save_result.has_value());
  CHECK(std::filesystem::exists(cuts_file));

  // File should contain metadata + header but no data lines
  std::ifstream ifs(cuts_file);
  std::string line;
  std::getline(ifs, line);  // metadata
  CHECK(line.starts_with("# scale_objective="));
  std::getline(ifs, line);  // header
  CHECK(line == "type,phase,scene,name,rhs,dual,coefficients");

  // No more data lines
  CHECK_FALSE(std::getline(ifs, line));

  std::filesystem::remove(cuts_file);
}

TEST_CASE(
    "save_cuts_csv with unknown phase UID uses unscaled "
    "fallback")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  const PlanningLP planning_lp(std::move(planning));

  const auto tmp_dir = std::filesystem::temp_directory_path();
  const auto cuts_file =
      (tmp_dir / "gtopt_test_cut_io_unknown_phase.csv").string();

  // Create a StoredCut with a phase UID that does not exist
  std::vector<StoredCut> cuts = {
      StoredCut {
          .phase = PhaseUid {999},
          .scene = SceneUid {1},
          .name = "bad_phase_cut",
          .rhs = 42.0,
          .coefficients =
              {
                  {ColIndex {0}, 1.5},
                  {ColIndex {1}, -2.5},
              },
      },
  };

  auto save_result =
      save_cuts_csv(std::span<const StoredCut>(cuts), planning_lp, cuts_file);
  REQUIRE(save_result.has_value());

  // Verify the file was written with the unscaled coefficients
  std::ifstream ifs(cuts_file);
  std::string line;
  std::getline(ifs, line);  // metadata
  std::getline(ifs, line);  // header
  std::getline(ifs, line);  // data line

  // The line should contain "o,999,1,bad_phase_cut" and coefficient data
  CHECK(line.starts_with("o,999,1,bad_phase_cut,"));

  std::filesystem::remove(cuts_file);
}

TEST_CASE("save_cuts_csv creates parent directories")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  const PlanningLP planning_lp(std::move(planning));

  const auto nested_dir = std::filesystem::temp_directory_path()
      / "gtopt_test_nested" / "sub1" / "sub2";
  const auto cuts_file = (nested_dir / "cuts.csv").string();

  std::vector<StoredCut> cuts;
  auto save_result =
      save_cuts_csv(std::span<const StoredCut>(cuts), planning_lp, cuts_file);
  REQUIRE(save_result.has_value());
  CHECK(std::filesystem::exists(cuts_file));

  std::filesystem::remove_all(std::filesystem::temp_directory_path()
                              / "gtopt_test_nested");
}

// ─── save_cuts_csv / load_cuts_csv round-trip via SDDPMethod ────────────────

TEST_CASE("save and load cuts round-trip via SDDPMethod")  // NOLINT
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

    SDDPMethod sddp(planning_lp, sddp_opts);
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
    load_opts.cut_recovery_mode = HotStartMode::replace;

    // Solve with hot-start — should converge faster or at least not crash
    SDDPMethod sddp2(planning_lp2, load_opts);
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
  CHECK(result.error().code == ErrorCode::FileIOError);
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
  CHECK(result->count == 0);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_cuts_csv skips comment lines and blank lines in "
    "body")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_cut_io_comments.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    ofs << "# scale_objective=1\n";
    ofs << "phase,scene,name,rhs,coefficients\n";
    ofs << "# This is a comment in the body\n";
    ofs << "\n";
    ofs << "# Another comment\n";
  }

  const LabelMaker label_maker(planning_lp.options());
  auto result = load_cuts_csv(planning_lp, tmp_file, label_maker);
  REQUIRE(result.has_value());
  CHECK(result->count == 0);

  std::filesystem::remove(tmp_file);
}

TEST_CASE("load_cuts_csv skips cuts with unknown phase UID")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_cut_io_unknown_phase_load.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    ofs << "# scale_objective=1\n";
    ofs << "phase,scene,name,rhs,coefficients\n";
    // Phase UID 999 does not exist
    ofs << "999,1,unknown_phase_cut,100.0\n";
  }

  const LabelMaker label_maker(planning_lp.options());
  auto result = load_cuts_csv(planning_lp, tmp_file, label_maker);
  REQUIRE(result.has_value());
  // The cut should be skipped (unknown phase), so 0 loaded
  CHECK(result->count == 0);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_cuts_csv parses coefficients and loads valid "
    "cuts")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_cut_io_valid_load.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    ofs << "# scale_objective=1\n";
    ofs << "phase,scene,name,rhs,coefficients\n";
    // Phase UID 1 exists in 3-phase planning, col 0 with coeff 1.5
    ofs << "1,1,my_cut,50.0,0:1.5,1:-2.0\n";
    ofs << "2,1,my_cut2,30.0,0:0.5\n";
  }

  const LabelMaker label_maker(planning_lp.options());
  auto result = load_cuts_csv(planning_lp, tmp_file, label_maker);
  REQUIRE(result.has_value());
  CHECK(result->count == 2);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_cuts_csv skips malformed lines and loads valid "
    "ones")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_cut_io_malformed.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    ofs << "# scale_objective=1\n";
    ofs << "phase,scene,name,rhs,coefficients\n";
    // Valid line
    ofs << "1,1,good_cut,50.0,0:1.5\n";
    // Malformed: non-numeric phase
    ofs << "abc,1,bad_phase,10.0\n";
    // Malformed: missing rhs
    ofs << "1,1,no_rhs\n";
    // Malformed: non-numeric rhs
    ofs << "1,1,bad_rhs,xyz\n";
    // Another valid line
    ofs << "2,1,good_cut2,30.0,0:0.5\n";
  }

  const LabelMaker label_maker(planning_lp.options());
  auto result = load_cuts_csv(planning_lp, tmp_file, label_maker);
  REQUIRE(result.has_value());
  // Only the 2 valid lines should be loaded
  CHECK(result->count == 2);

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

  SDDPMethod sddp(planning_lp, sddp_opts);
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

  // Verify file has correct header
  std::ifstream ifs(scene_file.string());
  std::string line;
  std::getline(ifs, line);
  CHECK(line.starts_with("# scale_objective="));
  std::getline(ifs, line);
  CHECK(line == "type,phase,scene,name,rhs,dual,coefficients");

  // Clean up
  std::filesystem::remove_all(tmp_dir);
}

TEST_CASE(
    "save_scene_cuts_csv with empty cuts creates empty "
    "file")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  const PlanningLP planning_lp(std::move(planning));

  const auto tmp_dir =
      (std::filesystem::temp_directory_path() / "gtopt_test_scene_cuts_empty")
          .string();

  std::vector<StoredCut> empty_cuts;
  auto save_result = save_scene_cuts_csv(std::span<const StoredCut>(empty_cuts),
                                         SceneIndex {0},
                                         1,
                                         planning_lp,
                                         tmp_dir);
  REQUIRE(save_result.has_value());

  const auto scene_file = std::filesystem::path(tmp_dir) / "scene_1.csv";
  CHECK(std::filesystem::exists(scene_file));

  std::filesystem::remove_all(tmp_dir);
}

// ─── load_scene_cuts_from_directory tests ───────────────────────────────────

TEST_CASE(
    "load_scene_cuts_from_directory loads files and skips "
    "errors")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  // Enable LP names at level 1 so SDDP cuts get named (required for CSV
  // round-trip: the loader rejects rows with empty name columns).
  planning.options.lp_matrix_options.names_level = LpNamesLevel::only_cols;
  PlanningLP planning_lp(std::move(planning));

  // Run SDDP to generate and save per-scene cuts
  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;
  sddp_opts.convergence_tol = 1e-6;

  SDDPMethod sddp(planning_lp, sddp_opts);
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
  CHECK(load_result->count > 0);

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
  CHECK(result->count == 0);
}

TEST_CASE(
    "load_scene_cuts_from_directory skips non-scene "
    "and non-csv files")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_dir = make_test_dir("skip_non_scene");

  // Create files that should be skipped
  {
    std::ofstream ofs1((tmp_dir / "random_file.csv").string());
    ofs1 << "phase,scene,name,rhs,coefficients\n";
    ofs1 << "1,1,cut1,10.0\n";
  }
  {
    std::ofstream ofs2((tmp_dir / "scene_1.txt").string());
    ofs2 << "not a csv\n";
  }
  {
    // Create a subdirectory that should be skipped
    std::filesystem::create_directories(tmp_dir / "scene_subdir");
  }

  const LabelMaker label_maker(planning_lp.options());
  auto result = load_scene_cuts_from_directory(
      planning_lp, tmp_dir.string(), label_maker);
  REQUIRE(result.has_value());
  // random_file.csv does not start with "scene_" and is not sddp_cuts.csv
  // scene_1.txt does not end with .csv
  CHECK(result->count == 0);

  std::filesystem::remove_all(tmp_dir);
}

TEST_CASE(
    "load_scene_cuts_from_directory loads sddp_cuts.csv "
    "combined file")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_dir = make_test_dir("combined_cuts");

  // Create a combined sddp_cuts.csv file
  {
    std::ofstream ofs((tmp_dir / sddp_file::combined_cuts).string());
    ofs << "# scale_objective=1\n";
    ofs << "phase,scene,name,rhs,coefficients\n";
    ofs << "1,1,combined_cut,25.0,0:1.0\n";
  }

  const LabelMaker label_maker(planning_lp.options());
  auto result = load_scene_cuts_from_directory(
      planning_lp, tmp_dir.string(), label_maker);
  REQUIRE(result.has_value());
  CHECK(result->count == 1);

  std::filesystem::remove_all(tmp_dir);
}

// ─── load_boundary_cuts_csv tests ───────────────────────────────────────────

TEST_CASE("load_boundary_cuts_csv noload mode returns 0")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::noload;

  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result = load_boundary_cuts_csv(
      planning_lp, "/tmp/any_file.csv", opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 0);
}

TEST_CASE(
    "load_boundary_cuts_csv returns error for nonexistent "
    "file")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;

  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result = load_boundary_cuts_csv(planning_lp,
                                       "/tmp/gtopt_nonexistent_boundary.csv",
                                       opts,
                                       label_maker,
                                       states);
  CHECK_FALSE(result.has_value());
  CHECK(result.error().code == ErrorCode::FileIOError);
}

TEST_CASE(
    "load_boundary_cuts_csv rejects header with too few "
    "columns")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_bdr_bad_header.csv")
          .string();

  {
    std::ofstream ofs(tmp_file);
    // Only 2 columns: name, rhs — too few
    ofs << "name,rhs\n";
    ofs << "cut1,10.0\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;

  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  CHECK_FALSE(result.has_value());
  CHECK(result.error().code == ErrorCode::InvalidInput);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_boundary_cuts_csv separated mode loads cuts with "
    "matching scene UID")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_bdr_separated.csv")
          .string();

  // The 3-phase hydro planning has junction "j_up" (uid 1) with reservoir
  // "rsv1" (uid 1). Boundary cuts look up element names in
  // name_to_class_uid which only contains junctions and batteries.
  // The reservoir state variable key has uid=1 (same as junction uid).
  // Scene UID is 1 (the only scenario UID in 3-phase planning).
  {
    std::ofstream ofs(tmp_file);
    // Default scene UID is 0 (auto-generated when scene_array is empty)
    ofs << "name,iteration,scene,rhs,j_up\n";
    ofs << "bdr_cut1,1,0,100.0,5.0\n";
    ofs << "bdr_cut2,2,0,200.0,10.0\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;
  opts.boundary_max_iterations = 0;  // no filtering

  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 2);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_boundary_cuts_csv shared mode broadcasts to all "
    "scenes")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_bdr_shared.csv")
          .string();

  {
    std::ofstream ofs(tmp_file);
    // "shared" mode: scene UID is ignored, cut goes to all scenes
    ofs << "name,iteration,scene,rhs,j_up\n";
    ofs << "shared_cut,1,0,50.0,3.0\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::combined;

  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 1);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_boundary_cuts_csv legacy format without iteration "
    "column")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_bdr_legacy.csv")
          .string();

  {
    std::ofstream ofs(tmp_file);
    // Legacy format: no iteration column; default scene UID = 0
    ofs << "name,scene,rhs,j_up\n";
    ofs << "legacy_cut,0,75.0,4.0\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;

  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 1);

  std::filesystem::remove(tmp_file);
}

TEST_CASE("load_boundary_cuts_csv filters by max_iterations")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_bdr_filter_iters.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    ofs << "name,iteration,scene,rhs,j_up\n";
    // 3 distinct iterations: 1, 2, 3; default scene UID = 0
    ofs << "cut_iter1,1,0,10.0,1.0\n";
    ofs << "cut_iter2,2,0,20.0,2.0\n";
    ofs << "cut_iter3a,3,0,30.0,3.0\n";
    ofs << "cut_iter3b,3,0,35.0,3.5\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;
  // Keep only the last 2 iterations (iterations 2 and 3)
  opts.boundary_max_iterations = 2;

  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  // Iteration 1 filtered out, iterations 2+3 kept = 3 cuts
  CHECK(result->count == 3);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_boundary_cuts_csv skips cuts with unknown scene "
    "UID in separated mode")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_bdr_unknown_scene.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    ofs << "name,iteration,scene,rhs,j_up\n";
    // Scene UID 999 does not exist
    ofs << "bad_scene_cut,1,999,10.0,1.0\n";
    // Scene UID 0 exists (default auto-generated scene)
    ofs << "good_cut,1,0,20.0,2.0\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;

  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  // Only the good_cut should be loaded; bad_scene_cut is skipped
  CHECK(result->count == 1);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_boundary_cuts_csv with unmatched state variable "
    "header")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_bdr_unmatched_svar.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    // "nonexistent_rsv" does not match any element
    ofs << "name,iteration,scene,rhs,nonexistent_rsv\n";
    ofs << "cut1,1,1,10.0,1.0\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::combined;

  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  // Default mode is skip_coeff: cut is loaded with the missing
  // coefficient dropped (only the alpha term remains).
  CHECK(result->count == 1);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_boundary_cuts_csv with ClassName:ElementName "
    "header")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_bdr_class_name.csv")
          .string();

  {
    std::ofstream ofs(tmp_file);
    // Use "Junction:j_up" format for the header
    ofs << "name,iteration,scene,rhs,Junction:j_up\n";
    ofs << "cut1,1,1,10.0,1.0\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::combined;

  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 1);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_boundary_cuts_csv with wrong class filter in "
    "header")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_bdr_wrong_class.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    // "Battery:j_up" - j_up is a Junction, not a Battery
    ofs << "name,iteration,scene,rhs,Battery:j_up\n";
    ofs << "cut1,1,1,10.0,1.0\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::combined;

  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  // Default skip_coeff: cut loaded with coefficient dropped.
  CHECK(result->count == 1);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_boundary_cuts_csv with zero coefficients "
    "skipped")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_bdr_zero_coeff.csv")
          .string();

  {
    std::ofstream ofs(tmp_file);
    ofs << "name,iteration,scene,rhs,j_up\n";
    // Zero coefficient should be skipped in the row
    ofs << "cut1,1,1,10.0,0.0\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::combined;

  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 1);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_boundary_cuts_csv creates alpha column on "
    "demand")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_bdr_alpha_col.csv")
          .string();

  {
    std::ofstream ofs(tmp_file);
    ofs << "name,iteration,scene,rhs,j_up\n";
    ofs << "alpha_cut,1,1,100.0,5.0\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::combined;
  opts.alpha_min = -1e9;
  opts.alpha_max = 1e9;

  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  // Before loading, alpha_col should be unknown
  const auto last_phase =
      PhaseIndex {planning_lp.simulation().phases().size() - 1};
  CHECK(states[SceneIndex {0}][last_phase].alpha_col
        == ColIndex {unknown_index});

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 1);

  // After loading, alpha_col should be set
  CHECK(states[SceneIndex {0}][last_phase].alpha_col
        != ColIndex {unknown_index});

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_boundary_cuts_csv with empty body loads 0 "
    "cuts")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_bdr_empty_body.csv")
          .string();

  {
    std::ofstream ofs(tmp_file);
    ofs << "name,iteration,scene,rhs,j_up\n";
    // No data lines
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::combined;

  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 0);

  std::filesystem::remove(tmp_file);
}

// ─── load_named_cuts_csv tests ──────────────────────────────────────────────

TEST_CASE(
    "load_named_cuts_csv returns error for nonexistent "
    "file")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const SDDPOptions opts;
  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result = load_named_cuts_csv(planning_lp,
                                    "/tmp/gtopt_nonexistent_named.csv",
                                    opts,
                                    label_maker,
                                    states);
  CHECK_FALSE(result.has_value());
  CHECK(result.error().code == ErrorCode::FileIOError);
}

TEST_CASE(
    "load_named_cuts_csv rejects header with too few "
    "columns")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_named_bad_header.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    // Only 4 columns, need at least 6 (name,iteration,scene,phase,rhs + 1)
    ofs << "name,iteration,scene,phase\n";
    ofs << "cut1,1,1,1\n";
  }

  const SDDPOptions opts;
  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  CHECK_FALSE(result.has_value());
  CHECK(result.error().code == ErrorCode::InvalidInput);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_named_cuts_csv rejects header with wrong phase "
    "column name")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_named_wrong_phase.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    // Column 3 should be "phase" but is "stage"
    ofs << "name,iteration,scene,stage,rhs,j_up\n";
    ofs << "cut1,1,1,1,10.0,1.0\n";
  }

  const SDDPOptions opts;
  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  CHECK_FALSE(result.has_value());
  CHECK(result.error().code == ErrorCode::InvalidInput);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_named_cuts_csv loads cuts for specific "
    "phases")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_named_phases.csv")
          .string();

  {
    std::ofstream ofs(tmp_file);
    ofs << "name,iteration,scene,phase,rhs,j_up\n";
    // Phase UIDs 1, 2, 3 exist in 3-phase planning
    ofs << "cut_p1,1,1,1,10.0,1.0\n";
    ofs << "cut_p2,1,1,2,20.0,2.0\n";
    ofs << "cut_p3,1,1,3,30.0,3.0\n";
  }

  const SDDPOptions opts;
  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 3);

  // Verify alpha columns were created for all phases
  for (Index pi = 0; pi < 3; ++pi) {
    CHECK(states[SceneIndex {0}][PhaseIndex {pi}].alpha_col
          != ColIndex {unknown_index});
  }

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_named_cuts_csv skips cuts with unknown phase "
    "UID")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_named_unknown_phase.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    ofs << "name,iteration,scene,phase,rhs,j_up\n";
    // Phase UID 999 does not exist
    ofs << "bad_cut,1,1,999,10.0,1.0\n";
    // Phase UID 1 exists
    ofs << "good_cut,1,1,1,20.0,2.0\n";
  }

  const SDDPOptions opts;
  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  // Only the good_cut loaded; bad_cut skipped
  CHECK(result->count == 1);

  std::filesystem::remove(tmp_file);
}

TEST_CASE("load_named_cuts_csv with empty body loads 0 cuts")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_named_empty_body.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    ofs << "name,iteration,scene,phase,rhs,j_up\n";
    // No data lines
  }

  const SDDPOptions opts;
  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 0);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_named_cuts_csv with ClassName:ElementName "
    "header")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_named_class_name.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    ofs << "name,iteration,scene,phase,rhs,Junction:j_up\n";
    ofs << "cut1,1,1,1,10.0,1.0\n";
  }

  const SDDPOptions opts;
  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 1);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_named_cuts_csv with wrong class filter skips "
    "coefficient")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_named_wrong_class.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    // j_up is a Junction, not a Battery
    ofs << "name,iteration,scene,phase,rhs,Battery:j_up\n";
    ofs << "cut1,1,1,1,10.0,1.0\n";
  }

  const SDDPOptions opts;
  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  // Default skip_coeff: cut loaded with coefficient dropped.
  CHECK(result->count == 1);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_named_cuts_csv with zero coefficient "
    "skipped")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_named_zero_coeff.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    ofs << "name,iteration,scene,phase,rhs,j_up\n";
    ofs << "cut1,1,1,1,10.0,0.0\n";
  }

  const SDDPOptions opts;
  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 1);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_named_cuts_csv with multiple state variable "
    "columns")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_named_multi_svar.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    // j_up is a real junction (uid 1), nonexistent is not
    ofs << "name,iteration,scene,phase,rhs,j_up,nonexistent\n";
    ofs << "cut1,1,1,2,50.0,3.0,7.0\n";
  }

  const SDDPOptions opts;
  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  // Default skip_coeff: cut loaded, "nonexistent" coefficient dropped.
  CHECK(result->count == 1);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_named_cuts_csv skips blank lines in "
    "body")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_named_blank_lines.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    ofs << "name,iteration,scene,phase,rhs,j_up\n";
    ofs << "cut1,1,1,1,10.0,1.0\n";
    ofs << "\n";
    ofs << "cut2,2,1,2,20.0,2.0\n";
    ofs << "\n";
  }

  const SDDPOptions opts;
  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 2);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_named_cuts_csv caches per-phase column "
    "maps")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_named_cache.csv")
          .string();

  {
    std::ofstream ofs(tmp_file);
    ofs << "name,iteration,scene,phase,rhs,j_up\n";
    // Multiple cuts for the same phase to exercise caching
    ofs << "cut1,1,1,1,10.0,1.0\n";
    ofs << "cut2,2,1,1,20.0,2.0\n";
    ofs << "cut3,3,1,1,30.0,3.0\n";
  }

  const SDDPOptions opts;
  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 3);

  std::filesystem::remove(tmp_file);
}

// ─── Windows \r\n line ending tests ─────────────────────────────────────────

TEST_CASE(
    "load_boundary_cuts_csv handles Windows \\r\\n line "
    "endings in header")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_bdr_crlf.csv")
          .string();

  // Write CSV with Windows \r\n line endings.
  // The last header token "j_up" would get a trailing \r without the fix.
  {
    std::ofstream ofs(tmp_file, std::ios::binary);
    ofs << "name,iteration,scene,rhs,j_up\r\n";
    ofs << "bdr_cut1,1,0,100.0,5.0\r\n";
    ofs << "bdr_cut2,2,0,200.0,10.0\r\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;
  opts.boundary_max_iterations = 0;

  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  // The "j_up" header must be matched despite \r\n endings
  CHECK(result->count == 2);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_named_cuts_csv handles Windows \\r\\n line "
    "endings in header")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_named_crlf.csv")
          .string();

  // Write CSV with Windows \r\n line endings
  {
    std::ofstream ofs(tmp_file, std::ios::binary);
    ofs << "name,iteration,scene,phase,rhs,j_up\r\n";
    ofs << "cut1,1,1,1,10.0,1.0\r\n";
    ofs << "cut2,2,1,1,20.0,2.0\r\n";
  }

  const SDDPOptions opts;
  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 2);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_cuts_csv handles Windows \\r\\n line endings in "
    "data lines")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_cuts_crlf.csv")
          .string();

  // Write CSV with DOS \r\n line endings in both header and data
  {
    std::ofstream ofs(tmp_file, std::ios::binary);
    ofs << "# scale_objective=1\r\n";
    ofs << "phase,scene,name,rhs,coefficients\r\n";
    ofs << "1,1,cut_crlf1,50.0,0:1.5\r\n";
    ofs << "2,1,cut_crlf2,30.0,0:0.5\r\n";
  }

  const LabelMaker label_maker(planning_lp.options());
  auto result = load_cuts_csv(planning_lp, tmp_file, label_maker);
  REQUIRE(result.has_value());
  CHECK(result->count == 2);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_boundary_cuts_csv handles Windows \\r\\n line "
    "endings in data lines")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_bdr_crlf_data.csv")
          .string();

  // Write CSV with DOS \r\n in header AND data lines
  {
    std::ofstream ofs(tmp_file, std::ios::binary);
    ofs << "name,iteration,scene,rhs,j_up\r\n";
    ofs << "bdr1,1,0,100.0,5.0\r\n";
    ofs << "bdr2,2,0,200.0,10.0\r\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;
  opts.boundary_max_iterations = 0;

  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 2);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_named_cuts_csv handles Windows \\r\\n line "
    "endings in data lines")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_named_crlf_data.csv")
                            .string();

  // Write CSV with DOS \r\n in header AND data lines
  {
    std::ofstream ofs(tmp_file, std::ios::binary);
    ofs << "name,iteration,scene,phase,rhs,j_up\r\n";
    ofs << "cut1,1,1,1,10.0,1.0\r\n";
    ofs << "cut2,2,1,1,20.0,2.0\r\n";
  }

  const SDDPOptions opts;
  const LabelMaker label_maker(planning_lp.options());
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 2);

  std::filesystem::remove(tmp_file);
}
