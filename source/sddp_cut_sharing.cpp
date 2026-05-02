/**
 * @file      sddp_cut_sharing.cpp
 * @brief     SDDP cut sharing across scenes — implementation
 * @date      2026-03-22
 * @author    marcelo
 * @copyright BSD-3-Clause
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
  // by ``max`` mode where every source cut is broadcast verbatim
  // to every scene, so a single iteration installs N cuts that
  // would otherwise collide on metadata.  Using SceneUid{} (=
  // unknown_uid) would collide on the first cut shared this
  // iteration — see ``make_iteration_context``'s precondition
  // checks.
  //
  // Hot-start / cascade safety: this ``extra`` only travels through
  // the in-memory LP row label and the ``m_active_cuts_`` low-memory
  // replay buffer (``LinearInterface::record_cut_row``).  Saved cut
  // files (``save_cuts_csv``) iterate
  // ``SDDPCutManager::m_scene_cuts_``, which is populated exclusively
  // by ``SDDPCutManager::store_cut`` (backward-pass optimality and
  // feasibility cuts).  ``share_cuts_for_phase`` never calls
  // ``store_cut``, so the broadcast rows minted here — and their
  // ``extra`` discriminators — are never persisted to the cut CSV.
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

  if (mode == CutSharingMode::accumulate) {
    // Accumulate mode: sum all scene cuts into one accumulated cut, then
    // broadcast to every scene's α^k LP.
    //
    // **VALIDITY WARNING** (2026-04-30 audit):
    // gtopt is multi-cut SDDP — each scene s has its own α^k_s column
    // bounding `Q_s(x)` (scene s's value function along scene s's
    // SPECIFIC sample path).  Summing scene cuts gives a cut whose
    // RHS is `Σ_s prob_s · Q_s*(x_s_trial)` — a quantity related to
    // E[Q] but NOT a valid lower bound on any individual scene's
    // α^k_d (which bounds Q_d, not E[Q]).  The earlier comment
    // claimed "probability is already embedded in LP coefficients
    // via block_ecost so summing gives the expected cut" — that
    // reasoning is correct for SINGLE-CUT SDDP (one shared α
    // bounding E[Q]) but NOT for the multi-cut formulation gtopt
    // implements.  See `support/sddp_cut_sharing_fix_plan_2026-04-30.md`.
    //
    // Result: this mode produces LB > UB that compounds across
    // iterations whenever scenes draw distinct sample-path
    // realizations (the typical multi-scenario production case,
    // e.g. juan/gtopt_iplp with 16 distinct hydrology samples).
    // It is mathematically valid only when every scene literally
    // realizes the same sample path (same inflows, demands,
    // capacities at every (phase, block)) — typically only true
    // for synthetic test fixtures.
    //
    // The shipping fix is `cut_sharing_mode: none` for production
    // runs; a runtime WARN is emitted at SDDP setup
    // (`sddp_method.cpp::initialize_solver`).  This branch is
    // retained for backwards compatibility on identical-sample-
    // path test fixtures (e.g. `test_sddp_bounds_sanity.cpp`'s
    // 2s10p case).
    std::vector<SparseRow> all_cuts;
    for (auto&& [si, cuts] : enumerate<SceneIndex>(scene_cuts)) {
      all_cuts.insert(all_cuts.end(), cuts.begin(), cuts.end());
    }

    if (all_cuts.empty()) {
      return;
    }

    const auto accumulated = accumulate_benders_cuts(all_cuts);

    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      auto scene_local = accumulated;  // copy: per-scene metadata
      // Single accumulated cut per scene → ``extra = 0`` is unique.
      stamp_for_scene(scene_local, scene_index, /*extra=*/0);
      // Route through the unified `add_cut_row`: it gates on
      // `CutType::Optimality` to call `free_alpha_for_cut`, releasing
      // the α^phase_index bootstrap pin if (and only if) the cut row
      // references α.  The previous raw `add_row + record_cut_row`
      // pair skipped that step, so shared optimality cuts that
      // reference α (every backward-pass cut does) left α frozen at
      // `lowb = uppb = 0` — making the phase LP infeasible on the
      // next iteration as soon as the cut required α > 0.  Observed
      // on juan/gtopt_iplp iter i1 p1: every scene declared
      // infeasible with "no predecessor phase to cut on" because
      // sddp_share_m1_* cuts demanded α ≈ 1.16e8 against a pinned
      // α = 0.  See `support/linear_interface_lifecycle_plan_2026-04-30.md`
      // §2.2 — manual `add_row + record_cut_row` pair was flagged
      // as a hazard; this is its first concrete victim.
      std::ignore = add_cut_row(planning,
                                scene_index,
                                phase_index,
                                CutType::Optimality,
                                scene_local,
                                /*eps=*/0.0);
    }

    SPDLOG_TRACE(
        "SDDP sharing: added accumulated cut to phase {} "
        "({} scene cuts summed)",
        phase_index,
        all_cuts.size());

  } else if (mode == CutSharingMode::expected) {
    // Expected mode: average cuts within each scene, then sum across
    // scenes; broadcast the sum to every scene's α^k LP.
    //
    // **VALIDITY WARNING** — see the `accumulate` branch above for the
    // full audit context.  In summary: the architecture mismatch is
    // that this mode tries to compute a single-cut SDDP "expected
    // value cut" but installs it on multi-cut SDDP per-scene α
    // columns.  The result is a cut whose RHS is shifted relative to
    // each scene's correct lower bound; LB > UB results whenever
    // scenes draw distinct sample-path realizations.  Use
    // `cut_sharing=none` for production multi-scenario runs.
    // See `support/sddp_cut_sharing_fix_plan_2026-04-30.md`.
    std::vector<SparseRow> scene_avg_cuts;
    scene_avg_cuts.reserve(static_cast<std::size_t>(num_scenes));

    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      const auto& cuts = scene_cuts[scene_index];
      if (cuts.empty()) {
        continue;
      }
      scene_avg_cuts.push_back(average_benders_cut(cuts));
    }

    if (scene_avg_cuts.empty()) {
      return;
    }

    const auto accumulated = accumulate_benders_cuts(scene_avg_cuts);

    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      auto scene_local = accumulated;  // copy: per-scene metadata
      // Single expected cut per scene → ``extra = 0`` is unique.
      stamp_for_scene(scene_local, scene_index, /*extra=*/0);
      // Route through the unified `add_cut_row`: it gates on
      // `CutType::Optimality` to call `free_alpha_for_cut`, releasing
      // the α^phase_index bootstrap pin if (and only if) the cut row
      // references α.  The previous raw `add_row + record_cut_row`
      // pair skipped that step, so shared optimality cuts that
      // reference α (every backward-pass cut does) left α frozen at
      // `lowb = uppb = 0` — making the phase LP infeasible on the
      // next iteration as soon as the cut required α > 0.  Observed
      // on juan/gtopt_iplp iter i1 p1: every scene declared
      // infeasible with "no predecessor phase to cut on" because
      // sddp_share_m1_* cuts demanded α ≈ 1.16e8 against a pinned
      // α = 0.  See `support/linear_interface_lifecycle_plan_2026-04-30.md`
      // §2.2 — manual `add_row + record_cut_row` pair was flagged
      // as a hazard; this is its first concrete victim.
      std::ignore = add_cut_row(planning,
                                scene_index,
                                phase_index,
                                CutType::Optimality,
                                scene_local,
                                /*eps=*/0.0);
    }

    SPDLOG_TRACE(
        "SDDP sharing: added expected cut to phase {} "
        "({} scenes with cuts, summed from scene averages)",
        phase_index,
        scene_avg_cuts.size());

  } else if (mode == CutSharingMode::max) {
    // Max mode: add ALL cuts from ALL scenes to ALL scenes.  The LP
    // solver will pick the tightest active cut on each scene's α^k.
    //
    // **VALIDITY WARNING** — see the `accumulate` branch above for
    // the full audit context.  In summary: a cut from scene S
    // broadcast onto scene D's α^k_D LP forces
    //   α^k_D ≥ prob_S · Q_S*(x_S_trial)
    // which is a valid bound on `Q_D(·)` only when S and D draw
    // identical sample paths.  Empirical: `max` mode produces the
    // largest LB-overshoot of the three sharing modes because the
    // LP picks the TIGHTEST broadcast cut, which is precisely the
    // scene with the highest (prob_S · Q_S*) — never the actual
    // bound on D's Q.  `cut_sharing=none` is the only safe choice
    // for production multi-scenario runs.  See
    // `support/sddp_cut_sharing_fix_plan_2026-04-30.md`.
    //
    // Each cut already carries its source-scene metadata from
    // ``apply_cut_sharing_for_iteration::to_sparse_row``, so we
    // stamp here only to overwrite with the DESTINATION scene's
    // context (each scene's LP needs a unique-within-LP label;
    // the source scene's metadata is fine cross-scene but breaks
    // the duplicate-label invariant within one LP when the same
    // cut is appended multiple times for max-mode broadcast).
    std::vector<SparseRow> all_cuts;
    for (auto&& [si, cuts] : enumerate<SceneIndex>(scene_cuts)) {
      all_cuts.insert(all_cuts.end(), cuts.begin(), cuts.end());
    }

    if (all_cuts.empty()) {
      return;
    }

    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      // Bulk-install path — `max` mode broadcasts ALL `all_cuts.size()`
      // cuts onto every scene's α^k LP, so the inner loop below is a
      // genuine within-cell bulk opportunity (multiple cuts landing on
      // ONE LinearInterface).  Replaces the per-cut `add_cut_row` loop
      // with: stamp + α-release pass, single `add_rows` dispatch, then
      // per-cut `record_cut_row` for low-memory replay.  Saves
      // N-1 backend round-trips per destination scene.
      //
      // `free_alpha_for_cut` is idempotent across the batch (it's just
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
      // cheap no-op).  Same `CutType::Optimality` semantics as the
      // accumulate / expected branches above — backward-pass
      // optimality cuts that demand α > 0 require this release; the
      // pre-fix raw `add_row + record_cut_row` pair skipped it and
      // left α frozen at `lowb = uppb = 0`, observed on
      // juan/gtopt_iplp iter i1 p1 as silent infeasibility.
      for (const auto& cut : stamped_cuts) {
        free_alpha_for_cut(planning, scene_index, phase_index, cut);
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
}

}  // namespace gtopt
