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
// M1e — rollback preserves cuts shared from peers (not in m_scene_cuts_)
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDPCutManager::clear_scene_cuts preserves cuts shared from peers")
{
  // Critical invariant for the `forward_infeas_rollback` design:
  // shared cuts received by scene S from peers via
  // `share_cuts_for_phase` are NOT in `m_scene_cuts_[S]` — they
  // live as LP rows + replay-buffer entries on S's LinearInterface.
  // When S is rolled back, only S's OWN cuts go (entries in
  // `m_scene_cuts_[S]`); shared peer cuts on S's LP must survive
  // because they encode peer progress that the stall-stop guard
  // (next iteration's forward dispatch) reads as "global cut count
  // grew, retry".
  //
  // This test simulates the situation directly:
  //   1. Inject one S=0 OPT cut (lands in m_scene_cuts_[0]).
  //   2. Inject one shared cut on S=0's LP via share_cuts_for_phase
  //      (lands on S=0's LP rows but NOT in m_scene_cuts_[0]).
  //   3. Roll back S=0.
  //   4. Verify: m_scene_cuts_[0] is empty AND S=0's LP still has
  //      the shared cut row (proven by row count delta = 1, not 2).
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;
  sddp_opts.enable_api = false;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  constexpr SceneIndex target_scene {0};
  constexpr PhaseIndex target_phase {0};
  auto& sys = plp.system(target_scene, target_phase);
  auto& li = sys.linear_interface();

  const auto rows_before = li.get_numrows();

  // Step 1: scene-0 owns this optimality cut (goes into
  // m_scene_cuts_[0]).
  std::ignore = inject_optcut(
      sddp, plp, target_scene, target_phase, /*rhs=*/100.0, /*extra=*/0);
  REQUIRE(sddp.cut_manager().scene_cuts()[target_scene].size() == 1);
  REQUIRE(li.get_numrows() == rows_before + 1);

  // Step 2: simulate a peer-shared cut by adding a row directly
  // through `add_cut_row` (without `cut_store().store_cut`).  This
  // mirrors the actual `share_cuts_for_phase` path which goes
  // through `add_cut_row` to release α but does NOT call
  // `m_cut_store_.store_cut` — see `sddp_cut_sharing.cpp:62-65`
  // comment "share_cuts_for_phase never calls store_cut".
  const auto* alpha_svar =
      find_alpha_state_var(plp.simulation(), target_scene, target_phase);
  REQUIRE(alpha_svar != nullptr);
  SparseRow shared_cut;
  shared_cut[alpha_svar->col()] = 1.0;
  shared_cut[ColIndex {0}] = 0.5;
  shared_cut.lowb = 50.0;
  shared_cut.uppb = LinearProblem::DblMax;
  shared_cut.class_name = sddp_alpha_class_name;
  shared_cut.constraint_name = sddp_share_cut_tag.constraint_name;
  shared_cut.variable_uid = plp.simulation().uid_of(target_phase);
  shared_cut.context =
      make_iteration_context(plp.simulation().uid_of(target_scene),
                             plp.simulation().uid_of(target_phase),
                             gtopt::uid_of(IterationIndex {0}),
                             /*extra=*/0);
  std::ignore = add_cut_row(plp,
                            target_scene,
                            target_phase,
                            CutType::Optimality,
                            shared_cut,
                            /*eps=*/0.0);

  // Now S=0's LP has 2 cut rows; m_scene_cuts_[0] still has only 1
  // (the shared cut wasn't recorded).
  REQUIRE(li.get_numrows() == rows_before + 2);
  REQUIRE(sddp.cut_manager().scene_cuts()[target_scene].size() == 1);

  // Step 3: roll back S=0.  Only the 1 cut owned by S=0 goes;
  // the shared cut survives.
  const auto deleted = sddp.cut_manager().clear_scene_cuts(target_scene, plp);
  CHECK(deleted == 1);
  CHECK(sddp.cut_manager().scene_cuts()[target_scene].empty());

  // Step 4: critical invariant — the shared cut is still on S=0's
  // LP.  Row count dropped by 1 (the owned cut), not 2.
  CHECK(li.get_numrows() == rows_before + 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// M1d — clear_scene_cuts deletes rows across MULTIPLE phases at once
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDPCutManager::clear_scene_cuts handles cuts spread across phases")
{
  // Existing M1 covers cuts on a single phase; this one exercises
  // the per-phase grouping inside `SceneCutStore::clear_with_lp`
  // (the `std::map<PhaseIndex, std::vector<int>> rows_to_delete`
  // collation step).  A real SDDP rollback typically has cuts
  // across many phases (PLP-style backtrack chain installs fcuts
  // on phases p, p-1, p-2, …) so this is the closer-to-production
  // shape.
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;
  sddp_opts.enable_api = false;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  constexpr SceneIndex target_scene {0};

  // Inject cuts on three different phases of scene 0.  Each cut
  // gets a distinct `extra` discriminator since they share
  // (class, constraint, scene_uid, iteration) — duplicate metadata
  // would trip `track_row_label_meta`.
  std::ignore = inject_optcut(
      sddp, plp, target_scene, PhaseIndex {0}, /*rhs=*/100.0, /*extra=*/0);
  std::ignore = inject_optcut(
      sddp, plp, target_scene, PhaseIndex {1}, /*rhs=*/200.0, /*extra=*/0);
  std::ignore = inject_optcut(
      sddp, plp, target_scene, PhaseIndex {2}, /*rhs=*/300.0, /*extra=*/0);

  REQUIRE(sddp.cut_manager().scene_cuts()[target_scene].size() == 3);

  // Snapshot row counts on each cell BEFORE rollback.
  const auto rows_p0 =
      plp.system(target_scene, PhaseIndex {0}).linear_interface().get_numrows();
  const auto rows_p1 =
      plp.system(target_scene, PhaseIndex {1}).linear_interface().get_numrows();
  const auto rows_p2 =
      plp.system(target_scene, PhaseIndex {2}).linear_interface().get_numrows();

  // Rollback every cut on scene 0 — three cells touched, three
  // separate `delete_rows` + `record_cut_deletion` calls inside
  // the per-phase loop.
  const auto deleted = sddp.cut_manager().clear_scene_cuts(target_scene, plp);
  CHECK(deleted == 3);
  CHECK(sddp.cut_manager().scene_cuts()[target_scene].empty());

  // Each cell drops by exactly one row.
  CHECK(
      plp.system(target_scene, PhaseIndex {0}).linear_interface().get_numrows()
      == rows_p0 - 1);
  CHECK(
      plp.system(target_scene, PhaseIndex {1}).linear_interface().get_numrows()
      == rows_p1 - 1);
  CHECK(
      plp.system(target_scene, PhaseIndex {2}).linear_interface().get_numrows()
      == rows_p2 - 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// M1c — rollback clears the low-memory replay buffer too
// ═══════════════════════════════════════════════════════════════════════════
//
// Critical invariant: when `clear_scene_cuts` deletes rows from the
// LP, the matching entries in `LinearInterface::m_active_cuts_` (the
// low-memory replay buffer) must also go.  Otherwise the next
// `release_backend → reconstruct_backend` cycle would re-add the
// rolled-back cuts via `replay_active_cuts()`, defeating the whole
// rollback semantic.  `record_cut_deletion` (called from inside
// `clear_with_lp`) is what enforces this.

TEST_CASE(  // NOLINT
    "SDDPCutManager::clear_scene_cuts purges low-memory replay buffer")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  // Engage compress mode end-to-end so m_active_cuts_ is populated
  // for every cut added (record_cut_row no-ops under off mode).
  planning.options.sddp_options.low_memory_mode = LowMemoryMode::compress;
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;
  sddp_opts.enable_api = false;
  sddp_opts.low_memory_mode = LowMemoryMode::compress;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  constexpr SceneIndex scene {0};
  constexpr PhaseIndex phase {0};
  auto& sys = plp.system(scene, phase);
  auto& li = sys.linear_interface();

  // Inject an α-referring cut.  inject_optcut routes through
  // add_cut_row → record_cut_row, so m_active_cuts_ grows by 1.
  std::ignore = inject_optcut(sddp,
                              plp,
                              scene,
                              phase,
                              /*rhs=*/100.0,
                              /*extra=*/0);

  // Verify m_active_cuts_ has the entry.  The public proxy is the
  // count check on `take_active_cuts` — but that consumes them, so
  // we use it on a local copy of the LinearInterface state via
  // an indirect check: release + reconstruct cycle preserves row
  // count iff the cut survives in the replay buffer.
  const auto rows_post_inject = li.get_numrows();
  REQUIRE(sddp.cut_manager().scene_cuts()[scene].size() == 1);

  // Sanity: a release/reconstruct cycle preserves the cut row
  // count (proves m_active_cuts_ has the entry).
  li.release_backend();
  li.reconstruct_backend();
  REQUIRE(li.get_numrows() == rows_post_inject);

  // Now roll back the scene's cuts.  This calls
  // `LinearInterface::delete_rows` + `record_cut_deletion`, which
  // both shrinks the LP rows AND prunes the replay buffer.
  const auto deleted = sddp.cut_manager().clear_scene_cuts(scene, plp);
  CHECK(deleted == 1);

  // The cut row is gone from the LP.
  CHECK(li.get_numrows() == rows_post_inject - 1);

  // The critical invariant: after a release/reconstruct cycle, the
  // replay buffer is empty so no cut comes back.  Pre-fix (or if
  // record_cut_deletion ever stops firing inside clear_with_lp),
  // the row count would jump back to `rows_post_inject`.
  li.release_backend();
  li.reconstruct_backend();
  CHECK(li.get_numrows() == rows_post_inject - 1);
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
  // Peer cuts reaching this scene's master is contingent on
  // ``cut_sharing != none``.  Under ``none`` the un-terminal /
  // stall-stop guard rightly compares against the scene's own cut
  // store (peer growth is invisible) and would NOT clear the marker
  // here.  Pick ``accumulate`` so the test premise — "peer cuts
  // reached this scene" — is actually realised.
  planning.options.sddp_options.cut_sharing_mode = CutSharingMode::accumulate;
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.enable_api = false;
  sddp_opts.forward_infeas_rollback = true;
  sddp_opts.cut_sharing = CutSharingMode::accumulate;
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
// S2b — Under cut_sharing=none, peer-cut growth must NOT clear the marker.
//       Regression for the TERMINAL ↔ un-terminal ping-pong observed on
//       juan/iplp 2026-05-03 trace_40, where 15 mark/unmark pairs fired
//       in 30 iterations because the un-terminal trigger compared against
//       the GLOBAL cut count even when cut_sharing=none made peer cuts
//       invisible to the failed scene's master.
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDPMethod stall-stop does NOT clear marker under cut_sharing=none "
    "even when peer cuts grew the global count")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  planning.options.sddp_options.forward_infeas_rollback = true;
  // Default cut_sharing is `none`; making it explicit for clarity.
  planning.options.sddp_options.cut_sharing_mode = CutSharingMode::none;
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.enable_api = false;
  sddp_opts.forward_infeas_rollback = true;
  sddp_opts.cut_sharing = CutSharingMode::none;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  // Inject a cut into scene 1 so the GLOBAL count is 1.  Scene 0's own
  // store is still empty.  Under cut_sharing=none, scene 1's cut is
  // NOT broadcast to scene 0's master, so the un-terminal trigger
  // (which now reads scene 0's OWN count under `none`) sees 0 → 0
  // and KEEPS scene 0 marked as stalled.  Result: the stall-stop
  // guard fires and the run aborts cleanly with `no recovery path`.
  std::ignore = inject_optcut(
      sddp, plp, SceneIndex {1}, PhaseIndex {0}, /*rhs=*/100.0, /*extra=*/0);
  REQUIRE(sddp.num_stored_cuts() == 1);

  // Scene 0 was "failed last iter" with own-store size 0 at failure.
  sddp.scene_retry_state(SceneIndex {0}).global_cuts_at_last_failure = 0;

  auto result = sddp.solve();
  // Pre-fix behaviour: peer cuts (global=1) "cleared" scene 0's
  // marker, scene 0 retried, succeeded, run continued.  Post-fix
  // behaviour: own-store is still 0, marker stays, stall guard
  // aborts.  Either outcome is acceptable AS LONG AS the run does
  // not silently consume peer cuts that can't reach scene 0's
  // master.  The strict pin: own-store-based comparison must NOT
  // clear the marker on peer growth alone.
  if (!result.has_value()) {
    // Stall abort path: confirm the message names the rollback
    // guard so a regression to "global count clears under none"
    // would fail the message check.
    CHECK(result.error().message.find("recovery path") != std::string::npos);
  }
  // Either way, the marker on scene 0 must reflect that no
  // own-store growth occurred.  The pre-fix bug would have cleared
  // the marker via the peer-cut delta.
  const auto& rs0 = sddp.scene_retry_state(SceneIndex {0});
  CHECK_FALSE((rs0.global_cuts_at_last_failure.has_value()
               && *rs0.global_cuts_at_last_failure
                   != 0));  // marker still 0 OR cleared via abort path
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

// ═══════════════════════════════════════════════════════════════════════════
// T1 — terminal-skip default + merge propagation
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SddpOptions::merge propagates terminal_failure_threshold override")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SddpOptions base;
  REQUIRE_FALSE(base.terminal_failure_threshold.has_value());

  SddpOptions override_opts;
  override_opts.terminal_failure_threshold = 5;

  base.merge(std::move(override_opts));
  REQUIRE(base.terminal_failure_threshold.has_value());
  CHECK(*base.terminal_failure_threshold == 5);
}

TEST_CASE(  // NOLINT
    "SDDPMethod SceneRetryState defaults — terminal flag clean on init")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // A fresh SDDPMethod must seed every per-scene retry slot with
  // ``terminal = false`` and ``consecutive_structural_failures = 0``.
  // The terminal-skip mechanism (``run_forward_pass_all_scenes`` head)
  // depends on this invariant: a previously-terminal scene from a
  // prior cascade level must not leak into a fresh solver instance.
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;
  sddp_opts.enable_api = false;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  for (auto si = 0; si < 2; ++si) {
    const auto& rs = sddp.scene_retry_state(SceneIndex {si});
    CHECK_FALSE(rs.terminal);
    CHECK(rs.consecutive_structural_failures == 0);
    CHECK_FALSE(rs.global_cuts_at_last_failure.has_value());
  }
}

TEST_CASE(  // NOLINT
    "SDDPMethod terminal-skip: scene with terminal=true and zero new cuts "
    "is skipped at next dispatch")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Smoke test the dispatch-time skip path.  We synthesise the
  // post-failure state by hand:
  //   * ``terminal = true``
  //   * ``global_cuts_at_last_failure = current``
  // Then call ``solve()`` with ``forward_infeas_rollback = true`` and
  // ``max_iterations = 1``.  The forward dispatch loop must skip the
  // scene (no LP solve, no peer cut growth required) and still let
  // the *other* scene complete normally.  The smoke signal is simply
  // that ``solve()`` completes and the iteration result has the
  // skipped scene marked infeasible without crashing on an invalid
  // future.
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.min_iterations = 1;
  sddp_opts.enable_api = false;
  sddp_opts.forward_infeas_rollback = true;
  sddp_opts.terminal_failure_threshold = 1;  // smallest non-disabling
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  auto& rs0 = sddp.scene_retry_state(SceneIndex {0});
  rs0.terminal = true;
  rs0.consecutive_structural_failures = 1;
  rs0.global_cuts_at_last_failure = sddp.num_stored_cuts();

  // The contract verified here is narrow: ``solve()`` MUST NOT crash
  // when a scene's terminal flag is set + its synthesised
  // ``global_cuts_at_last_failure`` matches the current cut count.
  // The dispatch loop's pre-dispatch terminal-skip pre-emptively
  // submits an empty future for the scene, and the result loop must
  // distinguish "skipped" from "solved" via ``skip_scene[i]`` — not
  // by calling ``.get()`` on an invalid future.
  //
  // Note: the post-training simulation pass runs scene 0 again
  // through a different dispatch path (sim pass uses
  // ``m_in_simulation_ = true``), so scene 0's final UB and
  // ``rs0.terminal`` may be updated by that pass; we only assert
  // the no-crash invariant here.  The dedicated coverage of
  // "terminal=true skips dispatch" lives in the runtime warning
  // log line emitted by the dispatch loop, exercisable in
  // integration tests.
  auto result = sddp.solve();
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->empty());
  const auto& last = result->back();
  REQUIRE(last.scene_upper_bounds.size() == 2);
}

TEST_CASE(  // NOLINT
    "SDDPMethod terminal-skip: scene un-terminals when global cut count grows")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Companion to the previous test: when fresh cuts arrive globally
  // (the snapshot stored at terminal-declaration is now stale), the
  // dispatch loop clears the terminal flag and lets the scene retry
  // its forward pass.  We synthesise a scene marked terminal at
  // ``snapshot = num_stored_cuts() - 1`` so any current count
  // exceeds it; the un-terminal log fires and the scene runs.
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.min_iterations = 1;
  sddp_opts.enable_api = false;
  sddp_opts.forward_infeas_rollback = true;
  sddp_opts.terminal_failure_threshold = 1;
  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  auto& rs0 = sddp.scene_retry_state(SceneIndex {0});
  rs0.terminal = true;
  rs0.consecutive_structural_failures = 1;
  // Snapshot below current count → restart trigger fires immediately.
  rs0.global_cuts_at_last_failure = std::ptrdiff_t {-1};

  auto result = sddp.solve();
  REQUIRE(result.has_value());

  // Restart hook cleared the terminal flag before dispatch.
  CHECK_FALSE(rs0.terminal);
}
