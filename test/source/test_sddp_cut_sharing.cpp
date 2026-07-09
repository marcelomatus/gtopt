/**
 * @file      test_sddp_cut_sharing.hpp
 * @brief     Unit tests for share_cuts_for_phase() free function
 * @date      2026-03-22
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/sddp_cut_sharing.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_types.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;

// ---------------------------------------------------------------------------
// share_cuts_for_phase — none mode (no-op)
// ---------------------------------------------------------------------------

TEST_CASE("share_cuts_for_phase none mode is a no-op")  // NOLINT
{
  using namespace gtopt;

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
  using namespace gtopt;

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

  // Even with multicut mode, single scene should not add cuts
  share_cuts_for_phase(
      first_phase_index(), scene_cuts, CutSharingMode::multicut, plp);

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
  using namespace gtopt;

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
      first_phase_index(), scene_cuts, CutSharingMode::multicut, plp);

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
  using namespace gtopt;

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
// multicut mode — pin: every scene cut is broadcast verbatim to every scene
// (the row content is untouched; only the label metadata is re-stamped per
// destination — validity comes from each cut referencing its own varphi_s,
// see docs/formulation/sddp-cut-validity.md §8).
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "share_cuts_for_phase multicut mode — characterization: "
    "each cut broadcast verbatim to every scene")
{
  using namespace gtopt;

  auto planning = make_2scene_3phase_hydro_planning(0.7, 0.3);
  PlanningLP plp(std::move(planning));

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 2);

  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);

  // Scene 0: two cuts; scene 1: one cut.  `multicut` adds all three to both
  // scenes in iteration order: first scene-0 cuts, then scene-1 cut.
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
      first_phase_index(), scene_cuts, CutSharingMode::multicut, plp);

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

// ---------------------------------------------------------------------------
// Free-function layer: N=3 uniform probability.  The
// `weighted_average_benders_cut` with uniform weights must reproduce the
// arithmetic mean of the cuts (the PLP-equivalence identity).  The former
// `average_benders_cut` combinator was removed 2026-07-08 with the
// `broadcast_mean` sharing mode, so the mean here is computed by hand.
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "cut_sharing free-function N=3 uniform weights match arithmetic mean")
{
  using namespace gtopt;

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

  // Hand-computed arithmetic mean:
  //   mean rhs = (60+90+120)/3 = 90
  //   mean c0  = (6+9+12)/3 = 9
  //   mean c1  = (0.5+1.5+2.5)/3 = 1.5

  // Probability-weighted with UNIFORM weights p=1/3 each — must match.
  const std::vector<double> uniform_weights {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
  const auto wavg = weighted_average_benders_cut(cuts, uniform_weights);
  CHECK(wavg.lowb == doctest::Approx(90.0));
  CHECK(wavg.cmap.at(ColIndex {
            0,
        })
        == doctest::Approx(9.0));
  CHECK(wavg.cmap.at(ColIndex {
            1,
        })
        == doctest::Approx(1.5));

  // Unnormalised uniform weights (1, 1, 1) — normalisation must kick in
  // and still recover the arithmetic mean.
  const std::vector<double> unnorm_uniform {1.0, 1.0, 1.0};
  const auto wavg2 = weighted_average_benders_cut(cuts, unnorm_uniform);
  CHECK(wavg2.lowb == doctest::Approx(90.0));
  CHECK(wavg2.cmap.at(ColIndex {
            0,
        })
        == doctest::Approx(9.0));
  CHECK(wavg2.cmap.at(ColIndex {
            1,
        })
        == doctest::Approx(1.5));
}

// ---------------------------------------------------------------------------
// share_cuts_for_phase routes through `add_cut_row` so optimality cuts that
// reference α release the bootstrap pin (bound_alpha_for_cut).
//
// Regression for juan/gtopt_iplp iter i1 p1 infeasibility (2026-04-30):
// every scene declared "no predecessor phase to cut on" because
// `sddp_share_m1_*` cuts demanded α ≈ 1.16e8 against a pinned α = 0.
// Root cause: `share_cuts_for_phase` used the raw `add_row + record_cut_row`
// pair instead of the unified `add_cut_row` free function, so
// `bound_alpha_for_cut` never fired and α stayed at `lowb = uppb = 0`.
// ---------------------------------------------------------------------------

namespace
{

/// Threshold below which a lower bound is considered "effectively −∞".
/// Mirrors `test_sddp_alpha_relax.cpp:53-54` — solver backends normalise
/// gtopt's `LinearProblem::DblMax` sentinel (1.8e308) to their own
/// infinity representation on read-back (CPLEX -1e20, HiGHS -1e30, CBC
/// -1e30); any raw value past ±1e15 confirms α is unbounded in that
/// direction.
constexpr double kEffectivelyMinusInf = -1e15;
constexpr double kEffectivelyPlusInf = 1e15;

/// Read α's raw LP bounds at (scene, phase) — `std::nullopt` if α is
/// not registered.  Same shape as the helper in test_sddp_alpha_relax.
auto alpha_bounds_raw(const PlanningLP& plp,
                      SceneIndex scene_index,
                      PhaseIndex phase_index)
    -> std::optional<std::pair<double, double>>
{
  const auto* svar =
      find_alpha_state_var(plp.simulation(), scene_index, phase_index);
  if (svar == nullptr) {
    return std::nullopt;
  }
  const auto& li = plp.system(scene_index, phase_index).linear_interface();
  return std::pair {
      li.get_col_low_raw()[svar->col()],
      li.get_col_upp_raw()[svar->col()],
  };
}

/// Build an optimality-shaped Benders cut that references α at
/// (scene, phase) with coefficient 1 and a single state-var coefficient
/// on a structural reservoir column.  RHS is `>= rhs` (α + state·coeff
/// >= rhs).  Mirrors the `sddp_share_m1_*` shape observed in juan's
/// error LPs.
SparseRow make_alpha_referring_cut(const PlanningLP& plp,
                                   SceneIndex scene_index,
                                   PhaseIndex phase_index,
                                   double state_coeff,
                                   double rhs)
{
  const auto* svar =
      find_alpha_state_var(plp.simulation(), scene_index, phase_index);
  REQUIRE(svar != nullptr);
  SparseRow cut;
  cut[svar->col()] = 1.0;
  // Use col 0 as a stand-in for a reservoir energy state column.  Any
  // structural col works; the cut just needs to be a well-formed
  // Benders shape (linear in state + α >= rhs).
  cut[ColIndex {0}] = state_coeff;
  cut.lowb = rhs;
  cut.uppb = LinearProblem::DblMax;
  return cut;
}

}  // namespace

TEST_CASE(  // NOLINT
    "share_cuts_for_phase multicut — optimality cut releases α^phase")
{
  using namespace gtopt;

  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  PlanningLP plp(std::move(planning));

  // Drive SDDP init so α is registered + pinned at lowb=uppb=0 on
  // every (scene, phase).  max_iterations=0 skips the solve loop.
  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;
  sddp_opts.enable_api = false;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  constexpr PhaseIndex target_phase {0};
  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());

  // Pre-condition: α is bootstrap-pinned at every scene of target_phase.
  for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
    const auto bounds = alpha_bounds_raw(plp, si, target_phase);
    REQUIRE(bounds.has_value());
    CHECK(bounds->first == doctest::Approx(0.0));
    CHECK(bounds->second == doctest::Approx(0.0));
  }

  // Build an α-referring cut per scene and feed share_cuts_for_phase
  // in `multicut` mode (which broadcasts every cut to every scene's
  // LP).
  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);
  for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
    scene_cuts[si].push_back(
        make_alpha_referring_cut(plp, si, target_phase, 0.5, 100.0));
  }

  share_cuts_for_phase(target_phase, scene_cuts, CutSharingMode::multicut, plp);

  // Post-condition: α at target_phase is freed on every scene.
  for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
    CAPTURE(si);
    const auto bounds = alpha_bounds_raw(plp, si, target_phase);
    REQUIRE(bounds.has_value());
    CHECK(bounds->first == doctest::Approx(0.0));
    CHECK(bounds->second > kEffectivelyPlusInf);
  }
}

TEST_CASE(  // NOLINT
    "share_cuts_for_phase — cut without α reference leaves bootstrap intact")
{
  // Negative control: `bound_alpha_for_cut` is gated on the cut row
  // actually referencing α (`cut.cmap.contains(alpha_svar->col())`),
  // so a shared cut on pure state coefficients must NOT release the
  // pin.  This pins the gate so a future regression that flips the
  // policy ("always free on share") would surface here.
  using namespace gtopt;
  // NOLINTBEGIN(bugprone-unchecked-optional-access,
  // hicpp-use-auto,modernize-use-auto)

  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;
  sddp_opts.enable_api = false;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  constexpr PhaseIndex target_phase {0};
  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());

  // No α col in the cut.
  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);
  for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
    SparseRow cut;
    cut[ColIndex {0}] = 1.0;  // pure state coefficient, no α
    cut.lowb = 10.0;
    cut.uppb = LinearProblem::DblMax;
    scene_cuts[si].push_back(cut);
  }

  share_cuts_for_phase(target_phase, scene_cuts, CutSharingMode::multicut, plp);

  // α must stay pinned at the bootstrap (lowb = uppb = 0).
  for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
    CAPTURE(si);
    const auto bounds = alpha_bounds_raw(plp, si, target_phase);
    REQUIRE(bounds.has_value());
    CHECK(bounds->first == doctest::Approx(0.0));
    CHECK(bounds->second == doctest::Approx(0.0));
  }
}

// ---------------------------------------------------------------------------
// alpha_cols_on_cell — N-α (multicut) vs single-α registration
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "alpha_cols_on_cell — multicut registers N future-cost columns")
{
  using namespace gtopt;

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;  // register α, skip the solve loop
  sddp_opts.enable_api = false;
  sddp_opts.cut_sharing = CutSharingMode::multicut;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  const auto& sim = plp.simulation();
  constexpr PhaseIndex target_phase {0};  // intermediate → N varphi columns

  const auto cols = alpha_cols_on_cell(sim, first_scene_index(), target_phase);
  // 2 scenes under multicut → 2 dedicated varphi columns on an intermediate
  // phase, with contiguous uids from sddp_alpha_uid and distinct LP columns.
  REQUIRE(cols.size() == 2);
  CHECK(cols[0].first != cols[1].first);
  CHECK(cols[0].second == static_cast<Uid>(sddp_alpha_uid));
  CHECK(cols[1].second == static_cast<Uid>(sddp_alpha_uid + 1));

  // The source-scene overload resolves each varphi_s; the legacy single-α
  // lookup resolves to varphi_0 (offset 0).
  const auto* v1 = find_alpha_state_var(
      sim, first_scene_index(), target_phase, SceneIndex {1});
  REQUIRE(v1 != nullptr);
  CHECK(v1->col() == cols[1].first);
  const auto* legacy =
      find_alpha_state_var(sim, first_scene_index(), target_phase);
  CHECK((legacy != nullptr && legacy->col() == cols[0].first));
}

TEST_CASE(  // NOLINT
    "alpha_cols_on_cell — single-α modes register one future-cost column")
{
  using namespace gtopt;

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;
  sddp_opts.enable_api = false;
  sddp_opts.cut_sharing = CutSharingMode::none;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  const auto& sim = plp.simulation();
  constexpr PhaseIndex target_phase {0};

  const auto cols = alpha_cols_on_cell(sim, first_scene_index(), target_phase);
  REQUIRE(cols.size() == 1);
  CHECK(cols[0].second == static_cast<Uid>(sddp_alpha_uid));
  // No varphi_1 exists in single-α mode.
  CHECK(find_alpha_state_var(
            sim, first_scene_index(), target_phase, SceneIndex {1})
        == nullptr);
}

// ---------------------------------------------------------------------------
// share_cuts_for_phase — multicut broadcasts all cuts to all scenes
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "share_cuts_for_phase multicut broadcasts every scene's cut to all scenes")
{
  using namespace gtopt;

  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;
  sddp_opts.enable_api = false;
  sddp_opts.cut_sharing = CutSharingMode::multicut;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  constexpr PhaseIndex target_phase {0};
  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  REQUIRE(num_scenes == 2);

  // Scene 0's cut references varphi_0; scene 1's references varphi_1 — the
  // backward-pass retarget the loader mirrors.  share_cuts_for_phase then
  // broadcasts both to every scene-LP (mechanically like `max`, valid here
  // because each cut pins its own dedicated varphi_s).
  StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
  scene_cuts.resize(num_scenes);
  for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
    const auto* vs = find_alpha_state_var(
        plp.simulation(), si, target_phase, si);  // varphi_si
    REQUIRE(vs != nullptr);
    SparseRow cut;
    cut[vs->col()] = 1.0;
    cut[ColIndex {0}] = 0.5;  // a structural state coefficient
    cut.lowb = 5.0 + static_cast<double>(static_cast<std::size_t>(si));
    cut.uppb = LinearProblem::DblMax;
    scene_cuts[si].push_back(cut);
  }

  const auto rows_before = plp.system(first_scene_index(), target_phase)
                               .linear_interface()
                               .get_numrows();

  share_cuts_for_phase(target_phase, scene_cuts, CutSharingMode::multicut, plp);

  // Both cuts land on every scene-LP (2 cuts → +2 rows per scene).
  for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
    CAPTURE(si);
    const auto rows_after =
        plp.system(si, target_phase).linear_interface().get_numrows();
    CHECK(rows_after == rows_before + 2);
  }
}

// NOLINTEND(bugprone-unchecked-optional-access,
// hicpp-use-auto,modernize-use-auto)
