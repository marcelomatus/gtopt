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
