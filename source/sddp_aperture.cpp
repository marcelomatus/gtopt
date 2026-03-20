/**
 * @file      sddp_aperture.cpp
 * @brief     Aperture backward-pass logic for SDDP solver
 * @date      2026-03-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the free functions declared in sddp_aperture.hpp.
 * Extracted from sddp_solver.cpp to reduce coupling and improve testability.
 */

#include <algorithm>
#include <filesystem>
#include <format>
#include <ranges>
#include <utility>

#include <gtopt/collection.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/sddp_aperture.hpp>
#include <gtopt/sddp_solver.hpp>
#include <gtopt/system_lp.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

// ─── build_effective_apertures ──────────────────────────────────────────────

auto build_effective_apertures(std::span<const Aperture> aperture_defs,
                               std::span<const Uid> phase_apertures)
    -> std::vector<ApertureEntry>
{
  std::vector<ApertureEntry> result;

  if (phase_apertures.empty()) {
    // Use all aperture definitions (each with count = 1)
    for (const auto& ap : aperture_defs) {
      if (ap.is_active()) {
        result.push_back({
            .aperture = std::cref(ap),
            .count = 1,
        });
      }
    }
    return result;
  }

  // Count occurrences of each UID in the phase_apertures list.
  // Preserving order of first appearance keeps results deterministic.
  std::vector<std::pair<Uid, int>> uid_counts;
  for (const auto ap_uid : phase_apertures) {
    auto it = std::ranges::find_if(
        uid_counts, [ap_uid](const auto& p) { return p.first == ap_uid; });
    if (it != uid_counts.end()) {
      ++(it->second);
    } else {
      uid_counts.emplace_back(ap_uid, 1);
    }
  }

  // Map each unique UID to its aperture definition
  for (const auto& [uid, cnt] : uid_counts) {
    auto it = std::ranges::find_if(aperture_defs,
                                   [uid](const auto& ap)
                                   { return ap.uid == uid && ap.is_active(); });
    if (it != aperture_defs.end()) {
      result.push_back({
          .aperture = std::cref(*it),
          .count = cnt,
      });
    }
  }

  return result;
}

// ─── build_synthetic_apertures ──────────────────────────────────────────────

auto build_synthetic_apertures(std::span<const ScenarioLP> all_scenarios,
                               int n_apertures) -> Array<Aperture>
{
  const auto n = std::min(n_apertures, static_cast<int>(all_scenarios.size()));
  Array<Aperture> synthetic;
  synthetic.reserve(static_cast<size_t>(n));
  const double prob = 1.0 / static_cast<double>(n);
  for (int i = 0; i < n; ++i) {
    const Uid scen_uid =
        static_cast<Uid>(all_scenarios[ScenarioIndex {i}].uid());
    synthetic.push_back(Aperture {
        .uid = scen_uid,
        .source_scenario = scen_uid,
        .probability_factor = prob,
    });
  }
  return synthetic;
}

// ─── solve_apertures_for_phase ──────────────────────────────────────────────

