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
 *
 * ``load_named_cuts_csv`` was retired in 2026-05; its tests (formerly
 * items 19-23) were removed alongside the CSV writer.
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

using namespace gtopt;

namespace
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

  // When option > 0, it is returned directly regardless of the cut
  // coefficient (the explicit override always wins).
  const auto alpha =
      effective_scale_alpha(planning_lp, 42.0, /*cut_max_coeff=*/9999.0);
  CHECK(alpha == doctest::Approx(42.0));
}

TEST_CASE(
    "effective_scale_alpha rounds cut coeff up to a power of ten")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  const PlanningLP planning_lp(std::move(planning));

  // option == 0 → auto-compute = max(scale_objective,
  // 10^ceil(log10(cut_max_coeff))).  7983 → 10^4 = 10000, well above the
  // (≤ 1e3) objective floor of the fixture.
  const auto alpha = effective_scale_alpha(planning_lp, 0.0, 7983.43);
  CHECK(alpha == doctest::Approx(10000.0));

  // A non-positive coefficient (no usable cut) degenerates to the
  // objective floor, never below 1.
  const auto floor_alpha = effective_scale_alpha(planning_lp, 0.0, 0.0);
  CHECK(floor_alpha >= 1.0);
}

// ─── boundary_cut_coeff_stats ───────────────────────────────────────────────

TEST_CASE("boundary_cut_coeff_stats summarises each cut column")  // NOLINT
{
  const auto tmp = std::filesystem::temp_directory_path()
      / "test_boundary_cut_coeff_stats.csv";
  {
    std::ofstream out(tmp);
    // header: scene, rhs, then two state columns
    out << "scene,rhs,RES_A,RES_B\n";
    out << "0,1000.0,-10.0,5.0\n";
    out << "0,2000.0,-30.0,5.0\n";  // RES_A: min -30, avg -20, max -10
  }

  const auto stats = boundary_cut_coeff_stats(tmp.string());
  REQUIRE(stats.size() == 2);

  const auto a = stats.find("RES_A");
  REQUIRE(a != stats.end());
  CHECK(a->second.min == doctest::Approx(-30.0));
  CHECK(a->second.avg == doctest::Approx(-20.0));
  CHECK(a->second.max == doctest::Approx(-10.0));

  const auto b = stats.find("RES_B");
  REQUIRE(b != stats.end());
  CHECK(b->second.min == doctest::Approx(5.0));
  CHECK(b->second.avg == doctest::Approx(5.0));
  CHECK(b->second.max == doctest::Approx(5.0));

  // scale_alpha helper uses max |avg|: max(|-20|, |5|) = 20.
  CHECK(boundary_cut_max_avg_coeff(tmp.string()) == doctest::Approx(20.0));

  // Missing / unreadable file → empty, and max-avg 0.
  CHECK(boundary_cut_coeff_stats("/no/such/file.csv").empty());
  CHECK(boundary_cut_max_avg_coeff("/no/such/file.csv")
        == doctest::Approx(0.0));

  std::filesystem::remove(tmp);
}

// ─── save_cuts_csv tests ────────────────────────────────────────────────────

// ─── save_cuts / load_cuts round-trip via SDDPMethod (Parquet) ──────────────

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

  // Phase 2: hot-start a fresh solver using saved cuts.  Since the
  // legacy CSV-based "named hot-start cuts" path was retired in
  // 2026-05, this Parquet round-trip is the single canonical path
  // for re-loading SDDP cuts across runs (cascade level transitions,
  // resumed runs, distributed solves).
  int num_loaded = 0;
  {
    auto planning2 = make_3phase_hydro_planning();
    PlanningLP planning_lp2(std::move(planning2));

    SDDPOptions load_opts;
    load_opts.max_iterations = 1;
    load_opts.convergence_tol = 1e-6;
    load_opts.cuts_input_file = cuts_file;
    load_opts.cut_recovery_mode = HotStartMode::replace;
    load_opts.recovery_mode = RecoveryMode::cuts;

    SDDPMethod sddp2(planning_lp2, load_opts);
    auto results2 = sddp2.solve();
    REQUIRE(results2.has_value());

    // After ``initialize_solver`` runs (inside ``solve``), the cuts
    // loaded from disk must be present in the manager BEFORE the new
    // iteration adds more cuts.  We compare ``stored_cuts.size()``
    // against ``num_saved`` allowing the post-solve count to be
    // strictly greater (this run added its own iter-1 cuts on top).
    num_loaded = static_cast<int>(sddp2.stored_cuts().size());
    CHECK(num_loaded >= num_saved);
  }

  std::filesystem::remove(cuts_file);
}

