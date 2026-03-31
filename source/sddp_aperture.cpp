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
#include <chrono>
#include <filesystem>
#include <format>
#include <ranges>
#include <thread>
#include <utility>

#include <gtopt/collection.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/sddp_aperture.hpp>
#include <gtopt/sddp_method.hpp>
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
  const auto n =
      std::min(static_cast<std::size_t>(n_apertures), all_scenarios.size());
  Array<Aperture> synthetic;
  synthetic.reserve(n);
  const double prob = 1.0 / static_cast<double>(n);
  for (std::size_t i = 0; i < n; ++i) {
    const auto scen_uid = Uid {all_scenarios[ScenarioIndex {i}].uid()};
    synthetic.push_back(Aperture {
        .uid = scen_uid,
        .source_scenario = scen_uid,
        .probability_factor = prob,
    });
  }
  return synthetic;
}

// ─── solve_apertures_for_phase ──────────────────────────────────────────────

auto solve_apertures_for_phase(
    SceneIndex scene,
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
    [[maybe_unused]] const std::string& log_directory,
    SceneUid scene_uid,
    PhaseUid phase_uid,
    const ApertureSubmitFunc& submit_fn,
    double aperture_timeout,
    [[maybe_unused]] bool save_aperture_lp,
    const ApertureDataCache& aperture_cache,
    std::span<const double> forward_col_sol,
    std::span<const double> forward_row_dual,
    LinearInterface* pooled_clone,
    IterationIndex iteration,
    CutCoeffMode cut_coeff_mode,
    double scale_alpha,
    double cut_coeff_eps,
    double cut_coeff_max) -> std::optional<SparseRow>
{
  const auto pi = Index {phase};
  const auto& phase_li = sys.linear_interface();

  // Apply aperture timeout to solver options if configured
  auto aperture_opts = opts;
  if (aperture_timeout > 0.0) {
    aperture_opts.time_limit = aperture_timeout;
  }

  // Build the effective aperture list for this phase
  auto effective_apertures =
      build_effective_apertures(aperture_defs, phase_apertures);

  if (effective_apertures.empty()) {
    return std::nullopt;
  }

  // ── Submit all aperture tasks to the pool ────────────────────────────
  //
  // Each task is a complete unit: clone → update → warm-start → solve →
  // build cut.  Tasks are independent (separate LP clones) and execute
  // concurrently in the SDDP work pool.

  const auto phase_start = std::chrono::steady_clock::now();
  const auto caller_tid = std::this_thread::get_id();

  SPDLOG_INFO(
      "SDDP aperture: scene {} phase {} — starting {} aperture(s) "
      "[thread {}]",
      scene_uid,
      phase_uid,
      effective_apertures.size(),
      std::hash<std::thread::id> {}(caller_tid) % 10000);

  std::vector<std::future<ApertureCutResult>> futures;
  futures.reserve(effective_apertures.size());
  int n_skipped = 0;

  for (const auto& [ap_ref, ap_count] : effective_apertures) {
    const auto& aperture = ap_ref.get();
    const ApertureUid ap_uid {aperture.uid};
    const double pf = aperture.probability_factor.value_or(1.0);
    if (pf <= 0.0) {
      SPDLOG_WARN(
          "SDDP aperture uid {}: non-positive probability_factor {:.6f}, "
          "using 1.0 as fallback",
          ap_uid,
          pf);
    }
    const double effective_pf = pf > 0.0 ? pf : 1.0;
    const double weight = static_cast<double>(ap_count) * effective_pf;

    // Find the scenario corresponding to this aperture's source_scenario
    auto scen_it = std::ranges::find_if(
        all_scenarios,
        [&aperture](const auto& scen)
        { return Uid {scen.uid()} == aperture.source_scenario; });

    if (scen_it == all_scenarios.end() && aperture_cache.empty()) {
      spdlog::info(
          "SDDP aperture: scene {} phase {} aperture uid {} — "
          "source_scenario {} not found and no aperture cache, skipping",
          scene_uid,
          phase_uid,
          ap_uid,
          aperture.source_scenario);
      ++n_skipped;
      continue;
    }

    // Submit the entire aperture task (clone + update + solve + cut)
    futures.push_back(submit_fn(
        [&, ap_uid, weight, scen_it]() -> ApertureCutResult
        {
          const auto ap_start = std::chrono::steady_clock::now();
          const auto task_tid = std::this_thread::get_id();

          // Use pooled clone (reused across aperture solves) or create fresh
          auto owned_clone = pooled_clone
              ? std::optional<LinearInterface> {}
              : std::optional<LinearInterface> {phase_li.clone()};
          auto& clone = pooled_clone ? *pooled_clone : *owned_clone;

          // Update scenario-dependent bounds via a unified visitor.
          // Build a value-provider that reads from the scenario LP
          // arrays when the scenario is in the forward set, or from
          // the aperture data cache otherwise.
          // If the aperture's source scenario matches the forward-pass
          // base scenario, the clone already has the correct bounds —
          // skip the visitor update entirely.
          const bool is_base_scenario =
              (scen_it != all_scenarios.end()
               && Uid {scen_it->uid()} == Uid {base_scenario.uid()});

          if (!is_base_scenario) {
            auto visitor = [&](auto& e) -> bool
            {
              using E = std::remove_cvref_t<decltype(e)>;
              if constexpr (HasUpdateAperture<E>) {
                for (const auto& stage : phase_lp.stages()) {
                  // Build value_fn for this element
                  ApertureValueFn value_fn;
                  if (scen_it != all_scenarios.end()) {
                    const auto& ap_scen = *scen_it;
                    value_fn = [&e, &ap_scen](
                                   StageUid st,
                                   BlockUid bl) -> std::optional<double>
                    { return e.aperture_value(ap_scen.uid(), st, bl); };
                  } else {
                    const ScenarioUid ap_uid_val {aperture.source_scenario};
                    value_fn = [&e, &aperture_cache, ap_uid_val](
                                   StageUid st,
                                   BlockUid bl) -> std::optional<double>
                    {
                      return aperture_cache.lookup(E::ClassName.full_name(),
                                                   e.id().second,
                                                   ap_uid_val,
                                                   st,
                                                   bl);
                    };
                  }
                  [[maybe_unused]] const auto ok =
                      e.update_aperture(clone, base_scenario, value_fn, stage);
                }
              }
              return true;
            };
            visit_elements(sys.collections(), visitor);
          } else {
            SPDLOG_DEBUG(
                "SDDP aperture: scene {} phase {} aperture uid {}"
                " — source matches base scenario, "
                "skipping bound update",
                scene_uid,
                phase_uid,
                ap_uid);
          }

          // Apply warm-start hint
          if (aperture_opts.reuse_basis) {
            clone.set_warm_start_solution(forward_col_sol, forward_row_dual);
          }

          // Configure solver log file for aperture clone
          const auto log_mode =
              aperture_opts.log_mode.value_or(SolverLogMode::nolog);
          if (log_mode == SolverLogMode::detailed && !log_directory.empty()) {
            clone.set_log_file((std::filesystem::path(log_directory)
                                / std::format("{}_sc{}_ph{}_ap{}",
                                              clone.solver_name(),
                                              scene_uid,
                                              phase_uid,
                                              ap_uid))
                                   .string());
          }

          // Solve
          [[maybe_unused]] auto solve_result = clone.resolve(aperture_opts);
          const bool feasible = clone.is_optimal();

          if (!feasible) {
            const auto ap_s = std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - ap_start)
                                  .count();
            spdlog::info(
                "SDDP aperture: scene {} phase {} uid {} infeasible "
                "({:.3f}s) [thread {}]",
                scene_uid,
                phase_uid,
                ap_uid,
                ap_s,
                std::hash<std::thread::id> {}(task_tid) % 10000);
            return ApertureCutResult {
                .ap_uid = ap_uid,
                .weight = weight,
                .feasible = false,
                .status = clone.get_status(),
            };
          }

          // Build Benders cut from the clone's solution
          const auto cut_name = label_maker.lp_label(
              "sddp", "aper_cut", scene, pi, ap_uid, total_cuts);
          auto cut = (cut_coeff_mode == CutCoeffMode::row_dual)
              ? build_benders_cut_from_row_duals(src_state.alpha_col,
                                                 src_state.outgoing_links,
                                                 clone.get_row_dual(),
                                                 clone.get_obj_value(),
                                                 cut_name,
                                                 scale_alpha,
                                                 cut_coeff_eps)
              : build_benders_cut(src_state.alpha_col,
                                  src_state.outgoing_links,
                                  clone.get_col_cost(),
                                  clone.get_obj_value(),
                                  cut_name,
                                  scale_alpha,
                                  cut_coeff_eps);
          rescale_benders_cut(cut, src_state.alpha_col, cut_coeff_max);
          filter_cut_coefficients(cut, src_state.alpha_col, cut_coeff_eps);

          const auto ap_s = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - ap_start)
                                .count();
          spdlog::info(
              "SDDP aperture: scene {} phase {} uid {} solved "
              "({:.3f}s) [thread {}]",
              scene_uid,
              phase_uid,
              ap_uid,
              ap_s,
              std::hash<std::thread::id> {}(task_tid) % 10000);

          return ApertureCutResult {
              .ap_uid = ap_uid,
              .weight = weight,
              .feasible = true,
              .status = 0,
              .cut = std::move(cut),
          };
        }));
  }

  // ── Collect results ─────────────────────────────────────────────────

  std::vector<SparseRow> aperture_cuts;
  std::vector<double> aperture_weights;
  double total_weight = 0.0;
  int n_infeasible = 0;

  for (auto& fut : futures) {
    auto result = fut.get();

    if (!result.feasible) {
      ++n_infeasible;
      if (aperture_timeout > 0.0 && (result.status == 1 || result.status == 3))
      {
        spdlog::warn(
            "SDDP aperture: scene {} phase {} aperture uid {} timed out "
            "({:.1f}s, status {}), treating as infeasible",
            scene_uid,
            phase_uid,
            result.ap_uid,
            aperture_timeout,
            result.status);
      } else {
        spdlog::info(
            "SDDP aperture: scene {} phase {} aperture uid {} infeasible "
            "(status {}), skipping",
            scene_uid,
            phase_uid,
            result.ap_uid,
            result.status);
      }
      continue;
    }

    if (result.cut.has_value()) {
      aperture_cuts.push_back(std::move(*result.cut));
      aperture_weights.push_back(result.weight);
      total_weight += result.weight;
    }
  }

  // Log summary
  const auto n_total = effective_apertures.size();
  const auto n_feasible = aperture_cuts.size();
  const auto phase_elapsed = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - phase_start)
                                 .count();
  spdlog::info(
      "SDDP aperture: scene {} phase {} — {}/{} feasible, "
      "{} infeasible, {} skipped ({:.3f}s) [thread {}]",
      scene_uid,
      phase_uid,
      n_feasible,
      n_total,
      n_infeasible,
      n_skipped,
      phase_elapsed,
      std::hash<std::thread::id> {}(caller_tid) % 10000);

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
      label_maker.lp_label("sddp", "ecut", scene, pi, iteration, total_cuts);
  return weighted_average_benders_cut(
      aperture_cuts, aperture_weights, expected_name);
}

}  // namespace gtopt
