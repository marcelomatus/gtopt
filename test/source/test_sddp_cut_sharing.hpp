/**
 * @file      test_sddp_cut_sharing.hpp
 * @brief     Unit tests for share_cuts_for_phase() free function
 * @date      2026-03-22
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/sddp_cut_sharing.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ---------------------------------------------------------------------------
// share_cuts_for_phase — none mode (no-op)
// ---------------------------------------------------------------------------

TEST_CASE("share_cuts_for_phase none mode is a no-op")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP plp(planning);

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  // Add a dummy cut for scene 0
  SparseRow cut("dummy_cut");
  cut.lowb = 10.0;
  cut.uppb = LinearProblem::DblMax;
  cut[ColIndex {
      0,
  }] = 1.0;
  scene_cuts[SceneIndex {0}].push_back(cut);

  const auto rows_before = plp.system(SceneIndex {0}, PhaseIndex {0})
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(
      PhaseIndex {0}, scene_cuts, CutSharingMode::none, plp, "test");

  // none mode should not add any rows
  const auto rows_after = plp.system(SceneIndex {0}, PhaseIndex {0})
                              .linear_interface()
                              .get_numrows();
  CHECK(rows_after == rows_before);
}

// ---------------------------------------------------------------------------
// share_cuts_for_phase — single scene (no sharing needed)
// ---------------------------------------------------------------------------

TEST_CASE("share_cuts_for_phase single scene returns early")  // NOLINT
{
  // With only 1 scene, no sharing is possible regardless of mode
  auto planning = make_3phase_hydro_planning();
  PlanningLP plp(planning);

  // Only 1 scene in the test planning
  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 1);

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  SparseRow cut("cut1");
  cut.lowb = 5.0;
  cut.uppb = LinearProblem::DblMax;
  cut[ColIndex {
      0,
  }] = 2.0;
  scene_cuts[SceneIndex {0}].push_back(cut);

  const auto rows_before = plp.system(SceneIndex {0}, PhaseIndex {0})
                               .linear_interface()
                               .get_numrows();

  // Even with accumulate mode, single scene should not add cuts
  share_cuts_for_phase(
      PhaseIndex {0}, scene_cuts, CutSharingMode::accumulate, plp, "test");

  const auto rows_after = plp.system(SceneIndex {0}, PhaseIndex {0})
                              .linear_interface()
                              .get_numrows();
  CHECK(rows_after == rows_before);
}

// ---------------------------------------------------------------------------
// share_cuts_for_phase — empty cuts
// ---------------------------------------------------------------------------

TEST_CASE("share_cuts_for_phase with empty cuts is a no-op")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP plp(planning);

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);
  // No cuts in any scene

  const auto rows_before = plp.system(SceneIndex {0}, PhaseIndex {0})
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(
      PhaseIndex {0}, scene_cuts, CutSharingMode::max, plp, "test");

  const auto rows_after = plp.system(SceneIndex {0}, PhaseIndex {0})
                              .linear_interface()
                              .get_numrows();
  CHECK(rows_after == rows_before);
}

// ---------------------------------------------------------------------------
// share_cuts_for_phase — accumulate mode
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "share_cuts_for_phase accumulate mode sums all cuts")
{
  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 2);

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  SparseRow cut_s0("cut_s0");
  cut_s0.lowb = 10.0;
  cut_s0.uppb = LinearProblem::DblMax;
  cut_s0[ColIndex {
      0,
  }] = 1.0;
  scene_cuts[SceneIndex {0}].push_back(cut_s0);

  SparseRow cut_s1("cut_s1");
  cut_s1.lowb = 20.0;
  cut_s1.uppb = LinearProblem::DblMax;
  cut_s1[ColIndex {
      0,
  }] = 2.0;
  scene_cuts[SceneIndex {1}].push_back(cut_s1);

  const auto rows_before = plp.system(SceneIndex {0}, PhaseIndex {0})
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(PhaseIndex {0},
                       scene_cuts,
                       CutSharingMode::accumulate,
                       plp,
                       "test_accum");

  // accumulate: one accumulated cut added to each scene
  const auto rows_s0 = plp.system(SceneIndex {0}, PhaseIndex {0})
                           .linear_interface()
                           .get_numrows();
  CHECK(rows_s0 == rows_before + 1);

  const auto rows_s1 = plp.system(SceneIndex {1}, PhaseIndex {0})
                           .linear_interface()
                           .get_numrows();
  CHECK(rows_s1 == rows_before + 1);
}

// ---------------------------------------------------------------------------
// share_cuts_for_phase — expected mode
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "share_cuts_for_phase expected mode uses probability-weighted average")
{
  auto planning = make_2scene_3phase_hydro_planning(0.7, 0.3);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 2);

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  SparseRow cut_s0("cut_s0");
  cut_s0.lowb = 15.0;
  cut_s0.uppb = LinearProblem::DblMax;
  cut_s0[ColIndex {
      0,
  }] = 3.0;
  scene_cuts[SceneIndex {0}].push_back(cut_s0);

  SparseRow cut_s1("cut_s1");
  cut_s1.lowb = 25.0;
  cut_s1.uppb = LinearProblem::DblMax;
  cut_s1[ColIndex {
      0,
  }] = 5.0;
  scene_cuts[SceneIndex {1}].push_back(cut_s1);

  const auto rows_before = plp.system(SceneIndex {0}, PhaseIndex {0})
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(
      PhaseIndex {0}, scene_cuts, CutSharingMode::expected, plp, "test_exp");

  // expected: one weighted-average cut added to each scene
  const auto rows_s0 = plp.system(SceneIndex {0}, PhaseIndex {0})
                           .linear_interface()
                           .get_numrows();
  CHECK(rows_s0 == rows_before + 1);

  const auto rows_s1 = plp.system(SceneIndex {1}, PhaseIndex {0})
                           .linear_interface()
                           .get_numrows();
  CHECK(rows_s1 == rows_before + 1);
}

// ---------------------------------------------------------------------------
// share_cuts_for_phase — max mode
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "share_cuts_for_phase max mode adds all cuts to all scenes")
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 2);

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  // 2 cuts in scene 0, 1 cut in scene 1 = 3 total
  SparseRow cut_a("cut_a");
  cut_a.lowb = 5.0;
  cut_a.uppb = LinearProblem::DblMax;
  cut_a[ColIndex {
      0,
  }] = 1.0;
  scene_cuts[SceneIndex {0}].push_back(cut_a);

  SparseRow cut_b("cut_b");
  cut_b.lowb = 7.0;
  cut_b.uppb = LinearProblem::DblMax;
  cut_b[ColIndex {
      0,
  }] = 1.5;
  scene_cuts[SceneIndex {0}].push_back(cut_b);

  SparseRow cut_c("cut_c");
  cut_c.lowb = 12.0;
  cut_c.uppb = LinearProblem::DblMax;
  cut_c[ColIndex {
      0,
  }] = 2.5;
  scene_cuts[SceneIndex {1}].push_back(cut_c);

  const auto rows_before = plp.system(SceneIndex {0}, PhaseIndex {0})
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(
      PhaseIndex {0}, scene_cuts, CutSharingMode::max, plp, "test_max");

  // max: all 3 cuts added to each scene
  const auto rows_s0 = plp.system(SceneIndex {0}, PhaseIndex {0})
                           .linear_interface()
                           .get_numrows();
  CHECK(rows_s0 == rows_before + 3);

  const auto rows_s1 = plp.system(SceneIndex {1}, PhaseIndex {0})
                           .linear_interface()
                           .get_numrows();
  CHECK(rows_s1 == rows_before + 3);
}

// ---------------------------------------------------------------------------
// share_cuts_for_phase — accumulate with one empty scene
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "share_cuts_for_phase accumulate with one empty scene")
{
  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  // Only scene 0 has a cut; scene 1 is empty
  SparseRow cut("cut_only_s0");
  cut.lowb = 8.0;
  cut.uppb = LinearProblem::DblMax;
  cut[ColIndex {
      0,
  }] = 1.0;
  scene_cuts[SceneIndex {0}].push_back(cut);

  const auto rows_before = plp.system(SceneIndex {0}, PhaseIndex {0})
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(PhaseIndex {0},
                       scene_cuts,
                       CutSharingMode::accumulate,
                       plp,
                       "test_partial");

  // The single cut is still accumulated and shared to both scenes
  const auto rows_s0 = plp.system(SceneIndex {0}, PhaseIndex {0})
                           .linear_interface()
                           .get_numrows();
  CHECK(rows_s0 == rows_before + 1);
}

// ---------------------------------------------------------------------------
// share_cuts_for_phase — expected with all empty cuts returns early
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "share_cuts_for_phase expected with empty cuts returns early")
{
  auto planning = make_2scene_3phase_hydro_planning(0.7, 0.3);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);
  // Both scenes have empty cuts

  const auto rows_before = plp.system(SceneIndex {0}, PhaseIndex {0})
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(
      PhaseIndex {0}, scene_cuts, CutSharingMode::expected, plp, "test_empty");

  // No cuts to average → no rows added
  const auto rows_after = plp.system(SceneIndex {0}, PhaseIndex {0})
                              .linear_interface()
                              .get_numrows();
  CHECK(rows_after == rows_before);
}
