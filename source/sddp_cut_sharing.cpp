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
  // context built from the destination scene's UID.  This is
  // intentionally per-scene (called inside the destination loop)
  // because the same accumulated row is replicated across scenes,
  // and each destination LP needs a label that is unique within
  // ITS OWN row table.  Using SceneUid{} (= unknown_uid) here would
  // collide on the first cut shared this iteration — see
  // ``make_iteration_context``'s precondition checks.
  const auto stamp_for_scene = [&](SparseRow& row, SceneIndex scene_index)
  {
    row.class_name = sddp_alpha_class_name;
    row.constraint_name = sddp_share_cut_constraint_name;
    row.context = make_iteration_context(
        sim.uid_of(scene_index), phase_uid, uid_of(iteration_index), 0);
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
      stamp_for_scene(scene_local, scene_index);
      auto& sys = planning.system(scene_index, phase_index);
      sys.linear_interface().add_row(scene_local);
      sys.record_cut_row(scene_local);
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
      stamp_for_scene(scene_local, scene_index);
      auto& sys = planning.system(scene_index, phase_index);
      sys.linear_interface().add_row(scene_local);
      sys.record_cut_row(scene_local);
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
      auto& sys = planning.system(scene_index, phase_index);
      auto& li = sys.linear_interface();
      for (auto cut : all_cuts) {  // copy each cut for per-scene stamp
        stamp_for_scene(cut, scene_index);
        li.add_row(cut);
        sys.record_cut_row(cut);
      }
    }

    SPDLOG_TRACE("SDDP sharing: added {} cuts to phase {} for all {} scenes",
                 all_cuts.size(),
                 phase_index,
                 num_scenes);
  }
}

}  // namespace gtopt
