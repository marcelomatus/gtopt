/**
 * @file      sddp_aperture_pass.cpp
 * @brief     Aperture backward pass methods for SDDPMethod
 * @date      Mon Mar 24 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the aperture-based backward pass variants of the SDDP solver.
 * Extracted from sddp_method.cpp to reduce file size.
 * These methods are member functions of SDDPMethod defined in sddp_method.hpp.
 */

#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <ranges>
#include <span>
#include <thread>
#include <vector>

#include <gtopt/benders_cut.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_aperture.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_pool.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace
{

/// Format a range of formattable values as a comma-separated string.
template<typename Range>
[[nodiscard]] std::string join_values(const Range& values)
{
  std::string result;
  bool first = true;
  for (const auto& v : values) {
    if (!first) {
      result += ", ";
    }
    result += std::format("{}", v);
    first = false;
  }
  return result;
}

}  // namespace

namespace gtopt
{

// ── Helper: build the ApertureSubmitFunc callback ───────────────────────────

auto SDDPMethod::make_aperture_submit_fn(PhaseIndex phase,
                                         IterationIndex iteration)
    -> ApertureSubmitFunc
{
  // Submit aperture tasks to the SDDP work pool for parallel execution.
  // Each aperture task operates on its own LP clone, so they are
  // independent and can safely execute concurrently.  The calling scene
  // thread blocks on the returned futures while pool threads process
  // the aperture solves.
  //
  // Fallback to synchronous execution when no pool is available (e.g.
  // during unit tests).
  auto* pool = m_pool_;
  // TaskPriority::Medium is the scheduling tier (controls CPU threshold);
  // the SDDPTaskKey tuple provides the secondary sort within that tier.
  const BasicTaskRequirements<SDDPTaskKey> req {
      .priority = TaskPriority::Medium,
      .priority_key = make_sddp_task_key(
          iteration, SDDPPassDirection::backward, phase, SDDPTaskKind::lp),
      .name = {},
  };

  return [pool, req](const std::function<ApertureCutResult()>& task)
             -> std::future<ApertureCutResult>
  {
    if (pool != nullptr) {
      auto fut = pool->submit(task, req);
      if (fut.has_value()) {
        return std::move(*fut);
      }
      SPDLOG_WARN("SDDP aperture: pool submit failed, running synchronously");
    }
    // Fallback: run synchronously
    std::promise<ApertureCutResult> p;
    p.set_value(task());
    return p.get_future();
  };
}

// ── Aperture per-phase implementation (shared by single-phase and full pass)

auto SDDPMethod::backward_pass_aperture_phase_impl(
    SceneIndex scene,
    PhaseIndex phase,
    int cut_offset,
    const ScenarioLP& base_scenario,
    std::span<const ScenarioLP> all_scenarios,
    std::span<const Aperture> aperture_defs,
    const SolverOptions& opts,
    IterationIndex iteration) -> std::expected<int, Error>
{
  auto& phase_states = m_scene_phase_states_[scene];
  int cuts_added = 0;

  const auto src_phase = phase - PhaseIndex {1};
  auto& src_li = planning_lp().system(scene, src_phase).linear_interface();
  const auto& src_state = phase_states[src_phase];
  const auto& plp = planning_lp().simulation().phases()[phase];

  // Enable warm-start on aperture clone resolves when configured.
  auto aperture_solve_opts = opts;
  aperture_solve_opts.reuse_basis = m_options_.warm_start;

  // Forward-pass solution for the target phase — used as warm-start hint
  const auto& target_state = phase_states[phase];

  auto expected_cut =
      solve_apertures_for_phase(scene,
                                phase,
                                src_state,
                                base_scenario,
                                all_scenarios,
                                aperture_defs,
                                plp.apertures(),
                                cut_offset,
                                planning_lp().system(scene, phase),
                                plp,
                                aperture_solve_opts,
                                m_label_maker_,
                                m_options_.log_directory,
                                scene_uid(scene),
                                phase_uid(phase),
                                make_aperture_submit_fn(phase, iteration),
                                m_options_.aperture_timeout,
                                m_options_.save_aperture_lp,
                                m_aperture_cache_,
                                target_state.forward_col_sol,
                                target_state.forward_row_dual,
                                nullptr,  // no pooled clone — each task clones
                                iteration,
                                m_options_.cut_coeff_mode,
                                m_options_.scale_alpha,
                                m_options_.cut_coeff_eps,
                                m_options_.cut_coeff_max);

  if (!expected_cut.has_value()) {
    // Fallback: build a regular Benders cut from the cached
    // forward-pass data (same as backward_pass).
    const auto& target_state = phase_states[phase];
    const auto coeff_mode = m_options_.cut_coeff_mode;
    const auto sa = m_options_.scale_alpha;
    const auto ceps = m_options_.cut_coeff_eps;
    const auto cmax = m_options_.cut_coeff_max;
    auto fallback_cut = (coeff_mode == CutCoeffMode::row_dual)
        ? build_benders_cut_from_row_duals(
              src_state.alpha_col,
              src_state.outgoing_links,
              target_state.forward_row_dual,
              target_state.forward_full_obj,
              sddp_label("sddp", "fcut", scene, phase, iteration, cut_offset),
              sa,
              ceps)
        : build_benders_cut(
              src_state.alpha_col,
              src_state.outgoing_links,
              target_state.forward_col_cost,
              target_state.forward_full_obj,
              sddp_label("sddp", "fcut", scene, phase, iteration, cut_offset),
              sa,
              ceps);
    rescale_benders_cut(fallback_cut, src_state.alpha_col, cmax);
    filter_cut_coefficients(fallback_cut, src_state.alpha_col, ceps);

    {
      const auto cut_row = src_li.add_row(fallback_cut);
      store_cut(scene, src_phase, fallback_cut, CutType::Optimality, cut_row);
    }
    ++cuts_added;

    SPDLOG_TRACE("SDDP aperture fallback: scene {} cut for phase {} rhs={:.4f}",
                 scene,
                 src_phase,
                 fallback_cut.lowb);

    if (src_phase > PhaseIndex {0}) {
      auto r = src_li.resolve(opts);
      if (r.has_value() && src_li.is_optimal()) {
        update_max_kappa(scene, src_phase, src_li, iteration);
      }
      if (!r.has_value() || !src_li.is_optimal()) {
        SPDLOG_WARN(
            "SDDP backward: iter {} scene {} phase {} non-optimal after "
            "fallback cut (status {}), skipping further backpropagation",
            iteration,
            scene_uid(scene),
            phase_uid(src_phase),
            src_li.get_status());
      }
    }

    return cuts_added;
  }

  rescale_benders_cut(
      *expected_cut, src_state.alpha_col, m_options_.cut_coeff_max);
  filter_cut_coefficients(
      *expected_cut, src_state.alpha_col, m_options_.cut_coeff_eps);

  {
    const auto cut_row = src_li.add_row(*expected_cut);
    store_cut(scene, src_phase, *expected_cut, CutType::Optimality, cut_row);
  }
  ++cuts_added;

  SPDLOG_TRACE("SDDP aperture: scene {} cut for phase {} rhs={:.4f}",
               scene,
               src_phase,
               expected_cut->lowb);

  // Re-solve source phase after adding the cut to propagate feasibility.
  // Feasibility cuts are never shared between scenes.
  if (src_phase > PhaseIndex {0}) {
    auto r = src_li.resolve(opts);
    if (r.has_value() && src_li.is_optimal()) {
      update_max_kappa(scene, src_phase, src_li, iteration);
    }
    if (!r.has_value() || !src_li.is_optimal()) {
      SPDLOG_WARN(
          "SDDP backward: iter {} scene {} phase {} non-optimal after "
          "expected cut (status {}), skipping further backpropagation",
          iteration,
          scene_uid(scene),
          phase_uid(src_phase),
          src_li.get_status());
    }
  }

  return cuts_added;
}

// ── Per-phase aperture backward pass step ───────────────────────────────────

auto SDDPMethod::backward_pass_with_apertures_single_phase(
    SceneIndex scene,
    PhaseIndex phase,
    int cut_offset,
    const SolverOptions& opts,
    IterationIndex iteration) -> std::expected<int, Error>
{
  const auto& simulation = planning_lp().simulation();
  const auto& all_scenarios = simulation.scenarios();
  const auto& aperture_defs = simulation.apertures();
  const auto& scene_lp = simulation.scenes()[scene];
  const auto& scene_scenarios = scene_lp.scenarios();

  if (scene_scenarios.empty()) {
    return backward_pass_single_phase(
        scene, phase, cut_offset, opts, iteration);
  }

  // Determine effective apertures based on options
  Array<Aperture> filtered;
  std::span<const Aperture> effective_defs;

  if (!m_options_.apertures.has_value()) {
    // nullopt: use simulation aperture_array as-is (per-phase filtering
    // happens inside build_effective_apertures via Phase::apertures)
    if (aperture_defs.empty()) {
      return backward_pass_single_phase(
          scene, phase, cut_offset, opts, iteration);
    }
    effective_defs = aperture_defs;
  } else {
    // Non-empty UID list: filter aperture_defs to only matching UIDs
    const auto& requested = *m_options_.apertures;
    if (requested.empty()) {
      return backward_pass_single_phase(
          scene, phase, cut_offset, opts, iteration);
    }

    if (!aperture_defs.empty()) {
      // Filter existing apertures by requested UIDs
      for (const auto& ap : aperture_defs) {
        if (std::ranges::find(requested, ap.uid) != requested.end()) {
          filtered.push_back(ap);
        }
      }
      for (const auto uid : requested) {
        const bool found = std::ranges::any_of(
            filtered, [uid](const auto& a) { return a.uid == uid; });
        if (!found) {
          SPDLOG_WARN("SDDP apertures: requested UID {} not found, skipping",
                      uid);
        }
      }
    } else {
      // No aperture_array: build synthetic from scenarios matching UIDs
      filtered = build_synthetic_apertures(all_scenarios,
                                           static_cast<int>(requested.size()));
    }

    if (filtered.empty()) {
      return backward_pass_single_phase(
          scene, phase, cut_offset, opts, iteration);
    }
    effective_defs = filtered;
  }

  return backward_pass_aperture_phase_impl(scene,
                                           phase,
                                           cut_offset,
                                           scene_scenarios.front(),
                                           all_scenarios,
                                           effective_defs,
                                           opts,
                                           iteration);
}

// ── Aperture backward pass ──────────────────────────────────────────────────

auto SDDPMethod::backward_pass_with_apertures(SceneIndex scene,
                                              const SolverOptions& opts,
                                              IterationIndex iteration)
    -> std::expected<int, Error>
{
  const auto& simulation = planning_lp().simulation();
  const auto& all_scenarios = simulation.scenarios();
  const auto& aperture_defs = simulation.apertures();

  // Determine the effective aperture definitions to use
  Array<Aperture> filtered;
  std::span<const Aperture> effective_defs;

  if (!m_options_.apertures.has_value()) {
    // nullopt: use simulation aperture_array (per-phase filtering via
    // Phase::apertures happens downstream in build_effective_apertures)
    if (aperture_defs.empty()) {
      return backward_pass(scene, opts, iteration);
    }
    effective_defs = aperture_defs;
  } else {
    const auto& requested = *m_options_.apertures;
    if (requested.empty()) {
      return backward_pass(scene, opts, iteration);
    }

    if (!aperture_defs.empty()) {
      // Filter existing apertures by requested UIDs
      for (const auto& ap : aperture_defs) {
        if (std::ranges::find(requested, ap.uid) != requested.end()) {
          filtered.push_back(ap);
        }
      }
      for (const auto uid : requested) {
        const bool found = std::ranges::any_of(
            filtered, [uid](const auto& a) { return a.uid == uid; });
        if (!found) {
          SPDLOG_WARN("SDDP apertures: requested UID {} not found, skipping",
                      uid);
        }
      }
    } else {
      // No aperture_array: build synthetic from scenarios
      const auto num_all = static_cast<int>(all_scenarios.size());
      filtered = build_synthetic_apertures(
          all_scenarios, std::min(static_cast<int>(requested.size()), num_all));
    }

    if (filtered.empty()) {
      return backward_pass(scene, opts, iteration);
    }
    effective_defs = filtered;
  }

  // ── Common aperture backward loop ─────────────────────────────────────
  const auto& phases = simulation.phases();
  const auto num_phases = static_cast<Index>(phases.size());
  auto& phase_states = m_scene_phase_states_[scene];
  int total_cuts = 0;
  [[maybe_unused]] const auto bwd_tid = std::this_thread::get_id();

  SPDLOG_INFO(
      "SDDP backward (apertures): scene {} iter {} starting ({} phases) "
      "[thread {}]",
      scene_uid(scene),
      iteration,
      num_phases - 1,
      std::hash<std::thread::id> {}(bwd_tid) % 10000);

  const auto& scene_lp = simulation.scenes()[scene];
  const auto& scene_scenarios = scene_lp.scenarios();
  if (scene_scenarios.empty()) {
    return backward_pass(scene, opts, iteration);
  }
  const auto& base_scenario = scene_scenarios.front();
  // Collect phases where all apertures were infeasible for a summary
  std::vector<PhaseUid> infeasible_phases;

  for (const auto phase :
       iota_range<PhaseIndex>(1, num_phases) | std::views::reverse)
  {
    if (should_stop()) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format(
              "SDDP backward (apertures): cancelled at scene {} phase {}",
              scene_uid(scene),
              phase_uid(phase)),
      });
    }

    const auto src_phase = phase - PhaseIndex {1};
    auto& src_li = planning_lp().system(scene, src_phase).linear_interface();
    const auto& src_state = phase_states[src_phase];
    const auto& plp = phases[phase];

    // Enable warm-start on aperture clone resolves when configured.
    auto ws_opts = opts;
    ws_opts.reuse_basis = m_options_.warm_start;

    // Forward-pass solution for the target phase — warm-start hint
    const auto& target_state = phase_states[phase];

    auto expected_cut = solve_apertures_for_phase(
        scene,
        phase,
        src_state,
        base_scenario,
        all_scenarios,
        effective_defs,
        plp.apertures(),
        total_cuts,
        planning_lp().system(scene, phase),
        plp,
        ws_opts,
        m_label_maker_,
        m_options_.log_directory,
        scene_uid(scene),
        phase_uid(phase),
        make_aperture_submit_fn(phase, iteration),
        0.0,
        m_options_.save_aperture_lp,
        m_aperture_cache_,
        target_state.forward_col_sol,
        target_state.forward_row_dual,
        nullptr,  // no pooled clone — each task clones
        iteration,
        m_options_.cut_coeff_mode,
        m_options_.scale_alpha,
        m_options_.cut_coeff_eps);

    if (!expected_cut.has_value()) {
      infeasible_phases.push_back(phase_uid(phase));
      const auto coeff_mode2 = m_options_.cut_coeff_mode;
      const auto sa = m_options_.scale_alpha;
      const auto ceps = m_options_.cut_coeff_eps;
      const auto cmax2 = m_options_.cut_coeff_max;
      auto fallback_cut = (coeff_mode2 == CutCoeffMode::row_dual)
          ? build_benders_cut_from_row_duals(
                src_state.alpha_col,
                src_state.outgoing_links,
                target_state.forward_row_dual,
                target_state.forward_full_obj,
                sddp_label("sddp", "fcut", scene, phase, iteration, total_cuts),
                sa,
                ceps)
          : build_benders_cut(
                src_state.alpha_col,
                src_state.outgoing_links,
                target_state.forward_col_cost,
                target_state.forward_full_obj,
                sddp_label("sddp", "fcut", scene, phase, iteration, total_cuts),
                sa,
                ceps);
      rescale_benders_cut(fallback_cut, src_state.alpha_col, cmax2);
      filter_cut_coefficients(fallback_cut, src_state.alpha_col, ceps);

      {
        const auto cut_row = src_li.add_row(fallback_cut);
        store_cut(scene, src_phase, fallback_cut, CutType::Optimality, cut_row);
      }
      ++total_cuts;

      SPDLOG_TRACE(
          "SDDP aperture fallback: scene {} cut for phase {} "
          "rhs={:.4f}",
          scene,
          src_phase,
          fallback_cut.lowb);

      if (src_phase > PhaseIndex {0}) {
        auto r = src_li.resolve(opts);
        if (r.has_value() && src_li.is_optimal()) {
          update_max_kappa(scene, src_phase, src_li, iteration);
        }
        if (!r.has_value() || !src_li.is_optimal()) {
          SPDLOG_WARN(
              "SDDP backward: iter {} scene {} phase {} non-optimal "
              "after fallback cut (status {}), skipping further "
              "backpropagation",
              iteration,
              scene_uid(scene),
              phase_uid(src_phase),
              src_li.get_status());
        }
      }

      continue;
    }

    rescale_benders_cut(
        *expected_cut, src_state.alpha_col, m_options_.cut_coeff_max);
    filter_cut_coefficients(
        *expected_cut, src_state.alpha_col, m_options_.cut_coeff_eps);

    {
      const auto cut_row = src_li.add_row(*expected_cut);
      store_cut(scene, src_phase, *expected_cut, CutType::Optimality, cut_row);
    }
    ++total_cuts;

    SPDLOG_TRACE("SDDP aperture: scene {} cut for phase {} rhs={:.4f}",
                 scene,
                 src_phase,
                 expected_cut->lowb);

    // Re-solve source phase after adding the cut to propagate
    // feasibility.
    if (src_phase > PhaseIndex {0}) {
      auto r = src_li.resolve(opts);
      if (r.has_value() && src_li.is_optimal()) {
        update_max_kappa(scene, src_phase, src_li, iteration);
      }
      if (!r.has_value() || !src_li.is_optimal()) {
        SPDLOG_WARN(
            "SDDP backward: iter {} scene {} phase {} non-optimal "
            "after expected cut (status {}), skipping further "
            "backpropagation",
            iteration,
            scene_uid(scene),
            phase_uid(src_phase),
            src_li.get_status());
      }
    }
  }

  // Log a single summary for all phases with infeasible apertures
  if (!infeasible_phases.empty()) {
    SPDLOG_WARN(
        "SDDP aperture: scene {} — all apertures infeasible at {} "
        "phase(s) [{}], used Benders fallback cuts",
        scene_uid(scene),
        infeasible_phases.size(),
        join_values(infeasible_phases));
  }

  return total_cuts;
}

}  // namespace gtopt
