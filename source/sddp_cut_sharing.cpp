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
    PhaseIndex phase,
    const StrongIndexVector<SceneIndex, std::vector<SparseRow>>& scene_cuts,
    CutSharingMode mode,
    PlanningLP& planning,
    std::string_view label_prefix)
{
  const auto num_scenes =
      static_cast<Index>(planning.simulation().scenes().size());

  if (num_scenes <= 1 || mode == CutSharingMode::none) {
    return;
  }

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

    const auto accumulated = accumulate_benders_cuts(all_cuts, label_prefix);

    for (Index si = 0; si < num_scenes; ++si) {
      auto& li = planning.system(SceneIndex {si}, phase).linear_interface();
      li.add_row(accumulated);
    }

    SPDLOG_TRACE(
        "SDDP sharing: added accumulated cut to phase {} "
        "({} scene cuts summed)",
        phase,
        all_cuts.size());

  } else if (mode == CutSharingMode::expected) {
    // Expected mode: probability-weighted average cut.
    const auto& scenes = planning.simulation().scenes();
    std::vector<double> scene_probs(static_cast<std::size_t>(num_scenes), 0.0);
    double total_prob = 0.0;

    for (Index si = 0; si < num_scenes; ++si) {
      if (scene_cuts[SceneIndex {si}].empty()) {
        continue;
      }
      if (std::cmp_less(si, scenes.size())) {
        for (const auto& sc : scenes[si].scenarios()) {
          scene_probs[static_cast<std::size_t>(si)] += sc.probability_factor();
        }
      }
      if (scene_probs[static_cast<std::size_t>(si)] <= 0.0) {
        scene_probs[static_cast<std::size_t>(si)] = 1.0;
      }
      total_prob += scene_probs[static_cast<std::size_t>(si)];
    }

    if (total_prob <= 0.0) {
      return;
    }

    std::vector<SparseRow> scene_avg_cuts;
    std::vector<double> weights;
    scene_avg_cuts.reserve(static_cast<std::size_t>(num_scenes));
    weights.reserve(static_cast<std::size_t>(num_scenes));

    for (Index si = 0; si < num_scenes; ++si) {
      const auto& cuts = scene_cuts[SceneIndex {si}];
      if (cuts.empty()) {
        continue;
      }
      const double w = scene_probs[static_cast<std::size_t>(si)];
      if (w <= 0.0) {
        continue;
      }
      scene_avg_cuts.push_back(average_benders_cut(cuts, label_prefix));
      weights.push_back(w);
    }

    if (scene_avg_cuts.empty()) {
      return;
    }

    const auto avg =
        weighted_average_benders_cut(scene_avg_cuts, weights, label_prefix);

    for (Index si = 0; si < num_scenes; ++si) {
      auto& li = planning.system(SceneIndex {si}, phase).linear_interface();
      li.add_row(avg);
    }

    SPDLOG_TRACE(
        "SDDP sharing: added probability-weighted average cut to phase {} "
        "({} scenes with cuts, total_prob={:.4f})",
        phase,
        scene_avg_cuts.size(),
        total_prob);

  } else if (mode == CutSharingMode::max) {
    // Max mode: add ALL cuts from ALL scenes to ALL scenes
    std::vector<SparseRow> all_cuts;
    for (auto&& [si, cuts] : enumerate<SceneIndex>(scene_cuts)) {
      all_cuts.insert(all_cuts.end(), cuts.begin(), cuts.end());
    }

    if (all_cuts.empty()) {
      return;
    }

    for (Index si = 0; si < num_scenes; ++si) {
      auto& li = planning.system(SceneIndex {si}, phase).linear_interface();
      for (const auto& cut : all_cuts) {
        li.add_row(cut);
      }
    }

    SPDLOG_TRACE("SDDP sharing: added {} cuts to phase {} for all {} scenes",
                 all_cuts.size(),
                 phase,
                 num_scenes);
  }
}

}  // namespace gtopt
