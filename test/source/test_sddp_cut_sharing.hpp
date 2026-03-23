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
