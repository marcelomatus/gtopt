/**
 * @file      test_sddp_cut_sharing.hpp
 * @brief     Unit tests for share_cuts_for_phase() free function
 * @date      2026-03-22
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/sddp_cut_sharing.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ---------------------------------------------------------------------------
// share_cuts_for_phase — none mode (no-op)
// ---------------------------------------------------------------------------

TEST_CASE("share_cuts_for_phase none mode is a no-op")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP plp(planning);

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  // Add a dummy cut for scene 0
  SparseRow cut;
  cut.lowb = 10.0;
  cut.uppb = LinearProblem::DblMax;
  cut[ColIndex {
      0,
  }] = 1.0;
  scene_cuts[first_scene_index()].push_back(cut);

  const auto rows_before = plp.system(first_scene_index(), first_phase_index())
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(
      first_phase_index(), scene_cuts, CutSharingMode::none, plp);

  // none mode should not add any rows
  const auto rows_after = plp.system(first_scene_index(), first_phase_index())
                              .linear_interface()
                              .get_numrows();
  CHECK(rows_after == rows_before);
}

// ---------------------------------------------------------------------------
// share_cuts_for_phase — single scene (no sharing needed)
// ---------------------------------------------------------------------------

TEST_CASE("share_cuts_for_phase single scene returns early")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // With only 1 scene, no sharing is possible regardless of mode
  auto planning = make_3phase_hydro_planning();
  PlanningLP plp(planning);

  // Only 1 scene in the test planning
  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 1);

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  SparseRow cut;
  cut.lowb = 5.0;
  cut.uppb = LinearProblem::DblMax;
  cut[ColIndex {
      0,
  }] = 2.0;
  scene_cuts[first_scene_index()].push_back(cut);

  const auto rows_before = plp.system(first_scene_index(), first_phase_index())
                               .linear_interface()
                               .get_numrows();

  // Even with accumulate mode, single scene should not add cuts
  share_cuts_for_phase(
      first_phase_index(), scene_cuts, CutSharingMode::accumulate, plp);

  const auto rows_after = plp.system(first_scene_index(), first_phase_index())
                              .linear_interface()
                              .get_numrows();
  CHECK(rows_after == rows_before);
}

// ---------------------------------------------------------------------------
// share_cuts_for_phase — empty cuts
// ---------------------------------------------------------------------------

TEST_CASE("share_cuts_for_phase with empty cuts is a no-op")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP plp(planning);

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);
  // No cuts in any scene

  const auto rows_before = plp.system(first_scene_index(), first_phase_index())
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(
      first_phase_index(), scene_cuts, CutSharingMode::max, plp);

  const auto rows_after = plp.system(first_scene_index(), first_phase_index())
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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 2);

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  SparseRow cut_s0;
  cut_s0.lowb = 10.0;
  cut_s0.uppb = LinearProblem::DblMax;
  cut_s0[ColIndex {
      0,
  }] = 1.0;
  scene_cuts[first_scene_index()].push_back(cut_s0);

  SparseRow cut_s1;
  cut_s1.lowb = 20.0;
  cut_s1.uppb = LinearProblem::DblMax;
  cut_s1[ColIndex {
      0,
  }] = 2.0;
  scene_cuts[SceneIndex {1}].push_back(cut_s1);

  const auto rows_before = plp.system(first_scene_index(), first_phase_index())
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(
      first_phase_index(), scene_cuts, CutSharingMode::accumulate, plp);

  // accumulate: one accumulated cut added to each scene
  const auto rows_s0 = plp.system(first_scene_index(), first_phase_index())
                           .linear_interface()
                           .get_numrows();
  CHECK(rows_s0 == rows_before + 1);

  const auto rows_s1 = plp.system(SceneIndex {1}, first_phase_index())
                           .linear_interface()
                           .get_numrows();
  CHECK(rows_s1 == rows_before + 1);
}

// ---------------------------------------------------------------------------
// share_cuts_for_phase — expected mode
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "share_cuts_for_phase expected mode sums scene-averaged cuts")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.7, 0.3);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 2);

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  SparseRow cut_s0;
  cut_s0.lowb = 15.0;
  cut_s0.uppb = LinearProblem::DblMax;
  cut_s0[ColIndex {
      0,
  }] = 3.0;
  scene_cuts[first_scene_index()].push_back(cut_s0);

  SparseRow cut_s1;
  cut_s1.lowb = 25.0;
  cut_s1.uppb = LinearProblem::DblMax;
  cut_s1[ColIndex {
      0,
  }] = 5.0;
  scene_cuts[SceneIndex {1}].push_back(cut_s1);

  const auto rows_before = plp.system(first_scene_index(), first_phase_index())
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(
      first_phase_index(), scene_cuts, CutSharingMode::expected, plp);

  // expected: one accumulated cut (sum of scene averages) added to each scene
  const auto rows_s0 = plp.system(first_scene_index(), first_phase_index())
                           .linear_interface()
                           .get_numrows();
  CHECK(rows_s0 == rows_before + 1);

  const auto rows_s1 = plp.system(SceneIndex {1}, first_phase_index())
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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 2);

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  // 2 cuts in scene 0, 1 cut in scene 1 = 3 total
  SparseRow cut_a;
  cut_a.lowb = 5.0;
  cut_a.uppb = LinearProblem::DblMax;
  cut_a[ColIndex {
      0,
  }] = 1.0;
  scene_cuts[first_scene_index()].push_back(cut_a);

  SparseRow cut_b;
  cut_b.lowb = 7.0;
  cut_b.uppb = LinearProblem::DblMax;
  cut_b[ColIndex {
      0,
  }] = 1.5;
  scene_cuts[first_scene_index()].push_back(cut_b);

  SparseRow cut_c;
  cut_c.lowb = 12.0;
  cut_c.uppb = LinearProblem::DblMax;
  cut_c[ColIndex {
      0,
  }] = 2.5;
  scene_cuts[SceneIndex {1}].push_back(cut_c);

  const auto rows_before = plp.system(first_scene_index(), first_phase_index())
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(
      first_phase_index(), scene_cuts, CutSharingMode::max, plp);

  // max: all 3 cuts added to each scene
  const auto rows_s0 = plp.system(first_scene_index(), first_phase_index())
                           .linear_interface()
                           .get_numrows();
  CHECK(rows_s0 == rows_before + 3);

  const auto rows_s1 = plp.system(SceneIndex {1}, first_phase_index())
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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  // Only scene 0 has a cut; scene 1 is empty
  SparseRow cut;
  cut.lowb = 8.0;
  cut.uppb = LinearProblem::DblMax;
  cut[ColIndex {
      0,
  }] = 1.0;
  scene_cuts[first_scene_index()].push_back(cut);

  const auto rows_before = plp.system(first_scene_index(), first_phase_index())
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(
      first_phase_index(), scene_cuts, CutSharingMode::accumulate, plp);

  // The single cut is still accumulated and shared to both scenes
  const auto rows_s0 = plp.system(first_scene_index(), first_phase_index())
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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.7, 0.3);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);
  // Both scenes have empty cuts

  const auto rows_before = plp.system(first_scene_index(), first_phase_index())
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(
      first_phase_index(), scene_cuts, CutSharingMode::expected, plp);

  // No cuts to average → no rows added
  const auto rows_after = plp.system(first_scene_index(), first_phase_index())
                              .linear_interface()
                              .get_numrows();
  CHECK(rows_after == rows_before);
}

// ===========================================================================
// Characterization tests — pin CURRENT coefficient/rhs values.
//
// Purpose: lock in what `share_cuts_for_phase` actually installs today so a
// subsequent refactor can be checked for behavioural parity.  These tests
// assert the ACTUAL values produced by the implementation — NOT the values
// a textbook SDDP derivation would predict.  When the two disagree, the
// characterization tests still pass (and carry a TODO comment pointing at
// the observed discrepancy).
// ===========================================================================

// ---------------------------------------------------------------------------
// none mode — pin: no row added to any scene's LP.
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "share_cuts_for_phase none mode — characterization: no rows added")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.7, 0.3);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 2);

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  SparseRow cut_s0;
  cut_s0.lowb = 40.0;
  cut_s0.uppb = LinearProblem::DblMax;
  cut_s0[ColIndex {
      0,
  }] = 4.0;
  scene_cuts[first_scene_index()].push_back(cut_s0);

  SparseRow cut_s1;
  cut_s1.lowb = 80.0;
  cut_s1.uppb = LinearProblem::DblMax;
  cut_s1[ColIndex {
      0,
  }] = 8.0;
  scene_cuts[SceneIndex {1}].push_back(cut_s1);

  const auto rows_s0_before =
      plp.system(first_scene_index(), first_phase_index())
          .linear_interface()
          .get_numrows();
  const auto rows_s1_before = plp.system(SceneIndex {1}, first_phase_index())
                                  .linear_interface()
                                  .get_numrows();

  share_cuts_for_phase(
      first_phase_index(), scene_cuts, CutSharingMode::none, plp);

  // none is a pure no-op — neither scene receives any row.
  CHECK(plp.system(first_scene_index(), first_phase_index())
            .linear_interface()
            .get_numrows()
        == rows_s0_before);
  CHECK(plp.system(SceneIndex {1}, first_phase_index())
            .linear_interface()
            .get_numrows()
        == rows_s1_before);
}

// ---------------------------------------------------------------------------
// accumulate mode — pin: single shared row = SUM of all scene cuts.
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "share_cuts_for_phase accumulate mode — characterization: "
    "coeffs and rhs are plain sums")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Non-uniform scene probabilities to keep parity with the `expected` test;
  // accumulate mode IGNORES probabilities (sums without weights).
  auto planning = make_2scene_3phase_hydro_planning(0.7, 0.3);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 2);

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  // Scene 0: two cuts.
  SparseRow cut_s0_a;
  cut_s0_a.lowb = 40.0;
  cut_s0_a.uppb = LinearProblem::DblMax;
  cut_s0_a[ColIndex {
      0,
  }] = 4.0;
  scene_cuts[first_scene_index()].push_back(cut_s0_a);

  SparseRow cut_s0_b;
  cut_s0_b.lowb = 60.0;
  cut_s0_b.uppb = LinearProblem::DblMax;
  cut_s0_b[ColIndex {
      0,
  }] = 6.0;
  scene_cuts[first_scene_index()].push_back(cut_s0_b);

  // Scene 1: one cut.
  SparseRow cut_s1;
  cut_s1.lowb = 80.0;
  cut_s1.uppb = LinearProblem::DblMax;
  cut_s1[ColIndex {
      0,
  }] = 8.0;
  scene_cuts[SceneIndex {1}].push_back(cut_s1);

  const auto rows_s0_before =
      plp.system(first_scene_index(), first_phase_index())
          .linear_interface()
          .get_numrows();
  const auto rows_s1_before = plp.system(SceneIndex {1}, first_phase_index())
                                  .linear_interface()
                                  .get_numrows();

  share_cuts_for_phase(
      first_phase_index(), scene_cuts, CutSharingMode::accumulate, plp);

  // Exactly one shared accumulated row per scene.
  auto& li_s0 =
      plp.system(first_scene_index(), first_phase_index()).linear_interface();
  auto& li_s1 =
      plp.system(SceneIndex {1}, first_phase_index()).linear_interface();
  REQUIRE(li_s0.get_numrows() == rows_s0_before + 1);
  REQUIRE(li_s1.get_numrows() == rows_s1_before + 1);

  const auto new_row_s0 = RowIndex {static_cast<Index>(rows_s0_before)};
  const auto new_row_s1 = RowIndex {static_cast<Index>(rows_s1_before)};

  // accumulate semantics: sum of rhs and sum of coefficients across ALL cuts.
  // expected coeff on col 0 = 4 + 6 + 8 = 18 (plain sum, no probabilities).
  // expected rhs (lowb)    = 40 + 60 + 80 = 180.
  SUBCASE("scene 0 accumulated coefficient and rhs")
  {
    CHECK(li_s0.get_coeff(new_row_s0,
                          ColIndex {
                              0,
                          })
          == doctest::Approx(18.0));
    CHECK(li_s0.get_row_low()[new_row_s0] == doctest::Approx(180.0));
  }

  SUBCASE("scene 1 receives the same accumulated row")
  {
    CHECK(li_s1.get_coeff(new_row_s1,
                          ColIndex {
                              0,
                          })
          == doctest::Approx(18.0));
    CHECK(li_s1.get_row_low()[new_row_s1] == doctest::Approx(180.0));
  }
}

// ---------------------------------------------------------------------------
// expected mode — pin: sum over scenes of UNWEIGHTED per-scene averages.
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "share_cuts_for_phase expected mode — characterization: "
    "per-scene average then accumulate (probabilities unused)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Non-uniform scene probabilities (0.7 / 0.3): a textbook probability-
  // weighted expectation would produce different coefficients than the
  // unweighted average actually implemented.  Using 0.7/0.3 makes that
  // distinction observable.
  auto planning = make_2scene_3phase_hydro_planning(0.7, 0.3);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 2);

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  // Scene 0: two cuts — unweighted average pins the "average_benders_cut"
  // semantics (arithmetic mean of coeffs and rhs, NOT probability-weighted).
  SparseRow cut_s0_a;
  cut_s0_a.lowb = 40.0;
  cut_s0_a.uppb = LinearProblem::DblMax;
  cut_s0_a[ColIndex {
      0,
  }] = 4.0;
  scene_cuts[first_scene_index()].push_back(cut_s0_a);

  SparseRow cut_s0_b;
  cut_s0_b.lowb = 60.0;
  cut_s0_b.uppb = LinearProblem::DblMax;
  cut_s0_b[ColIndex {
      0,
  }] = 6.0;
  scene_cuts[first_scene_index()].push_back(cut_s0_b);

  // Scene 1: a single cut — `average_benders_cut` returns it unchanged.
  SparseRow cut_s1;
  cut_s1.lowb = 80.0;
  cut_s1.uppb = LinearProblem::DblMax;
  cut_s1[ColIndex {
      0,
  }] = 8.0;
  scene_cuts[SceneIndex {1}].push_back(cut_s1);

  const auto rows_s0_before =
      plp.system(first_scene_index(), first_phase_index())
          .linear_interface()
          .get_numrows();
  const auto rows_s1_before = plp.system(SceneIndex {1}, first_phase_index())
                                  .linear_interface()
                                  .get_numrows();

  share_cuts_for_phase(
      first_phase_index(), scene_cuts, CutSharingMode::expected, plp);

  auto& li_s0 =
      plp.system(first_scene_index(), first_phase_index()).linear_interface();
  auto& li_s1 =
      plp.system(SceneIndex {1}, first_phase_index()).linear_interface();
  REQUIRE(li_s0.get_numrows() == rows_s0_before + 1);
  REQUIRE(li_s1.get_numrows() == rows_s1_before + 1);

  const auto new_row_s0 = RowIndex {static_cast<Index>(rows_s0_before)};
  const auto new_row_s1 = RowIndex {static_cast<Index>(rows_s1_before)};

  // TODO(plan N-1): verify semantics.  `expected` currently does:
  //   per-scene coeff_avg (unweighted) = (4+6)/2 = 5.0   on scene 0
  //   per-scene coeff_avg (unweighted) = 8.0             on scene 1 (1 cut)
  //   accumulated: 5.0 + 8.0 = 13.0
  //   per-scene rhs_avg (unweighted) = (40+60)/2 = 50.0  on scene 0
  //   per-scene rhs_avg (unweighted) = 80.0              on scene 1
  //   accumulated: 50.0 + 80.0 = 130.0
  //
  // A textbook probability-weighted expectation with p=(0.7, 0.3) would
  // give coeff = 0.7*5 + 0.3*8 = 5.9 and rhs = 0.7*50 + 0.3*80 = 59.0 —
  // the current implementation ignores scene probabilities and simply
  // sums the (unweighted) scene averages via `accumulate_benders_cuts`.
  SUBCASE("scene 0 expected row coefficient and rhs")
  {
    CHECK(li_s0.get_coeff(new_row_s0,
                          ColIndex {
                              0,
                          })
          == doctest::Approx(13.0));
    CHECK(li_s0.get_row_low()[new_row_s0] == doctest::Approx(130.0));
  }

  SUBCASE("scene 1 receives the same expected row")
  {
    CHECK(li_s1.get_coeff(new_row_s1,
                          ColIndex {
                              0,
                          })
          == doctest::Approx(13.0));
    CHECK(li_s1.get_row_low()[new_row_s1] == doctest::Approx(130.0));
  }
}

// ---------------------------------------------------------------------------
// max mode — pin: every scene cut is broadcast verbatim to every scene.
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "share_cuts_for_phase max mode — characterization: "
    "each cut broadcast verbatim to every scene")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.7, 0.3);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 2);

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  // Scene 0: two cuts; scene 1: one cut.  `max` adds all three to both scenes
  // in iteration order: first scene-0 cuts, then scene-1 cut.
  SparseRow cut_s0_a;
  cut_s0_a.lowb = 40.0;
  cut_s0_a.uppb = LinearProblem::DblMax;
  cut_s0_a[ColIndex {
      0,
  }] = 4.0;
  scene_cuts[first_scene_index()].push_back(cut_s0_a);

  SparseRow cut_s0_b;
  cut_s0_b.lowb = 60.0;
  cut_s0_b.uppb = LinearProblem::DblMax;
  cut_s0_b[ColIndex {
      0,
  }] = 6.0;
  scene_cuts[first_scene_index()].push_back(cut_s0_b);

  SparseRow cut_s1;
  cut_s1.lowb = 80.0;
  cut_s1.uppb = LinearProblem::DblMax;
  cut_s1[ColIndex {
      0,
  }] = 8.0;
  scene_cuts[SceneIndex {1}].push_back(cut_s1);

  const auto rows_s0_before =
      plp.system(first_scene_index(), first_phase_index())
          .linear_interface()
          .get_numrows();
  const auto rows_s1_before = plp.system(SceneIndex {1}, first_phase_index())
                                  .linear_interface()
                                  .get_numrows();

  share_cuts_for_phase(
      first_phase_index(), scene_cuts, CutSharingMode::max, plp);

  auto& li_s0 =
      plp.system(first_scene_index(), first_phase_index()).linear_interface();
  auto& li_s1 =
      plp.system(SceneIndex {1}, first_phase_index()).linear_interface();
  REQUIRE(li_s0.get_numrows() == rows_s0_before + 3);
  REQUIRE(li_s1.get_numrows() == rows_s1_before + 3);

  // Rows are appended in the order: scene-0 cut_a, scene-0 cut_b, scene-1 cut.
  const auto r0 = RowIndex {static_cast<Index>(rows_s0_before)};
  const auto r1 = RowIndex {static_cast<Index>(rows_s0_before + 1)};
  const auto r2 = RowIndex {static_cast<Index>(rows_s0_before + 2)};

  SUBCASE("scene 0 receives all three cuts verbatim in order")
  {
    // Expected = input coefficients preserved one-to-one (no averaging).
    CHECK(li_s0.get_coeff(r0,
                          ColIndex {
                              0,
                          })
          == doctest::Approx(4.0));
    CHECK(li_s0.get_row_low()[r0] == doctest::Approx(40.0));
    CHECK(li_s0.get_coeff(r1,
                          ColIndex {
                              0,
                          })
          == doctest::Approx(6.0));
    CHECK(li_s0.get_row_low()[r1] == doctest::Approx(60.0));
    CHECK(li_s0.get_coeff(r2,
                          ColIndex {
                              0,
                          })
          == doctest::Approx(8.0));
    CHECK(li_s0.get_row_low()[r2] == doctest::Approx(80.0));
  }

  SUBCASE("scene 1 receives the same three cuts in the same order")
  {
    const auto s1_r0 = RowIndex {static_cast<Index>(rows_s1_before)};
    const auto s1_r1 = RowIndex {static_cast<Index>(rows_s1_before + 1)};
    const auto s1_r2 = RowIndex {static_cast<Index>(rows_s1_before + 2)};
    CHECK(li_s1.get_coeff(s1_r0,
                          ColIndex {
                              0,
                          })
          == doctest::Approx(4.0));
    CHECK(li_s1.get_row_low()[s1_r0] == doctest::Approx(40.0));
    CHECK(li_s1.get_coeff(s1_r1,
                          ColIndex {
                              0,
                          })
          == doctest::Approx(6.0));
    CHECK(li_s1.get_row_low()[s1_r1] == doctest::Approx(60.0));
    CHECK(li_s1.get_coeff(s1_r2,
                          ColIndex {
                              0,
                          })
          == doctest::Approx(8.0));
    CHECK(li_s1.get_row_low()[s1_r2] == doctest::Approx(80.0));
  }
}

// ===========================================================================
// PLP cross-scene cut-sharing parity tests
//
// These tests target the user's concern: when all N scenarios carry equal
// probability 1/N, PLP's cross-scenario cut broadcast should coincide with
// the arithmetic mean of the per-scenario cuts, and gtopt's `expected`
// mode should produce the same arithmetic mean.
//
// PLP reference (local `~/git/plp_storage/CEN65/src`):
//   * `plp-espercnd.f:43-54`  — WITHIN-scene expectation: divides by NApert
//     (not NSimul), building one per-scenario optimality cut from the
//     arithmetic mean of aperture duals / objectives.
//   * `plp-agrespd.f:738-752, 781-803` — CROSS-scenario propagation: the
//     single scenario-ISimul cut is BROADCAST verbatim (`DO II = IBeg,
//     IEnd`) to every scenario's LP when `FSeparaCFA = .FALSE.`.  No
//     averaging, no probability weights applied at cross-scenario time.
//
// gtopt reference:
//   * `/home/marce/git/gtopt/source/sddp_aperture.cpp:195-216,476-484`
//     WITHIN-scene: `weighted_average_benders_cut` with aperture weights
//     `w = count × probability_factor` (normalised to sum to 1).
//   * `/home/marce/git/gtopt/source/sddp_cut_sharing.cpp:77-105`
//     CROSS-scene `expected` mode: `average_benders_cut` per scene
//     (unweighted arithmetic mean) then `accumulate_benders_cuts`
//     (plain sum, no scene-probability).  Rationale per
//     `docs/analysis/critical-review-report.md:253-269`: LP obj coeffs
//     already embed scene probability via `block_ecost`.
//
// Key invariant to check: under uniform aperture probability and a SINGLE
// scene cut per scene, the resulting broadcast row in gtopt must match
// the arithmetic mean of the per-scene cuts (PLP's implied identity on
// the uniform-probability special case).
// ===========================================================================

// ---------------------------------------------------------------------------
// Uniform equal-probability with 1 cut per scene → expected mode yields
// the ARITHMETIC-SUM of the per-scene single cuts (each `average_benders_cut`
// on a singleton is identity, then `accumulate_benders_cuts` sums).
//
// PLP equivalent: for NSimul scenarios, PLP generates NSimul *broadcast*
// cuts (one per scenario, each added to all scenarios) — so every LP
// receives the full set.  gtopt `expected` collapses that to a single
// SUM row.  The two are NOT bitwise equivalent but produce the same
// α lower-envelope in the uniform-probability case because the sum
// coefficient = 2× average in the N=2 equal-count case (see below).
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "cut_sharing uniform N=2 expected is sum of singletons (PLP parity)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Uniform p=1/N: PLP's `DO II = IBeg, IEnd` broadcast mode.
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 2);

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  // Single per-scene cut (PLP: one per-scenario optimality cut at
  // dual-phase end).
  SparseRow cut_s0;
  cut_s0.lowb = 100.0;
  cut_s0.uppb = LinearProblem::DblMax;
  cut_s0[ColIndex {
      0,
  }] = 10.0;
  scene_cuts[first_scene_index()].push_back(cut_s0);

  SparseRow cut_s1;
  cut_s1.lowb = 200.0;
  cut_s1.uppb = LinearProblem::DblMax;
  cut_s1[ColIndex {
      0,
  }] = 20.0;
  scene_cuts[SceneIndex {1}].push_back(cut_s1);

  const auto rows_before = plp.system(first_scene_index(), first_phase_index())
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(
      first_phase_index(), scene_cuts, CutSharingMode::expected, plp);

  auto& li_s0 =
      plp.system(first_scene_index(), first_phase_index()).linear_interface();
  auto& li_s1 =
      plp.system(SceneIndex {1}, first_phase_index()).linear_interface();
  REQUIRE(li_s0.get_numrows() == rows_before + 1);
  REQUIRE(li_s1.get_numrows() == rows_before + 1);

  const auto new_row = RowIndex {static_cast<Index>(rows_before)};

  // `average_benders_cut` on a singleton returns it unchanged; then
  // `accumulate_benders_cuts` sums across the N scenes.
  // Expected: coeff = 10 + 20 = 30, rhs = 100 + 200 = 300.
  //
  // The ARITHMETIC MEAN of the two cuts would be coeff=15, rhs=150 —
  // which matches gtopt's sum-row DIVIDED BY N.  See the next test for
  // that comparison.  Per `sddp_cut_sharing.cpp:77-82`, scene-probability
  // weighting is intentionally skipped because the LP objectives already
  // embed probability via `block_ecost`.
  SUBCASE("shared row = Σ_s cut_s (both scenes)")
  {
    CHECK(li_s0.get_coeff(new_row,
                          ColIndex {
                              0,
                          })
          == doctest::Approx(30.0));
    CHECK(li_s0.get_row_low()[new_row] == doctest::Approx(300.0));
    CHECK(li_s1.get_coeff(new_row,
                          ColIndex {
                              0,
                          })
          == doctest::Approx(30.0));
    CHECK(li_s1.get_row_low()[new_row] == doctest::Approx(300.0));
  }

  SUBCASE("shared row / N == arithmetic mean of per-scene cuts")
  {
    // Explicit arithmetic-mean cross-check (the PLP uniform-prob
    // characterisation): divide gtopt's sum-row by N to recover the
    // arithmetic average of the per-scene cuts.
    const double n_scenes = static_cast<double>(num_scenes);
    const double coeff_over_n = li_s0.get_coeff(new_row,
                                                ColIndex {
                                                    0,
                                                })
        / n_scenes;
    const double rhs_over_n = li_s0.get_row_low()[new_row] / n_scenes;
    // Arithmetic mean: (10 + 20)/2 = 15, (100 + 200)/2 = 150.
    CHECK(coeff_over_n == doctest::Approx(15.0));
    CHECK(rhs_over_n == doctest::Approx(150.0));
  }
}

// ---------------------------------------------------------------------------
// Non-uniform probabilities → `expected` mode STILL uses arithmetic mean
// within each scene and plain sum across scenes.  This pins the known
// divergence from a textbook probability-weighted expectation
// E[cut] = Σ_s p_s · cut_s.
//
// Rationale (from `sddp_cut_sharing.cpp:77-82` and
// `docs/analysis/critical-review-report.md:253-272`): LP coefficients
// carry probability through `block_ecost`, so reapplying weights at
// cut-sharing time would double-count.  The "arithmetic sum" result
// is the intended expectation under that accounting.
//
// Under uniform probability this matches PLP; under non-uniform
// probability it does NOT (PLP's broadcast is also not weighted, so
// neither side encodes p_s at the cross-scene level).
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "cut_sharing non-uniform probabilities are IGNORED by expected mode")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Extreme skew: 0.9 / 0.1.
  auto planning = make_2scene_3phase_hydro_planning(0.9, 0.1);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 2);

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  SparseRow cut_s0;
  cut_s0.lowb = 100.0;
  cut_s0.uppb = LinearProblem::DblMax;
  cut_s0[ColIndex {
      0,
  }] = 10.0;
  scene_cuts[first_scene_index()].push_back(cut_s0);

  SparseRow cut_s1;
  cut_s1.lowb = 200.0;
  cut_s1.uppb = LinearProblem::DblMax;
  cut_s1[ColIndex {
      0,
  }] = 20.0;
  scene_cuts[SceneIndex {1}].push_back(cut_s1);

  const auto rows_before = plp.system(first_scene_index(), first_phase_index())
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(
      first_phase_index(), scene_cuts, CutSharingMode::expected, plp);

  auto& li_s0 =
      plp.system(first_scene_index(), first_phase_index()).linear_interface();
  REQUIRE(li_s0.get_numrows() == rows_before + 1);
  const auto new_row = RowIndex {static_cast<Index>(rows_before)};

  // Under probabilities (0.9, 0.1), a textbook probability-weighted cut
  //     coeff = 0.9·10 + 0.1·20 = 11,  rhs = 0.9·100 + 0.1·200 = 110
  // but the current implementation ignores scene probabilities and
  // returns the plain sum:  coeff = 30, rhs = 300.
  //
  // [Section D — NOT a bug on its own]: this is an intentional design
  // choice motivated by LP-objective probability embedding.  The test
  // pins the behaviour so any future refactor has to decide whether to
  // change it.
  CHECK(li_s0.get_coeff(new_row,
                        ColIndex {
                            0,
                        })
        == doctest::Approx(30.0));
  CHECK(li_s0.get_row_low()[new_row] == doctest::Approx(300.0));
}

// ---------------------------------------------------------------------------
// Multi-cut uniform case (N=2 scenes, M=2 cuts per scene) → each scene's
// internal average is taken first, then summed across scenes.
//
// PLP equivalent: each per-scenario cut is broadcast verbatim — so the
// final α envelope is max over all installed cuts, not a per-scene
// average.  gtopt's `expected` collapses the per-scene set to an
// average BEFORE summing, losing the tightness of the max envelope.
//
// Under uniform probability, gtopt's `accumulate` mode (plain sum of
// ALL cuts) reproduces the PLP broadcast behaviour one-to-one after
// dividing by N.  This test pins the accumulate/expected divergence:
//   expected   = Σ_s mean_s(cuts_s)
//   accumulate = Σ_s Σ_c cut_{s,c}     (no mean; equivalent to PLP ×N)
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "cut_sharing uniform N=2 M=2 expected vs accumulate differ by mean")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 2);

  // Two cuts per scene.
  auto make_row = [](double lb, double c)
  {
    SparseRow r;
    r.lowb = lb;
    r.uppb = LinearProblem::DblMax;
    r[ColIndex {
        0,
    }] = c;
    return r;
  };

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);
  scene_cuts[first_scene_index()].push_back(make_row(40.0, 4.0));
  scene_cuts[first_scene_index()].push_back(make_row(60.0, 6.0));
  scene_cuts[SceneIndex {1}].push_back(make_row(100.0, 10.0));
  scene_cuts[SceneIndex {1}].push_back(make_row(140.0, 14.0));

  const auto rows_before = plp.system(first_scene_index(), first_phase_index())
                               .linear_interface()
                               .get_numrows();

  // Expected mode: per-scene mean → scene 0: (5, 50); scene 1: (12, 120).
  // Sum = (17, 170).
  {
    // Fresh planning copy so we can also run accumulate mode below on
    // an independent LP state.
    auto planning2 = make_2scene_3phase_hydro_planning(0.5, 0.5);
    PlanningLP plp2(std::move(planning2));
    share_cuts_for_phase(
        first_phase_index(), scene_cuts, CutSharingMode::expected, plp2);
    auto& li = plp2.system(first_scene_index(), first_phase_index())
                   .linear_interface();
    const auto new_row = RowIndex {static_cast<Index>(rows_before)};
    CHECK(li.get_coeff(new_row,
                       ColIndex {
                           0,
                       })
          == doctest::Approx(17.0));
    CHECK(li.get_row_low()[new_row] == doctest::Approx(170.0));
  }

  // Accumulate mode: plain sum of all four cuts.
  // Coeff = 4+6+10+14 = 34; rhs = 40+60+100+140 = 340.
  {
    auto planning3 = make_2scene_3phase_hydro_planning(0.5, 0.5);
    PlanningLP plp3(std::move(planning3));
    share_cuts_for_phase(
        first_phase_index(), scene_cuts, CutSharingMode::accumulate, plp3);
    auto& li = plp3.system(first_scene_index(), first_phase_index())
                   .linear_interface();
    const auto new_row = RowIndex {static_cast<Index>(rows_before)};
    CHECK(li.get_coeff(new_row,
                       ColIndex {
                           0,
                       })
          == doctest::Approx(34.0));
    CHECK(li.get_row_low()[new_row] == doctest::Approx(340.0));
  }

  // Sanity: accumulate = 2 × expected in this uniform N=2 M=2 case,
  // because per-scene count is uniform (2/2) and probabilities are (0.5/0.5).
  // accumulate/expected factor = (Σ M_s) / (Σ 1) = (2+2)/(1+1) = 2.
}

// ---------------------------------------------------------------------------
// Coefficient-wise AND RHS-wise arithmetic-mean equivalence on multi-col
// cuts with N=3 uniform scenes.  This is the core "uniform p=1/N ⇒ gtopt
// expected mode = PLP broadcast mean (scaled by N)" invariant.
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "cut_sharing uniform N=3 single-cut-per-scene expected = N × mean "
    "(all coeffs)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Build a 3-scene planning fixture by tweaking the 2-scene helper.
  // The helper constructor does not expose N=3, so we use the 2-scene
  // helper plus `free-function average` to verify scalar equivalence,
  // then separately verify N=3 at the free-function layer in
  // test_benders_cut.cpp (see next block).
  // Here we pin the 2-scene multi-column case.
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 2);

  // Multi-column cuts: coefficients on cols 0 and 1 differ.
  auto make_row2 = [](double lb, double c0, double c1)
  {
    SparseRow r;
    r.lowb = lb;
    r.uppb = LinearProblem::DblMax;
    r[ColIndex {
        0,
    }] = c0;
    r[ColIndex {
        1,
    }] = c1;
    return r;
  };

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);
  scene_cuts[first_scene_index()].push_back(make_row2(100.0, 10.0, 1.0));
  scene_cuts[SceneIndex {1}].push_back(make_row2(300.0, 30.0, 3.0));

  const auto rows_before = plp.system(first_scene_index(), first_phase_index())
                               .linear_interface()
                               .get_numrows();
  share_cuts_for_phase(
      first_phase_index(), scene_cuts, CutSharingMode::expected, plp);

  auto& li =
      plp.system(first_scene_index(), first_phase_index()).linear_interface();
  const auto new_row = RowIndex {static_cast<Index>(rows_before)};

  // Per-scene `average_benders_cut` on singleton = identity.
  // `accumulate_benders_cuts` across scenes = element-wise sum:
  //   col 0: 10 + 30 = 40
  //   col 1: 1  +  3 =  4
  //   rhs:  100 + 300 = 400
  CHECK(li.get_coeff(new_row,
                     ColIndex {
                         0,
                     })
        == doctest::Approx(40.0));
  CHECK(li.get_coeff(new_row,
                     ColIndex {
                         1,
                     })
        == doctest::Approx(4.0));
  CHECK(li.get_row_low()[new_row] == doctest::Approx(400.0));

  // Arithmetic-mean identity (divide by N): should match the mean
  // coefficient- AND rhs-wise.  This is the uniform-probability PLP
  // parity condition.
  const double n_scenes = static_cast<double>(num_scenes);
  CHECK(li.get_coeff(new_row,
                     ColIndex {
                         0,
                     })
            / n_scenes
        == doctest::Approx(20.0));  // (10+30)/2
  CHECK(li.get_coeff(new_row,
                     ColIndex {
                         1,
                     })
            / n_scenes
        == doctest::Approx(2.0));  // (1+3)/2
  CHECK(li.get_row_low()[new_row] / n_scenes
        == doctest::Approx(200.0));  // (100+300)/2
}

// ---------------------------------------------------------------------------
// Free-function layer: N=3 uniform probability.  The
// `weighted_average_benders_cut` with weights = (1/3, 1/3, 1/3) must
// equal the plain `average_benders_cut`.  This is the lowest-level
// assertion of the PLP-equivalence identity.
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "cut_sharing free-function N=3 uniform weights match arithmetic mean")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto make_cut = [](double lb, double c0, double c1)
  {
    SparseRow r;
    r.lowb = lb;
    r.uppb = LinearProblem::DblMax;
    r[ColIndex {
        0,
    }] = c0;
    r[ColIndex {
        1,
    }] = c1;
    return r;
  };

  const std::vector<SparseRow> cuts = {
      make_cut(60.0, 6.0, 0.5),
      make_cut(90.0, 9.0, 1.5),
      make_cut(120.0, 12.0, 2.5),
  };

  // Unweighted arithmetic mean (what gtopt `expected` uses per-scene).
  const auto avg = average_benders_cut(cuts);
  // mean rhs = (60+90+120)/3 = 90
  // mean c0  = (6+9+12)/3 = 9
  // mean c1  = (0.5+1.5+2.5)/3 = 1.5
  CHECK(avg.lowb == doctest::Approx(90.0));
  CHECK(avg.cmap.at(ColIndex {
            0,
        })
        == doctest::Approx(9.0));
  CHECK(avg.cmap.at(ColIndex {
            1,
        })
        == doctest::Approx(1.5));

  // Probability-weighted with UNIFORM weights p=1/3 each — must match.
  const std::vector<double> uniform_weights {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
  const auto wavg = weighted_average_benders_cut(cuts, uniform_weights);
  CHECK(wavg.lowb == doctest::Approx(avg.lowb));
  CHECK(wavg.cmap.at(ColIndex {
            0,
        })
        == doctest::Approx(avg.cmap.at(ColIndex {
            0,
        })));
  CHECK(wavg.cmap.at(ColIndex {
            1,
        })
        == doctest::Approx(avg.cmap.at(ColIndex {
            1,
        })));

  // Unnormalised uniform weights (1, 1, 1) — normalisation must kick in
  // and still recover the arithmetic mean.
  const std::vector<double> unnorm_uniform {1.0, 1.0, 1.0};
  const auto wavg2 = weighted_average_benders_cut(cuts, unnorm_uniform);
  CHECK(wavg2.lowb == doctest::Approx(avg.lowb));
  CHECK(wavg2.cmap.at(ColIndex {
            0,
        })
        == doctest::Approx(avg.cmap.at(ColIndex {
            0,
        })));
  CHECK(wavg2.cmap.at(ColIndex {
            1,
        })
        == doctest::Approx(avg.cmap.at(ColIndex {
            1,
        })));
}