auto solve_apertures_for_phase(SceneIndex scene,
                               PhaseIndex phase,
                               const PhaseStateInfo& src_state,
                               const ScenarioLP& base_scenario,
                               std::span<const ScenarioLP> all_scenarios,
                               std::span<const Aperture> aperture_defs,
                               std::span<const Uid> phase_apertures,
                               int total_cuts,
                               SystemLP& sys,
                               const PhaseLP& phase_lp,
                               const SolverOptions& opts,
                               const LabelMaker& label_maker,
                               const std::string& log_directory,
                               int scene_uid,
                               int phase_uid,
                               const ApertureResolveFunc& resolve_fn,
                               double aperture_timeout,
                               std::span<const double> forward_col_sol,
                               std::span<const double> forward_row_dual)
    -> std::optional<SparseRow>
{
  const auto pi = static_cast<Index>(phase);
  const auto& phase_li = sys.linear_interface();

  // Apply aperture timeout to solver options if configured
  auto aperture_opts = opts;
  if (aperture_timeout > 0.0) {
    aperture_opts.time_limit = aperture_timeout;
  }

  std::vector<SparseRow> aperture_cuts;
  std::vector<double> aperture_weights;
  double total_weight = 0.0;

  // Build the effective aperture list for this phase
  auto effective_apertures =
      build_effective_apertures(aperture_defs, phase_apertures);

  if (effective_apertures.empty()) {
    return std::nullopt;
  }

  int n_infeasible = 0;
  [[maybe_unused]] int n_skipped = 0;

  for (const auto& [ap_ref, ap_count] : effective_apertures) {
    const auto& aperture = ap_ref.get();
    const auto ap_uid = aperture.uid;
    const double pf = aperture.probability_factor.value_or(1.0);
    if (pf <= 0.0) {
      SPDLOG_WARN(
          "SDDP aperture uid {}: non-positive probability_factor {:.6f}, "
          "using 1.0 as fallback",
          ap_uid,
          pf);
    }
    // The effective weight is N * probability_factor.
    // This makes the result equivalent to solving the LP N separate times.
    const double effective_pf = pf > 0.0 ? pf : 1.0;
    const double weight = static_cast<double>(ap_count) * effective_pf;

    // Find the scenario corresponding to this aperture's source_scenario
    auto scen_it = std::ranges::find_if(
        all_scenarios,
        [&aperture](const auto& scen)
        { return static_cast<Uid>(scen.uid()) == aperture.source_scenario; });

    if (scen_it == all_scenarios.end()) {
      SPDLOG_DEBUG(
          "SDDP aperture: scene {} phase {} aperture uid {} — "
          "source_scenario {} not found, skipping",
          scene,
          phase,
          ap_uid,
          aperture.source_scenario);
      ++n_skipped;
      continue;
    }

    const auto& aperture_scenario = *scen_it;

    // Clone the phase LP (state variables already fixed from forward pass).
    auto clone = phase_li.clone();

    // Update scenario-dependent bounds for this aperture.
    // Elements with update_aperture_lp (FlowLP, GeneratorProfileLP,
    // DemandProfileLP) get their per-block values replaced with the
    // aperture scenario data.  Other element types are silently skipped.
    auto aperture_visitor = [&](auto& e) -> bool
    {
      if constexpr (requires {
                      e.update_aperture_lp(clone,
                                           base_scenario,
                                           aperture_scenario,
                                           std::declval<const StageLP&>());
                    })
      {
        for (const auto& stage : phase_lp.stages()) {
          [[maybe_unused]] const auto ok = e.update_aperture_lp(
              clone, base_scenario, aperture_scenario, stage);
        }
      }
      return true;
    };
    visit_elements(sys.collections(), aperture_visitor);

    // Apply the saved forward-pass solution as warm-start hint.
    // Dimension mismatches (new cut rows) are handled internally.
    if (aperture_opts.warm_start) {
      clone.set_warm_start_solution(forward_col_sol, forward_row_dual);
    }

    // Solve the clone via the resolve callback (with aperture timeout)
    auto result = resolve_fn(clone, aperture_opts, phase);
    if (!result.has_value() || !clone.is_optimal()) {
      const auto status = clone.get_status();

      // Check for aperture timeout: status 1 (abandoned) or 3 (other)
      // when a time limit was set indicates a timeout
      if (aperture_timeout > 0.0 && (status == 1 || status == 3)) {
        ++n_infeasible;
        spdlog::warn(
            "SDDP aperture: scene {} phase {} aperture uid {} timed out "
            "({:.1f}s, status {}), treating as infeasible",
            scene_uid,
            phase_uid,
            ap_uid,
            aperture_timeout,
            status);
        continue;
      }

      ++n_infeasible;
      SPDLOG_DEBUG(
          "SDDP aperture: scene {} phase {} aperture uid {} infeasible "
          "(status {}), skipping",
          scene,
          phase,
          ap_uid,
          status);

      // Save the infeasible aperture LP for later inspection only in
      // trace/debug mode — aperture infeasibility is expected in some
      // scenarios and writing LPs during normal SDDP iteration is
      // expensive.
      if (!log_directory.empty() && spdlog::get_level() <= spdlog::level::debug)
      {
        std::filesystem::create_directories(log_directory);
        const auto err_stem = (std::filesystem::path(log_directory)
                               / std::format("error_aperture_sc_{}_ph_{}_ap_{}",
                                             scene_uid,
                                             phase_uid,
                                             ap_uid))
                                  .string();

        clone.write_lp(err_stem);
        SPDLOG_DEBUG("SDDP aperture: saved infeasible LP to {}.lp", err_stem);
      }

      continue;
    }

    // Build a Benders cut from the clone's reduced costs.
    // Use aperture UID (not 0-based index) in user-facing labels.
    const auto cut_name =
        label_maker.lp_label("sddp", "aper_cut", scene, pi, ap_uid, total_cuts);
    auto cut = build_benders_cut(src_state.alpha_col,
                                 src_state.outgoing_links,
                                 clone.get_col_cost(),
                                 clone.get_obj_value(),
                                 cut_name);

    // Accumulate the cut with weight = N * probability_factor.
    // Both the cut contribution and the denominator use the same
    // effective weight, so repeated optimal apertures correctly
    // amplify their influence in the weighted average.
    aperture_cuts.push_back(std::move(cut));
    aperture_weights.push_back(weight);
    total_weight += weight;
  }

  // Log summary when some apertures were infeasible
  [[maybe_unused]] const auto n_total =
      static_cast<int>(effective_apertures.size());
  [[maybe_unused]] const auto n_feasible =
      static_cast<int>(aperture_cuts.size());
  if (n_infeasible > 0) {
    SPDLOG_TRACE(
        "SDDP aperture: scene {} phase {} — {}/{} feasible, "
        "{} infeasible, {} skipped",
        scene,
        phase,
        n_feasible,
        n_total,
        n_infeasible,
        n_skipped);
  }

  if (aperture_cuts.empty()) {
    return std::nullopt;
  }

  // Normalise weights
  if (total_weight > 0.0) {
    for (auto& w : aperture_weights) {
      w /= total_weight;
    }
  }

  // Compute the probability-weighted expected cut
  const auto expected_name =
      label_maker.lp_label("sddp", "ecut", scene, pi, total_cuts);
  return weighted_average_benders_cut(
      aperture_cuts, aperture_weights, expected_name);
}

}  // namespace gtopt
