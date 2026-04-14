/**
 * @file      sddp_cut_sharing.cpp
 * @brief     SDDP cut sharing across scenes — implementation
 * @date      2026-03-22
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <utility>

#include <gtopt/benders_cut.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_sharing.hpp>
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
    LpContext context)
{
  const auto num_scenes = planning.simulation().scene_count();

  if (num_scenes <= 1 || mode == CutSharingMode::none) {
    return;
  }

  // Apply metadata context to a row (when context is set).
  const auto apply_context = [&](SparseRow& row)
  {
    if (!std::holds_alternative<std::monostate>(context)) {
      row.class_name = "Sddp";
      row.constraint_name = "share";
      row.context = context;
    }
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

    auto accumulated = accumulate_benders_cuts(all_cuts);
    apply_context(accumulated);

    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      auto& sys = planning.system(scene_index, phase_index);
      sys.linear_interface().add_row(accumulated);
      sys.record_cut_row(accumulated);
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

    auto accumulated = accumulate_benders_cuts(scene_avg_cuts);
    apply_context(accumulated);

    for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
      auto& sys = planning.system(scene_index, phase_index);
      sys.linear_interface().add_row(accumulated);
      sys.record_cut_row(accumulated);
    }

    SPDLOG_TRACE(
        "SDDP sharing: added expected cut to phase {} "
        "({} scenes with cuts, summed from scene averages)",
        phase_index,
        scene_avg_cuts.size());

  } else if (mode == CutSharingMode::max) {
    // Max mode: add ALL cuts from ALL scenes to ALL scenes
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
      for (const auto& cut : all_cuts) {
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
