// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_forward_infeas_rollback.cpp
 * @brief     Tests for `SDDPOptions::forward_infeas_rollback` and the
 *            `SDDPCutManager::clear_scene_cuts` rollback primitive.
 * @date      2026-04-30
 *
 * Coverage axes (per
 * `support/scene_infeasibility_rollback_plan_2026-04-30.md` §4):
 *
 *   M1: `clear_scene_cuts` deletes every row scene S has stored in
 *       `m_scene_cuts_[S]` from the matching LP cells and empties the
 *       per-scene vector.  Other scenes' cuts and shared cuts (which
 *       live only on the LP, not in m_scene_cuts_) are unaffected.
 *   M2: `clear_scene_cuts` on an empty scene is a no-op.
 *   M3: option plumbing: `forward_infeas_rollback` defaults to false
 *       and round-trips through `PlanningOptionsLP`.
 *   M4: `m_scene_retry_state_` is reset to fresh state on every
 *       `ensure_initialized()` call (idempotent across cascade levels
 *       / repeated solve invocations).
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_types.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Add a synthetic optimality cut on (scene, phase) referencing α
/// with coefficient 1 and column 0 with coefficient 0.5 (mirrors the
/// shape of a real Benders cut).  Routes through the unified
/// `add_cut_row` free function so `m_active_cuts_` and α-release
/// bookkeeping stay consistent, then persists to `SDDPCutManager`
/// via the public `cut_manager()` accessor so the rollback primitive
/// has something to delete.
[[nodiscard]] RowIndex inject_optcut(SDDPMethod& sddp,
                                     PlanningLP& plp,
                                     SceneIndex scene_index,
                                     PhaseIndex phase_index,
                                     double rhs,
                                     int extra = 0)
{
  const auto* svar =
      find_alpha_state_var(plp.simulation(), scene_index, phase_index);
  REQUIRE(svar != nullptr);

  const auto scene_uid = plp.simulation().uid_of(scene_index);
  const auto phase_uid = plp.simulation().uid_of(phase_index);

  SparseRow cut;
  cut[svar->col()] = 1.0;
  cut[ColIndex {0}] = 0.5;
  cut.lowb = rhs;
  cut.uppb = LinearProblem::DblMax;
  cut.class_name = sddp_alpha_class_name;
  cut.constraint_name = sddp_scut_tag.constraint_name;
  cut.variable_uid = phase_uid;
  cut.context = make_iteration_context(
      scene_uid, phase_uid, gtopt::uid_of(IterationIndex {0}), extra);

  const auto row = add_cut_row(
      plp, scene_index, phase_index, CutType::Optimality, cut, /*eps=*/0.0);
  sddp.cut_manager().store_cut(scene_index,
                               phase_index,
                               cut,
                               CutType::Optimality,
                               row,
                               scene_uid,
                               phase_uid);
  return row;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// M1 — clear_scene_cuts deletes scene's rows and clears its vector
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDPCutManager::clear_scene_cuts deletes rows and clears scene's vector")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;
  sddp_opts.enable_api = false;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  constexpr SceneIndex target_scene {0};
  constexpr PhaseIndex target_phase {0};

  // Snapshot LP row count before any cuts on the target cell.
  auto& sys = plp.system(target_scene, target_phase);
  const auto rows_before = sys.linear_interface().get_numrows();

  // Inject two synthetic optimality cuts on (target_scene, target_phase).
  // `extra` discriminates the two cuts so the metadata-uniqueness
  // check in `track_row_label_meta` (class+cons+uid+context) passes.
  std::ignore = inject_optcut(
      sddp, plp, target_scene, target_phase, /*rhs=*/100.0, /*extra=*/0);
  std::ignore = inject_optcut(
      sddp, plp, target_scene, target_phase, /*rhs=*/200.0, /*extra=*/1);

  REQUIRE(sys.linear_interface().get_numrows() == rows_before + 2);
  REQUIRE(sddp.cut_manager().scene_cuts()[target_scene].size() == 2);

  // Roll back every cut on the target scene.
  const auto deleted = sddp.cut_manager().clear_scene_cuts(target_scene, plp);

  CHECK(deleted == 2);
  CHECK(sddp.cut_manager().scene_cuts()[target_scene].empty());
  CHECK(sys.linear_interface().get_numrows() == rows_before);
}