TEST_CASE(  // NOLINT
    "load_cuts_parquet multicut broadcasts inherited optimality cuts to "
    "all scenes")
{
  // Under `cut_sharing_mode=multicut` each scene-LP carries the full set
  // of future-cost columns `varphi_0..N-1`; at run time
  // `share_cuts_for_phase` broadcasts every scene's cut onto its
  // `varphi_S` in EVERY scene-LP, but that share is never persisted (only
  // origin cuts go through `store_cut`).  The loader must therefore
  // RECONSTRUCT the broadcast — otherwise only each scene's own `varphi_S`
  // is cut on load, the other N-1 columns free-fall to 0, and the LB
  // collapses at iter-1 of every cascade level.

  const auto dir = std::filesystem::temp_directory_path()
      / "gtopt_test_multicut_bcast_roundtrip";
  std::filesystem::create_directories(dir);
  const auto cuts_file = (dir / "sddp_cuts.parquet").string();

  // ── Phase 1: generate + save multicut cuts from a 2-scene SDDP ──
  {
    auto planning = make_2scene_3phase_hydro_planning();
    planning.options.sddp_options.cut_sharing_mode = CutSharingMode::multicut;
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions opts;
    opts.max_iterations = 3;
    opts.convergence_tol = 1e-6;
    opts.cut_sharing = CutSharingMode::multicut;
    opts.cuts_output_file = cuts_file;

    SDDPMethod sddp(planning_lp, opts);
    REQUIRE(sddp.solve().has_value());
    REQUIRE_FALSE(sddp.stored_cuts().empty());
  }
  REQUIRE(std::filesystem::exists(cuts_file));

  // ── Phase 2: load into a fresh multicut 2-scene solver ──
  auto planning2 = make_2scene_3phase_hydro_planning();
  planning2.options.sddp_options.cut_sharing_mode = CutSharingMode::multicut;
  PlanningLP planning_lp2(std::move(planning2));

  SDDPOptions lopts;
  lopts.cut_sharing = CutSharingMode::multicut;
  SDDPMethod sddp2(planning_lp2, lopts);
  REQUIRE(sddp2.ensure_initialized().has_value());

  const auto& sim = planning_lp2.simulation();
  const auto num_scenes = static_cast<Index>(sim.scenes().size());
  const auto num_phases = static_cast<Index>(sim.phases().size());
  REQUIRE(num_scenes == 2);

  // Sum LP rows across the whole (scene, phase) grid.  Measured AFTER
  // ensure_initialized so structural rows + alpha columns are baseline;
  // the load delta is then purely the installed cut rows.
  const auto total_rows = [&]
  {
    std::ptrdiff_t n = 0;
    for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
      for (const auto pi : iota_range<PhaseIndex>(0, num_phases)) {
        n += planning_lp2.system(si, pi).linear_interface().get_numrows();
      }
    }
    return n;
  };
  const auto rows_before = total_rows();

  auto loaded = sddp2.load_cuts(cuts_file);
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->count >= 1);

  const auto installed = total_rows() - rows_before;

  // Broadcast: every loaded OPTIMALITY cut is installed on ALL
  // `num_scenes` scene-LPs, so the total rows installed STRICTLY EXCEEDS
  // the origin-only source count `loaded->count` (which counts each cut
  // once).  Under the pre-fix per-scene routing, `installed` would equal
  // `loaded->count` exactly — this is the regression guard.
  CHECK(installed > static_cast<std::ptrdiff_t>(loaded->count));

  std::filesystem::remove_all(dir);
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
    ofs << "iteration,scene,rhs,j_up\n";
    ofs << "1,0,100.0,5.0\n";
    ofs << "2,0,200.0,10.0\n";
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
    ofs << "iteration,scene,rhs,j_up\n";
    ofs << "1,0,50.0,3.0\n";
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
    // Legacy format: no iteration column; default scene UID = 0.
    // No leading name column either (retired 2026-05): the only CSV
    // cut format gtopt still reads is the PLP-compatible "iter+scene+
    // rhs+state_vars" or the older "scene+rhs+state_vars" form.
    ofs << "scene,rhs,j_up\n";
    ofs << "0,75.0,4.0\n";
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
    ofs << "iteration,scene,rhs,j_up\n";
    // 3 distinct iterations: 1, 2, 3; default scene UID = 0
    ofs << "1,0,10.0,1.0\n";
    ofs << "2,0,20.0,2.0\n";
    ofs << "3,0,30.0,3.0\n";
    ofs << "3,0,35.0,3.5\n";
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
    ofs << "iteration,scene,rhs,j_up\n";
    // Scene UID 999 does not exist
    ofs << "1,999,10.0,1.0\n";
    // Scene UID 0 exists (default auto-generated scene)
    ofs << "1,0,20.0,2.0\n";
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
    ofs << "iteration,scene,rhs,nonexistent_rsv\n";
    ofs << "1,1,10.0,1.0\n";
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
    ofs << "iteration,scene,rhs,Junction:j_up\n";
    ofs << "1,1,10.0,1.0\n";
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
    ofs << "iteration,scene,rhs,Battery:j_up\n";
    ofs << "1,1,10.0,1.0\n";
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
    ofs << "iteration,scene,rhs,j_up\n";
    // Zero coefficient should be skipped in the row
    ofs << "1,1,10.0,0.0\n";
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
    ofs << "iteration,scene,rhs,j_up\n";
    ofs << "1,1,100.0,5.0\n";
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
    ofs << "iteration,scene,rhs,j_up\n";
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

// ``load_named_cuts_csv`` was retired in 2026-05.  All tests for the
// CSV-based named hot-start cut path were removed alongside it; the
// equivalent Parquet round-trip is covered by
// ``test_sddp_cut_parquet.cpp``.

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
    ofs << "iteration,scene,rhs,j_up\r\n";
    ofs << "1,0,100.0,5.0\r\n";
    ofs << "2,0,200.0,10.0\r\n";
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
    ofs << "iteration,scene,rhs,j_up\r\n";
    ofs << "1,0,100.0,5.0\r\n";
    ofs << "2,0,200.0,10.0\r\n";
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
