/**
 * @file      sddp_cut_sharing.cpp
 * @brief     SDDP cut sharing across scenes — implementation
 * @date      2026-03-22
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Only the `multicut` broadcast survives here: the legacy `accumulate`,
 * `broadcast_mean` (alias `expected`), and `max` branches were REMOVED
 * 2026-07-08 — all three broadcast onto the destination scene's own α
 * and are mathematically invalid for distinct sample paths (verdicts in
 * `docs/formulation/sddp-cut-validity.md` §7; history in
 * `docs/analysis/investigations/sddp/sddp_cut_sharing_fix_plan_2026-04-30.md`
 * and git).
 *
 * `markov` (2026-07-08, opt-in) reuses the same broadcast mechanics
 * unchanged: the destination column is baked into the cut row (scene
 * S's cut references `varphi_{m(S)}` — the Markov-state column — set up
 * by the backward-pass retarget), so the per-destination install below
 * needs no mode-specific routing.  See
 * `docs/formulation/sddp-markov.md` §6.
 */

#include <utility>

#include <gtopt/benders_cut.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_sharing.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

void share_cuts_for_phase(
    PhaseIndex phase_index,
    const StrongIndexVector<SceneIndex, std::vector<SparseRow>>& scene_cuts,
    CutSharingMode mode,
    PlanningLP& planning,
    IterationIndex iteration_index)
{
  const auto num_scenes = planning.simulation().scene_count();

  if (num_scenes <= 1 || mode == CutSharingMode::none) {
    return;
  }

  const auto& sim = planning.simulation();
  const auto phase_uid = sim.uid_of(phase_index);

  // Stamp ``row`` with class_name/constraint_name and a per-scene
  // context built from the destination scene's UID and a per-cut
  // ``extra`` discriminator.  This is intentionally per-scene
  // (called inside the destination loop) because the same row is
  // replicated across scenes, and each destination LP needs a label
  // that is unique within ITS OWN row table.  The ``extra`` field
  // disambiguates cuts that share (scene, phase, iter) — required
  // because the multicut broadcast installs every source cut on every
  // scene-LP, so a single iteration lands N cuts that would otherwise
  // collide on metadata.  Using SceneUid{} (= unknown_uid) would
  // collide on the first cut shared this iteration — see
  // ``make_iteration_context``'s precondition checks.
  //
  // Hot-start / cascade safety: this ``extra`` only travels through
  // the in-memory LP row label and the ``m_active_cuts_`` low-memory
  // replay buffer (``LinearInterface::record_cut_row``).  Saved cut
  // files (``save_cuts_parquet``) iterate
  // ``SDDPCutManager::m_scene_cuts_``, which is populated exclusively
  // by ``SDDPCutManager::store_cut`` (backward-pass optimality and
  // feasibility cuts).  ``share_cuts_for_phase`` never calls
  // ``store_cut``, so the broadcast rows minted here — and their
  // ``extra`` discriminators — are never persisted to the cut file.
  //
  // Cascade level handoff (``cascade_method.cpp``): each level
  // saves only ``current_solver->stored_cuts()`` (=
  // ``m_cut_store_.build_combined_cuts``), then drops the LP +
  // solver + ``m_active_cuts_`` buffer before the next level
  // allocates a fresh LP.  Level N+1 rebuilds its own broadcast
  // share rows from scratch during its iterations, under a
  // disjoint ``iteration_uid`` range (seeded via
  // ``iteration_offset_hint = global_iter_index``), so per-cut
  // ``extra=0..N-1`` cannot collide across levels.  Each
  // iteration's call stamps a fresh ``iteration_uid``, so two
  // iterations can both use ``extra=0..N-1`` without collision.
  const auto stamp_for_scene =
      [&](SparseRow& row, SceneIndex scene_index, int extra)
  {
    sddp_share_cut_tag.apply_to(row);
    row.context = make_iteration_context(
        sim.uid_of(scene_index), phase_uid, uid_of(iteration_index), extra);
  };

  // **`multicut` broadcast** (PLP-faithful) — scene S's backward cut
  // references S's OWN dedicated column `varphi_S` (set up by the
  // backward-pass retarget in `sddp_method_iteration.cpp`), and EVERY
  // scene-LP carries the full set `varphi_0..N-1` priced 1/N.
  // Broadcasting scene S's cut onto scene D's LP therefore forces
  //   varphi_S_D ≥ Q_S*(x_S_trial)
  // i.e. `varphi_S` in EVERY LP is bounded ONLY by scenario-S's cuts —
  // no cut ever over-tightens another scene's own bound.  Under
  // UNIFORM scene probabilities the resulting recursion is the Bellman
  // recursion of the stagewise-RESAMPLED process (scene data redrawn
  // at every phase boundary), for which the LB is valid (theorem M1);
  // the objective's Σ_s (1/N)·varphi_s is that process's expected
  // cost-to-go — NOT a general `E[Q]` identity, and NOT certified for
  // non-uniform probabilities (theorem M3).  See
  // `docs/formulation/sddp-cut-validity.md` §8.  PLP parity:
  // `plp-agrespd.f:94` IColx = NCol-NSimul+ISimul source indexing,
  // `defprbpd.f:810` 1/N averaging.  `bound_alpha_for_cut` (called per
  // cut below) releases whichever `varphi_s` the cut references.
  //
  // Each cut already carries its source-scene metadata from
  // ``apply_cut_sharing_for_iteration::to_sparse_row``, so we
  // stamp here only to overwrite with the DESTINATION scene's
  // context (each scene's LP needs a unique-within-LP label;
  // the source scene's metadata is fine cross-scene but breaks
  // the duplicate-label invariant within one LP when the same
  // cut is appended multiple times for the broadcast).
  std::vector<SparseRow> all_cuts;
  {
    size_t total = 0;
    for (const auto& cuts : scene_cuts) {
      total += cuts.size();
    }
    all_cuts.reserve(total);
  }
  for (auto&& [si, cuts] : enumerate<SceneIndex>(scene_cuts)) {
    all_cuts.insert(all_cuts.end(), cuts.begin(), cuts.end());
  }

  if (all_cuts.empty()) {
    return;
  }

  for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
    // Bulk-install path — the broadcast lands ALL `all_cuts.size()`
    // cuts onto every scene's LP, so the inner loop below is a
    // genuine within-cell bulk opportunity (multiple cuts landing on
    // ONE LinearInterface).  Replaces the per-cut `add_cut_row` loop
    // with: stamp + α-release pass, single `add_rows` dispatch, then
    // per-cut `record_cut_row` for low-memory replay.  Saves
    // N-1 backend round-trips per destination scene.
    //
    // `bound_alpha_for_cut` is idempotent across the batch (it's just
    // a `set_col_low_raw / set_col_upp_raw` to ±DblMax on the same α
    // column, mirrored into `m_dynamic_cols_`).  The early-out on
    // `cut.cmap.contains(α_col)` short-circuits cuts that don't
    // reference α — same gating as the per-cut path.
    //
    // ``extra`` is the per-cut counter within this destination
    // scene's broadcast — unique within (scene, phase, iter) so
    // the metadata invariant holds even when N source cuts land
    // on the same LP.  `iota_range` keeps the index strongly
    // typed (no raw int loop counter).
    std::vector<SparseRow> stamped_cuts;
    stamped_cuts.reserve(all_cuts.size());
    for (auto&& [extra, src_cut] : enumerate<int>(all_cuts)) {
      auto cut = src_cut;  // copy: per-scene/per-cut stamp below
      stamp_for_scene(cut, scene_index, extra);
      stamped_cuts.push_back(std::move(cut));
    }

    // Step 1: release α (only fires for cuts that reference α; the
    // call is idempotent across cuts, so a redundant release is a
    // cheap no-op).  Backward-pass optimality cuts that demand α > 0
    // require this release; a raw `add_row + record_cut_row` pair
    // would skip it and leave α frozen at `lowb = uppb = 0`, observed
    // on juan/gtopt_iplp iter i1 p1 as silent infeasibility.
    for (const auto& cut : stamped_cuts) {
      bound_alpha_for_cut(planning, scene_index, phase_index, cut);
    }

    // Step 2: bulk row dispatch — single backend call.
    auto& li = planning.system(scene_index, phase_index).linear_interface();
    li.add_rows(stamped_cuts, /*eps=*/0.0);

    // Step 3: per-cut bookkeeping (no-op when low_memory_mode == off).
    for (const auto& cut : stamped_cuts) {
      li.record_cut_row(cut);
    }
  }

  SPDLOG_TRACE("SDDP sharing: added {} cuts to phase {} for all {} scenes",
               all_cuts.size(),
               phase_index,
               num_scenes);
}

}  // namespace gtopt