// ═══════════════════════════════════════════════════════════════════════════
// M2 — clear_scene_cuts on an empty scene is a no-op
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDPCutManager::clear_scene_cuts on empty scene_cuts is a no-op")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.7, 0.3);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;
  sddp_opts.enable_api = false;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  REQUIRE(sddp.cut_manager().scene_cuts()[SceneIndex {0}].empty());

  // Capture LP row count for both scenes' phase 0 cells before the
  // no-op clear.
  const auto rows_s0 = plp.system(SceneIndex {0}, PhaseIndex {0})
                           .linear_interface()
                           .get_numrows();
  const auto rows_s1 = plp.system(SceneIndex {1}, PhaseIndex {0})
                           .linear_interface()
                           .get_numrows();

  const auto deleted = sddp.cut_manager().clear_scene_cuts(SceneIndex {0}, plp);

  CHECK(deleted == 0);
  CHECK(plp.system(SceneIndex {0}, PhaseIndex {0})
            .linear_interface()
            .get_numrows()
        == rows_s0);
  CHECK(plp.system(SceneIndex {1}, PhaseIndex {0})
            .linear_interface()
            .get_numrows()
        == rows_s1);
}

// ═══════════════════════════════════════════════════════════════════════════
// M1b — clear_scene_cuts on scene 0 leaves scene 1's cuts untouched
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDPCutManager::clear_scene_cuts is independent across scenes")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;
  sddp_opts.enable_api = false;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  constexpr PhaseIndex target_phase {0};

  // Inject cuts on both scenes' (scene, target_phase) cells.  Scene 1
  // gets two cuts so we test that scene 0's rollback doesn't disturb
  // either of them.  `extra` is per-(scene, phase) unique.
  std::ignore = inject_optcut(
      sddp, plp, SceneIndex {0}, target_phase, /*rhs=*/50.0, /*extra=*/0);
  std::ignore = inject_optcut(
      sddp, plp, SceneIndex {1}, target_phase, /*rhs=*/75.0, /*extra=*/0);
  std::ignore = inject_optcut(
      sddp, plp, SceneIndex {1}, target_phase, /*rhs=*/90.0, /*extra=*/1);

  REQUIRE(sddp.cut_manager().scene_cuts()[SceneIndex {0}].size() == 1);
  REQUIRE(sddp.cut_manager().scene_cuts()[SceneIndex {1}].size() == 2);

  const auto rows_s1_before =
      plp.system(SceneIndex {1}, target_phase).linear_interface().get_numrows();

  // Clear scene 0.  Scene 1's cuts must survive — both vector size
  // and LP row count.
  std::ignore = sddp.cut_manager().clear_scene_cuts(SceneIndex {0}, plp);

  CHECK(sddp.cut_manager().scene_cuts()[SceneIndex {0}].empty());
  CHECK(sddp.cut_manager().scene_cuts()[SceneIndex {1}].size() == 2);
  CHECK(
      plp.system(SceneIndex {1}, target_phase).linear_interface().get_numrows()
      == rows_s1_before);
}

// ═══════════════════════════════════════════════════════════════════════════
// M3 — option plumbing: default false + round-trip via PlanningOptionsLP
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "PlanningOptionsLP::sddp_forward_infeas_rollback defaults to true")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  // Default flipped 2026-04-30 (plan step 6).
  // planning.options.sddp_options.forward_infeas_rollback is
  // std::nullopt → resolves to default_sddp_forward_infeas_rollback
  // (true).
  PlanningLP plp(std::move(planning));
  CHECK(plp.options().sddp_forward_infeas_rollback());
}

TEST_CASE(  // NOLINT
    "PlanningOptionsLP::sddp_forward_infeas_rollback respects explicit false")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  planning.options.sddp_options.forward_infeas_rollback = false;
  PlanningLP plp(std::move(planning));
  CHECK_FALSE(plp.options().sddp_forward_infeas_rollback());
}

// ═══════════════════════════════════════════════════════════════════════════
// M3b — SDDPOptions::merge propagates forward_infeas_rollback
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SddpOptions::merge propagates forward_infeas_rollback override")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SddpOptions base;
  REQUIRE_FALSE(base.forward_infeas_rollback.has_value());

  SddpOptions override_opts;
  override_opts.forward_infeas_rollback = true;

  base.merge(std::move(override_opts));
  REQUIRE(base.forward_infeas_rollback.has_value());
  CHECK(*base.forward_infeas_rollback);
}

