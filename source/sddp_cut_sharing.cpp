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
  // ``SDDPCutStore::m_scene_cuts_``, which is populated exclusively
  // by ``SDDPCutStore::store_cut`` (backward-pass optimality and
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
    // Accumulate mode: sum all scene cuts into one accumulated cut.
    // When LP objectives already include probability factors, the correct
    // expected cut is the sum of all individual scene cuts.
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
    // Expected mode: average cuts within each scene, then sum across scenes.
    // Probability is already embedded in the LP objective coefficients
    // (via block_ecost = cost * probability * discount * duration / scale),
    // so the Benders cut z* and reduced costs inherit that weighting.
    // The correct expected-value cut is the sum of scene-averaged cuts.
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
    // Max mode: add ALL cuts from ALL scenes to ALL scenes.
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
      // ``extra`` is the per-cut counter within this destination
      // scene's broadcast — unique within (scene, phase, iter) so
      // the metadata invariant holds even when N source cuts land
      // on the same LP.  `iota_range` keeps the index strongly
      // typed (no raw int loop counter).
      for (auto&& [extra, src_cut] : enumerate<int>(all_cuts)) {
        auto cut = src_cut;  // copy: per-scene/per-cut stamp below
        stamp_for_scene(cut, scene_index, extra);
        // Same `add_cut_row` rationale as the accumulate / expected
        // branches above — release α's bootstrap pin via
        // `free_alpha_for_cut` whenever a shared optimality cut
        // references α.  The cuts broadcast in `max` mode are still
        // backward-pass optimality cuts (`SDDPCutStore::scene_cuts`),
        // so `CutType::Optimality` is correct here too.
        std::ignore = add_cut_row(planning,
                                  scene_index,
                                  phase_index,
                                  CutType::Optimality,
                                  cut,
                                  /*eps=*/0.0);
      }
    }

    SPDLOG_TRACE("SDDP sharing: added {} cuts to phase {} for all {} scenes",
                 all_cuts.size(),
                 phase_index,
                 num_scenes);
  }
}

}  // namespace gtopt
