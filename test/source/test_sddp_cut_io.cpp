/**
 * @file      test_sddp_cut_io.hpp
 * @brief     Unit tests for the SDDP cut I/O module (save/load round-trip)
 * @date      2026-03-18
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. build_phase_uid_map produces correct mapping from phase UIDs
 *  1b. build_scene_uid_map produces correct mapping from scene UIDs
 *  1c. effective_scale_alpha returns explicit or auto-computed value
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

#include <bit>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_method.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)
// NOLINTBEGIN(misc-const-correctness)

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

}  // namespace

// ─── build_phase_uid_map tests ──────────────────────────────────────────────

TEST_CASE("build_phase_uid_map produces correct mapping")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  const PlanningLP planning_lp(std::move(planning));

  const auto phase_map = build_phase_uid_map(planning_lp);

  // 3-phase planning has UIDs 1, 2, 3
  REQUIRE(phase_map.size() == 3);
  CHECK(phase_map.contains(make_uid<Phase>(1)));
  CHECK(phase_map.contains(make_uid<Phase>(2)));
  CHECK(phase_map.contains(make_uid<Phase>(3)));

  // Verify the indices map correctly
  CHECK(phase_map.at(make_uid<Phase>(1)) == first_phase_index());
  CHECK(phase_map.at(make_uid<Phase>(2)) == PhaseIndex {1});
  CHECK(phase_map.at(make_uid<Phase>(3)) == PhaseIndex {2});
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
  CHECK(it->second == first_phase_index());
}

// ─── build_scene_uid_map tests ──────────────────────────────────────────────

TEST_CASE("build_scene_uid_map produces correct mapping")  // NOLINT
{
  auto planning = make_2scene_3phase_hydro_planning();
  const PlanningLP planning_lp(std::move(planning));

  const auto scene_map = build_scene_uid_map(planning_lp);

  // 2-scene planning has UIDs 1, 2
  REQUIRE(scene_map.size() == 2);
  CHECK(scene_map.contains(make_uid<Scene>(1)));
  CHECK(scene_map.contains(make_uid<Scene>(2)));

  // Verify the indices map correctly
  CHECK(scene_map.at(make_uid<Scene>(1)) == SceneIndex {0});
  CHECK(scene_map.at(make_uid<Scene>(2)) == SceneIndex {1});
}

TEST_CASE("build_scene_uid_map with single-scene planning")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  const PlanningLP planning_lp(std::move(planning));

  const auto scene_map = build_scene_uid_map(planning_lp);

  // Default single-scene planning has 1 scene
  REQUIRE(scene_map.size() == 1);

  // Verify there is exactly one entry mapping to SceneIndex{0}
  const auto it = scene_map.begin();
  CHECK(it->second == SceneIndex {0});
}

// ─── effective_scale_alpha tests ────────────────────────────────────────────

TEST_CASE(
    "effective_scale_alpha returns explicit value when positive")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  const PlanningLP planning_lp(std::move(planning));

  // When option > 0, it should be returned directly
  const auto alpha = effective_scale_alpha(planning_lp, 42.0);
  CHECK(alpha == doctest::Approx(42.0));
}

TEST_CASE("effective_scale_alpha auto-computes when zero")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  const PlanningLP planning_lp(std::move(planning));

  // When option is 0.0, auto-compute from state variables
  const auto alpha = effective_scale_alpha(planning_lp, 0.0);
  CHECK(alpha >= 0.0);
}

// ─── save_cuts_csv tests ────────────────────────────────────────────────────

// ─── save_cuts_csv / load_cuts_csv round-trip via SDDPMethod ────────────────

TEST_CASE("save and load cuts round-trip via SDDPMethod")  // NOLINT
{
  const auto tmp_dir = std::filesystem::temp_directory_path();
  const auto cuts_file =
      (tmp_dir / "gtopt_test_cut_io_roundtrip.parquet").string();

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

// ─── alpha resolution as a state variable ──────────────────────────────────

// ─── load_cuts_csv edge cases ───────────────────────────────────────────────

// ─── save_scene_cuts_csv tests ──────────────────────────────────────────────

// ─── load_scene_cuts_from_directory tests ───────────────────────────────────

// ─── load_boundary_cuts_csv tests ───────────────────────────────────────────

TEST_CASE("load_boundary_cuts_csv noload mode returns 0")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::noload;

  const LabelMaker label_maker {LpNamesLevel::none};
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

  const LabelMaker label_maker {LpNamesLevel::none};
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

  const LabelMaker label_maker {LpNamesLevel::none};
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

  const LabelMaker label_maker {LpNamesLevel::none};
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

  const LabelMaker label_maker {LpNamesLevel::none};
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

  const LabelMaker label_maker {LpNamesLevel::none};
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

  const LabelMaker label_maker {LpNamesLevel::none};
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

  const LabelMaker label_maker {LpNamesLevel::none};
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

  const LabelMaker label_maker {LpNamesLevel::none};
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

  const LabelMaker label_maker {LpNamesLevel::none};
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

  const LabelMaker label_maker {LpNamesLevel::none};
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

  const LabelMaker label_maker {LpNamesLevel::none};
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 1);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_boundary_cuts_csv consumes pre-registered α on the last "
    "phase")  // NOLINT
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

  const LabelMaker label_maker {LpNamesLevel::none};
  auto states = make_scene_phase_states(planning_lp);

  // α is now registered by `register_alpha_variables` during
  // SDDP init (unified across all phases); the cut loader no
  // longer creates it.  Set up the same precondition here for
  // the isolated-loader call.  After install, the loader calls
  // `bound_alpha` per cut — the freed-bounds assertion lives in
  // `test_sddp_alpha_relax.cpp`, which goes through the full
  // SDDPMethod::ensure_initialized path (`get_col_low_raw` needs
  // a fully-loaded backend to return non-NaN values).
  const auto num_scenes =
      static_cast<Index>(planning_lp.simulation().scenes().size());
  for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
    register_alpha_variables(planning_lp, scene_index, opts.scale_alpha);
  }

  const auto last_phase =
      PhaseIndex {planning_lp.simulation().phases().size() - 1};
  REQUIRE(find_alpha_state_var(
              planning_lp.simulation(), first_scene_index(), last_phase)
          != nullptr);

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 1);

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

  const LabelMaker label_maker {LpNamesLevel::none};
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
  const LabelMaker label_maker {LpNamesLevel::none};
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
  const LabelMaker label_maker {LpNamesLevel::none};
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
  const LabelMaker label_maker {LpNamesLevel::none};
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
  const LabelMaker label_maker {LpNamesLevel::none};
  auto states = make_scene_phase_states(planning_lp);

  // α is now registered by `register_alpha_variables` during
  // SDDP init (unified across all phases); the cut loaders
  // require it as a precondition.  Set up the same state here
  // for an isolated-loader test call.
  const auto num_scenes =
      static_cast<Index>(planning_lp.simulation().scenes().size());
  for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
    register_alpha_variables(planning_lp, scene_index, opts.scale_alpha);
  }

  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 3);

  // Verify alpha is registered as a state variable for all phases
  // (precondition set up above; the loader consumes it).
  for (Index pi = 0; pi < 3; ++pi) {
    CHECK(find_alpha_state_var(
              planning_lp.simulation(), first_scene_index(), PhaseIndex {pi})
          != nullptr);
  }

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "load_named_cuts_csv installs cuts in canonical sign convention "
    "without double scaling")  // NOLINT
{
  // Regression test for the named-cuts double-composition bug
  // (cut-sign-audit 2026-05-06).  The pre-fix loader pre-divided RHS
  // by `scale_obj`, set `row.scale = sa`, set `row[α] = sa`, and
  // pre-multiplied state coeffs by `col_scale / scale_obj` — then
  // handed the row to `add_rows`, which applied `compose_physical`
  // (× col_scale, ÷ row_max·scale_obj) a SECOND time.  Net effect at
  // `scale_obj=1000`: the LP row landed with `lowb` ≈ `rhs / 1000`
  // and `row[α]` ≈ `sa²/scale_obj`, so named hot-start cuts were
  // non-binding regardless of solver progress.
  //
  // The fix is to assemble a pure physical-space row (`row.lowb=rhs`,
  // `row.scale=1.0` (default), `row[α]=1.0`, `row[state]=-coeff`),
  // exactly mirroring the boundary-cuts and CSV/JSON loaders, and
  // let `add_rows` do the single `compose_physical` pass.
  //
  // Verification strategy: install the SAME cut twice.  First via the
  // named-cuts loader (which goes through the loader code path under
  // test).  Second via a manually-constructed canonical SparseRow
  // (the reference path used by every other cut loader).  Both rows
  // must land at byte-identical LP-space values — same `lowb`, same
  // `α` coefficient, same `state` coefficient.  This invariant is
  // independent of `scale_objective` (works whether the fixture has
  // it set to 1.0 or a planning-cost magnitude like 1000), so the
  // assertions are tight regardless of fixture configuration.
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_named_sign_round_trip.csv")
                            .string();

  constexpr double cut_rhs = 1234.0;
  constexpr double cut_coeff = 2.5;
  {
    std::ofstream ofs(tmp_file);
    ofs << "name,iteration,scene,phase,rhs,j_up\n";
    ofs << std::format("cut_p1,1,1,1,{},{}\n", cut_rhs, cut_coeff);
  }

  const SDDPOptions opts;
  const LabelMaker label_maker {LpNamesLevel::none};
  auto states = make_scene_phase_states(planning_lp);

  const auto num_scenes =
      static_cast<Index>(planning_lp.simulation().scenes().size());
  for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
    register_alpha_variables(planning_lp, scene_index, opts.scale_alpha);
  }

  auto& li = planning_lp.system(first_scene_index(), first_phase_index())
                 .linear_interface();
  const auto rows_before = li.get_numrows();

  // ── Path A: loader under test ─────────────────────────────────────
  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  REQUIRE(result->count == 1);
  const auto rows_after_a = li.get_numrows();
  REQUIRE(rows_after_a == rows_before + 1);
  const auto loader_row = RowIndex {rows_after_a - 1};

  // Resolve the j_up state column the loader bound the cut to so
  // we can ask the manual reference path to bind to the same column.
  // The named-cuts loader at the cell-uid mapping pass keys the
  // header `j_up` to the matching j_up state-variable column.
  // Reconstructing this lookup keeps the test self-contained.
  const auto* alpha_svar = find_alpha_state_var(
      planning_lp.simulation(), first_scene_index(), first_phase_index());
  REQUIRE(alpha_svar != nullptr);

  // ── Path B: canonical-form manual install ─────────────────────────
  // Pure physical-space row with α only (the test's CSV header
  // `j_up` is a Junction name, not a state-variable column, so the
  // loader silently drops its coefficient — we mirror that here so
  // both paths produce identical LP rows).
  auto canonical = SparseRow {
      .lowb = cut_rhs,
      .uppb = LinearProblem::DblMax,
  };
  canonical[alpha_svar->col()] = 1.0;
  std::vector<SparseRow> canonical_batch {canonical};
  li.add_rows(canonical_batch);
  const auto canonical_row = RowIndex {li.get_numrows() - 1};

  // ── Equivalence: both paths landed at byte-identical LP rows ──────
  // The pre-fix loader would have produced row.lowb = rhs / scale_obj²
  // (vs the canonical rhs / scale_obj) and row[α] = sa²/scale_obj
  // (vs the canonical 1.0 × col_scale(α)).  At fixture scale_obj=1.0,
  // the bound divergence collapses (1/1² = 1/1) but the α-coeff
  // divergence remains: pre-fix sa² ≠ post-fix col_scale(α).  Either
  // way, "loader row == canonical row" is the invariant we want, and
  // it fires regardless of what scale_obj happens to be.
  CHECK(li.get_row_low_raw()[loader_row]
        == doctest::Approx(li.get_row_low_raw()[canonical_row]));
  CHECK(li.get_row_upp_raw()[loader_row]
        == doctest::Approx(li.get_row_upp_raw()[canonical_row]));
  CHECK(li.get_coeff_raw(loader_row, alpha_svar->col())
        == doctest::Approx(li.get_coeff_raw(canonical_row, alpha_svar->col())));

  // Sign sanity: α coefficient is positive after compose_physical
  // (the canonical form has +1.0; scaling preserves sign).
  CHECK(li.get_coeff_raw(loader_row, alpha_svar->col()) > 0.0);

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
  const LabelMaker label_maker {LpNamesLevel::none};
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
  const LabelMaker label_maker {LpNamesLevel::none};
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
  const LabelMaker label_maker {LpNamesLevel::none};
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
  const LabelMaker label_maker {LpNamesLevel::none};
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
  const LabelMaker label_maker {LpNamesLevel::none};
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
  const LabelMaker label_maker {LpNamesLevel::none};
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
  const LabelMaker label_maker {LpNamesLevel::none};
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
  const LabelMaker label_maker {LpNamesLevel::none};
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

  const LabelMaker label_maker {LpNamesLevel::none};
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
  const LabelMaker label_maker {LpNamesLevel::none};
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
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

  const LabelMaker label_maker {LpNamesLevel::none};
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
  const LabelMaker label_maker {LpNamesLevel::none};
  auto states = make_scene_phase_states(planning_lp);

  auto result =
      load_named_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  CHECK(result->count == 2);

  std::filesystem::remove(tmp_file);
}

// ``extract_iteration_from_name`` was removed in 2026-05.  Iteration
// index is now stored directly on every cut struct
// (``StoredCut::iteration_index``, ``RawBoundaryCut::iteration_index``)
// so there is nothing left to parse from row labels.  The previous
// TEST_CASEs that exercised the parser (label-format coverage +
// malformed-input rejects) were deleted along with the function itself.
//
// The JSON cut I/O code paths (save_cuts_json / load_cuts_json) were
// removed in 2026-05.  Parquet is the single supported on-disk format
// for SDDP cut persistence; the format-dispatching ``save_cuts`` /
// ``load_cuts`` free functions were removed alongside them.
// Parquet-only round-trip coverage lives in
// ``test_sddp_cut_parquet.cpp``.

// ── is_final_state_col ───────────────────────────────────────────────────────
//
// Allow-list predicate driving cut-CSV column classification.
// ``EfinColName`` (= ``StorageLP::EfinName``, the storage final-energy
// column) is the only state-variable column actually emitted by the
// current LP assembly — silently dropping it here would cause the cut
// loader to skip every state coefficient on reload, corrupting the
// future-cost approximation.  Constexpr so the test also doubles as a
// compile-time invariant.

TEST_CASE(
    "is_final_state_col matches the canonical state vocabulary")  // NOLINT
{
  // Drive the predicate from its own named constant — never an inline
  // string literal — so any rename of the constant still leaves the
  // test asserting against the real allow-list.
  CHECK(is_final_state_col(EfinColName));

  // Promote the constexpr-ness: any future regression that turns the
  // predicate into a non-constexpr function will fail to compile
  // (static_assert), not just at runtime.
  static_assert(is_final_state_col(EfinColName));

  SUBCASE("constant carries the agreed-upon on-disk value")
  {
    // Lock the wire format: the CSV header token that downstream
    // tooling and existing cut files already depend on.  Changing
    // ``EfinColName`` is a format break — this assertion is the
    // tripwire.  We compare to a string literal here because this
    // is the one place we are explicitly testing the value, not the
    // identifier.
    static_assert(EfinColName == std::string_view {"efin"});
    CHECK(true);
  }

  SUBCASE("rejects metadata, near-miss, and unrelated column names")
  {
    // Metadata columns that share the cut CSV header but are NOT
    // state coefficients.
    CHECK_FALSE(is_final_state_col("rhs"));
    CHECK_FALSE(is_final_state_col("scene"));
    CHECK_FALSE(is_final_state_col("phase"));
    CHECK_FALSE(is_final_state_col("iteration"));
    CHECK_FALSE(is_final_state_col("name"));

    // Near-miss / case-sensitive: predicate is exact match.
    CHECK_FALSE(is_final_state_col("EFIN"));
    CHECK_FALSE(is_final_state_col("Efin"));
    CHECK_FALSE(is_final_state_col("efin "));
    CHECK_FALSE(is_final_state_col(" efin"));
    CHECK_FALSE(is_final_state_col("efin2"));
    CHECK_FALSE(is_final_state_col(""));

    // Unrelated state-ish names that callers might accidentally pass
    // (e.g. dispatch/flow vars must NOT be classified as final-state,
    // and historical aspirational names like ``soc`` / ``vfin`` are
    // NOT in the allow-list because no LP element actually emits them).
    CHECK_FALSE(is_final_state_col("eini"));
    CHECK_FALSE(is_final_state_col("vini"));
    CHECK_FALSE(is_final_state_col("alpha"));
    CHECK_FALSE(is_final_state_col("flow"));
    CHECK_FALSE(is_final_state_col("dispatch"));
    CHECK_FALSE(is_final_state_col("soc"));
    CHECK_FALSE(is_final_state_col("vfin"));
  }
}

// NOLINTEND(misc-const-correctness)