// ═══════════════════════════════════════════════════════════════════════════
// M4 — ensure_initialized resets per-scene retry state on each fresh call
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDPMethod ensure_initialized resets m_scene_retry_state_ on construction")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;
  sddp_opts.enable_api = false;
  sddp_opts.forward_infeas_rollback = true;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  // After fresh init, every scene's retry-state slot is empty
  // (no prior failure).  We can't observe the private vector
  // directly, but the public invariant we verify is: a no-fail
  // sequence of forward dispatches must NOT trigger the stall-stop
  // guard, regardless of how many iterations run.  Stall-stop fires
  // only when at least one scene previously failed, so a clean
  // init must leave the guard quiescent.
  //
  // The implication: after `ensure_initialized`, the next
  // `run_forward_pass_all_scenes` call must succeed structurally
  // (modulo the actual LP outcome).  Drive a single forward pass
  // using a no-op work pool spec — the existing fixtures use a
  // small in-memory LP so plain CPLEX/HiGHS solves complete in
  // milliseconds.
  //
  // We invoke the public `solve()` with max_iterations=0 to confirm
  // the guard never fires when no failure has been recorded.
  auto result = sddp.solve();
  REQUIRE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// S1 — stall-stop guard fires when previously-failed scene sees no progress
// ═══════════════════════════════════════════════════════════════════════════
//
// Synthesise the "scene 0 failed at the previous iteration" state via
// the test-friendly public `scene_retry_state(s)` accessor instead of
// forcing a real LP infeasibility (the latter requires a dedicated
// fixture that's deferred).  Setting the snapshot equal to the
// current global cut count guarantees the next forward dispatch sees
// `current_global_cuts > snapshot` as false → the scene is "stalled"
// → the guard returns `Error{SolverError, "no recovery path"}`.

TEST_CASE(  // NOLINT
    "SDDPMethod stall-stop fires when failed scene sees no new cuts")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  planning.options.sddp_options.forward_infeas_rollback = true;
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.enable_api = false;
  sddp_opts.forward_infeas_rollback = true;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  // Synthesise: scene 0 failed last iteration, snapshot of global cuts
  // taken at the moment of failure equals the current count.  Scene 1
  // has not failed.  Next forward dispatch must see "0 stalled scenes"
  // would be wrong — `failed_last == 1` and `stalled == 1`, so the
  // guard fires.
  const auto current_cuts = sddp.num_stored_cuts();
  sddp.scene_retry_state(SceneIndex {0}).global_cuts_at_last_failure =
      current_cuts;

  auto result = sddp.solve();
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().code == ErrorCode::SolverError);
  CHECK(result.error().message.find("no recovery path") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// S2 — stall-stop guard clears the failure marker when progress detected
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDPMethod stall-stop clears marker when peer cuts arrived")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  planning.options.sddp_options.forward_infeas_rollback = true;
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.enable_api = false;
  sddp_opts.forward_infeas_rollback = true;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  // Synthesise: scene 0 failed *before* any cuts existed (snapshot=0),
  // then scene 1 contributed a cut (current=1).  At forward dispatch
  // the guard sees `current(1) > snapshot(0)`, clears scene 0's marker,
  // and the run continues.
  std::ignore = inject_optcut(
      sddp, plp, SceneIndex {1}, PhaseIndex {0}, /*rhs=*/100.0, /*extra=*/0);
  REQUIRE(sddp.num_stored_cuts() == 1);

  sddp.scene_retry_state(SceneIndex {0}).global_cuts_at_last_failure = 0;

  auto result = sddp.solve();
  REQUIRE(result.has_value());

  // The guard cleared the marker — scene 0's snapshot is empty.
  CHECK_FALSE(sddp.scene_retry_state(SceneIndex {0})
                  .global_cuts_at_last_failure.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// S3 — default-OFF preserves legacy behaviour (guard never fires)
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDPMethod stall-stop is gated on forward_infeas_rollback option")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  // Flag deliberately omitted — defaults to false.
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.enable_api = false;
  // sddp_opts.forward_infeas_rollback intentionally left at default false.
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  // Even if a snapshot is somehow present (e.g. from a previous
  // forward_infeas_rollback=true cascade level), the guard must
  // respect the *current* option value.  The synthetic state below
  // would trip the guard if the flag were on; with the flag off the
  // run completes normally.
  sddp.scene_retry_state(SceneIndex {0}).global_cuts_at_last_failure =
      sddp.num_stored_cuts();

  auto result = sddp.solve();
  REQUIRE(result.has_value());
}
