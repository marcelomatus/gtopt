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

/// Resolve the effective aperture definitions for this iteration.
///
/// Returns one of:
///   - empty optional → caller should fall back to the non-aperture
///     backward-pass path (apertures disabled, no aperture_array, or
///     no requested UIDs matched),
///   - filled optional → use the contained span as the effective
///     aperture definitions.  The span references either the original
///     `aperture_defs` (no filtering needed) or the `owned` storage
///     appended to for synthetic/filtered apertures.
///
/// Shared by `backward_pass_with_apertures` (loop) and
/// `backward_pass_with_apertures_single_phase` (single-phase
/// dispatcher).  Previously this was duplicated ~30 LOC across both
/// call sites; the filter semantics diverging silently was a
/// regression magnet.
[[nodiscard]] auto resolve_effective_apertures(
    std::span<const gtopt::Aperture> aperture_defs,
    std::span<const gtopt::ScenarioLP> all_scenarios,
    const std::optional<gtopt::Array<gtopt::Uid>>& requested_uids,
    gtopt::Array<gtopt::Aperture>& owned,  // NOLINT(runtime/references)
    std::string_view log_tag) -> std::optional<std::span<const gtopt::Aperture>>
{
  using namespace gtopt;
  if (!requested_uids.has_value()) {
    if (aperture_defs.empty()) {
      return std::nullopt;  // fallback
    }
    return std::span<const Aperture> {aperture_defs};
  }

  const auto& requested = *requested_uids;
  if (requested.empty()) {
    return std::nullopt;  // fallback
  }

  if (!aperture_defs.empty()) {
    // Filter existing apertures by requested UIDs.
    for (const auto& ap : aperture_defs) {
      if (std::ranges::find(requested, ap.uid) != requested.end()) {
        owned.push_back(ap);
      }
    }
    for (const auto uid : requested) {
      const bool found = std::ranges::any_of(
          owned, [uid](const auto& a) { return a.uid == uid; });
      if (!found) {
        SPDLOG_WARN("{}: requested UID {} not found, skipping", log_tag, uid);
      }
    }
  } else {
    // No aperture_array: build synthetic from scenarios matching UIDs.
    const auto num_all = std::ssize(all_scenarios);
    owned = build_synthetic_apertures(all_scenarios,
                                      std::min(std::ssize(requested), num_all));
  }

  if (owned.empty()) {
    return std::nullopt;  // fallback
  }
  return std::span<const Aperture> {owned};
}

}  // namespace

namespace gtopt
{

// ── Helper: build the ApertureSubmitFunc callback ───────────────────────────

auto SDDPMethod::make_aperture_submit_fn(PhaseIndex phase_index,
                                         IterationIndex iteration_index)
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
      .priority_key = make_sddp_task_key(iteration_index,
                                         SDDPPassDirection::backward,
                                         phase_index,
                                         SDDPTaskKind::lp),
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
      SPDLOG_WARN("SDDP Aperture: pool submit failed, running synchronously");
    }
    // Fallback: run synchronously
    std::promise<ApertureCutResult> p;
    p.set_value(task());
    return p.get_future();
  };
}

// ── Aperture per-phase implementation (shared by single-phase and full pass)

auto SDDPMethod::backward_pass_aperture_phase_impl(
    SceneIndex scene_index,
    PhaseIndex phase_index,
    int cut_offset,
    const ScenarioLP& base_scenario,
    std::span<const ScenarioLP> all_scenarios,
    std::span<const Aperture> aperture_defs,
    const SolverOptions& opts,
    IterationIndex iteration_index) -> std::expected<int, Error>
{
  auto& phase_states = m_scene_phase_states_[scene_index];
  int cuts_added = 0;
  m_phase_grid_.record(
      iteration_index, scene_uid(scene_index), phase_index, GridCell::Aperture);

  const auto src_phase_index = previous(phase_index);
  auto& src_sys = planning_lp().system(scene_index, src_phase_index);
  const auto& src_state = phase_states[src_phase_index];

  // Ensure the source-phase LP backend is live.  No-op when already
  // live; otherwise reloads from snapshot (snapshot/compress) or
  // re-flattens from collections (rebuild).
  src_sys.ensure_lp_built();

  auto& src_li = src_sys.linear_interface();
  const auto& plp = planning_lp().simulation().phases()[phase_index];

  auto& target_sys = planning_lp().system(scene_index, phase_index);
  target_sys.ensure_lp_built();

  // Populate the per-element XLP state (generation_cols, …) on the
  // main thread BEFORE dispatching aperture tasks.  Under compress
  // the backward-pass aperture update loop in sddp_aperture.cpp
  // reads `sys.collections()` from every task concurrently — without
  // a pre-populated state, each task would race on `m_collections_`
  // via `rebuild_collections_if_needed()` and segfault.
  // Single-threaded call here is safe; the subsequent aperture tasks
  // only read collections.  No-op under `off` (collections always
  // alive) and under `rebuild` (refreshed by rebuild_in_place).
  target_sys.rebuild_collections_if_needed();

  // Keep the flat LP decompressed while aperture tasks create clones.
  // The guard re-compresses on scope exit (level 2 only).
  const DecompressionGuard dcomp_guard(target_sys.linear_interface());

  // Resolve the α column for the source phase once; it is passed into
  // aperture cut building and reused below for any fallback.
  const auto* src_alpha_svar = find_alpha_state_var(
      planning_lp().simulation(), scene_index, src_phase_index);
  const auto src_alpha_col = (src_alpha_svar != nullptr)
      ? src_alpha_svar->col()
      : ColIndex {unknown_index};

  auto expected_cut = solve_apertures_for_phase(
      scene_index,
      phase_index,
      src_state,
      src_alpha_col,
      base_scenario,
      all_scenarios,
      aperture_defs,
      plp.apertures(),
      cut_offset,
      target_sys,
      plp,
      opts,
      m_label_maker_,
      m_options_.log_directory,
      scene_uid(scene_index),
      phase_uid(phase_index),
      make_aperture_submit_fn(phase_index, iteration_index),
      m_options_.aperture_timeout,
      m_options_.save_aperture_lp,
      m_aperture_cache_,
      iteration_index,
      m_options_.scale_alpha,
      m_options_.cut_coeff_eps,
      m_options_.cut_coeff_max,
      planning_lp().options().scale_objective());

  if (!expected_cut.has_value()) {
    // Fallback: build a regular Benders cut from the cached
    // forward-pass data (same as backward_pass).
    const auto& target_state = phase_states[phase_index];
    const auto sa = m_options_.scale_alpha;
    const auto ceps = m_options_.cut_coeff_eps;
    const auto cmax = m_options_.cut_coeff_max;
    const auto scale_obj = planning_lp().options().scale_objective();
    // Reduced costs are read from each link's back-pointer to the
    // source StateVariable (no per-phase full-vector caches).
    // Fallback Benders optimality cut, labelled `bcut` (backward-fallback)
    // to distinguish it from the forward-pass elastic feasibility cut
    // `fcut` (which is a real feasibility cut, stored as
    // CutType::Feasibility).  This bcut is built from the cached
    // forward-pass state-variable reduced costs and stored as
    // CutType::Optimality — it tightens the future-cost approximation
    // but does not represent a feasibility violation.
    auto fallback_cut = build_benders_cut(src_alpha_col,
                                          src_state.outgoing_links,
                                          target_state.forward_full_obj,
                                          sa,
                                          ceps,
                                          scale_obj);
    fallback_cut.class_name = "Sddp";
    fallback_cut.constraint_name = "bcut";
    fallback_cut.context = make_iteration_context(scene_uid(scene_index),
                                                  phase_uid(phase_index),
                                                  iteration_index,
                                                  cut_offset);
    rescale_benders_cut(fallback_cut, src_alpha_col, cmax);
    filter_cut_coefficients(fallback_cut, src_alpha_col, ceps);

    {
      const auto cut_row = src_li.add_row(fallback_cut);
      store_cut(scene_index,
                src_phase_index,
                fallback_cut,
                CutType::Optimality,
                cut_row);
    }
    ++cuts_added;

    SPDLOG_TRACE("{}: fallback cut for phase {} rhs={:.4f}",
                 sddp_log("Aperture",
                          iteration_index,
                          scene_uid(scene_index),
                          phase_uid(phase_index)),
                 src_phase_index,
                 fallback_cut.lowb);

    // No re-solve after add_row(): src_li was already optimal when this
    // backward step was entered (precondition of the surrounding
    // backpropagation), and the fallback cut is a valid Benders
    // optimality cut built from the cached forward-pass state-variable
    // reduced costs (no-span overload of build_benders_cut reads them
    // from each link's state_var->reduced_cost()).  Adding such a cut to
    // an optimal LP cannot produce infeasibility — only worsen the
    // objective.  The next forward pass at this (scene, phase) will
    // re-solve and update kappa naturally.

    return cuts_added;
  }

  rescale_benders_cut(*expected_cut, src_alpha_col, m_options_.cut_coeff_max);
  filter_cut_coefficients(
      *expected_cut, src_alpha_col, m_options_.cut_coeff_eps);

  {
    const auto cut_row = src_li.add_row(*expected_cut);
    store_cut(scene_index,
              src_phase_index,
              *expected_cut,
              CutType::Optimality,
              cut_row);
  }
  ++cuts_added;

  SPDLOG_TRACE("{}: cut for phase {} rhs={:.4f}",
               sddp_log("Aperture",
                        iteration_index,
                        scene_uid(scene_index),
                        phase_uid(phase_index)),
               src_phase_index,
               expected_cut->lowb);

  // Re-solve source phase after adding the cut to propagate feasibility.
  // Feasibility cuts are never shared between scenes.
  if (src_phase_index) {
    src_li.set_log_tag(sddp_log("Backward",
                                iteration_index,
                                scene_uid(scene_index),
                                phase_uid(src_phase_index)));
    auto r = src_li.resolve(opts);
    if (r.has_value() && src_li.is_optimal()) {
      update_max_kappa(scene_index, src_phase_index, src_li, iteration_index);
    }
    if (!r.has_value() || !src_li.is_optimal()) {
      SPDLOG_WARN(
          "{}: non-optimal after expected cut (status {}), "
          "skipping further backpropagation",
          sddp_log("Backward",
                   iteration_index,
                   scene_uid(scene_index),
                   phase_uid(src_phase_index)),
          src_li.get_status());
    }
  }

  return cuts_added;
}

// ── Per-phase aperture backward pass step ───────────────────────────────────

auto SDDPMethod::backward_pass_with_apertures_single_phase(
    SceneIndex scene_index,
    PhaseIndex phase_index,
    int cut_offset,
    const SolverOptions& opts,
    IterationIndex iteration_index) -> std::expected<int, Error>
{
  const auto& simulation = planning_lp().simulation();
  const auto& all_scenarios = simulation.scenarios();
  const auto& aperture_defs = simulation.apertures();
  const auto& scene_lp = simulation.scenes()[scene_index];
  const auto& scene_scenarios = scene_lp.scenarios();

  if (scene_scenarios.empty()) {
    return backward_pass_single_phase(
        scene_index, phase_index, cut_offset, opts, iteration_index);
  }

  Array<Aperture> owned;
  const auto effective = resolve_effective_apertures(
      aperture_defs,
      all_scenarios,
      m_options_.apertures,
      owned,
      sddp_log("Aperture", iteration_index, scene_uid(scene_index)));
  if (!effective.has_value()) {
    return backward_pass_single_phase(
        scene_index, phase_index, cut_offset, opts, iteration_index);
  }

  return backward_pass_aperture_phase_impl(scene_index,
                                           phase_index,
                                           cut_offset,
                                           scene_scenarios.front(),
                                           all_scenarios,
                                           *effective,
                                           opts,
                                           iteration_index);
}

// ── Aperture backward pass ──────────────────────────────────────────────────

auto SDDPMethod::backward_pass_with_apertures(SceneIndex scene_index,
                                              const SolverOptions& opts,
                                              IterationIndex iteration_index)
    -> std::expected<int, Error>
{
  const auto& simulation = planning_lp().simulation();
  const auto& all_scenarios = simulation.scenarios();
  const auto& aperture_defs = simulation.apertures();

  Array<Aperture> owned;
  const auto effective = resolve_effective_apertures(
      aperture_defs,
      all_scenarios,
      m_options_.apertures,
      owned,
      sddp_log("Aperture", iteration_index, scene_uid(scene_index)));
  if (!effective.has_value()) {
    return backward_pass(scene_index, opts, iteration_index);
  }
  const auto effective_defs = *effective;

  // ── Common aperture backward loop ─────────────────────────────────────
  const auto& phases = simulation.phases();
  const auto num_phases = static_cast<Index>(phases.size());
  auto& phase_states = m_scene_phase_states_[scene_index];
  int total_cuts = 0;
  [[maybe_unused]] const auto bwd_tid = std::this_thread::get_id();

  SPDLOG_INFO("{}: backward starting ({} phases) [thread {}]",
              sddp_log("Aperture", iteration_index, scene_uid(scene_index)),
              num_phases - 1,
              std::hash<std::thread::id> {}(bwd_tid) % 10000);

  const auto& scene_lp = simulation.scenes()[scene_index];
  const auto& scene_scenarios = scene_lp.scenarios();
  if (scene_scenarios.empty()) {
    return backward_pass(scene_index, opts, iteration_index);
  }
  const auto& base_scenario = scene_scenarios.front();
  // Collect phases where all apertures were infeasible for a summary
  std::vector<PhaseUid> infeasible_phases;

  for (const auto phase_index :
       iota_range<PhaseIndex>(1, num_phases) | std::views::reverse)
  {
    if (should_stop()) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format(
              "{}: cancelled at phase {}",
              sddp_log("Aperture", iteration_index, scene_uid(scene_index)),
              phase_uid(phase_index)),
      });
    }

    const auto src_phase_index = previous(phase_index);
    auto& src_sys = planning_lp().system(scene_index, src_phase_index);

    const auto& src_state = phase_states[src_phase_index];

    // Ensure the source-phase LP backend is live; no-op when already
    // loaded, otherwise reload (snapshot/compress) or re-flatten
    // (rebuild).
    src_sys.ensure_lp_built();

    auto& src_li = src_sys.linear_interface();
    const auto& plp = phases[phase_index];

    // Forward-pass summary for the target phase.
    const auto& target_state = phase_states[phase_index];

    auto& target_sys = planning_lp().system(scene_index, phase_index);
    target_sys.ensure_lp_built();
    // Single-threaded XLP-state rebuild before aperture tasks are
    // dispatched (same rationale as in
    // `backward_pass_aperture_phase_impl`).
    target_sys.rebuild_collections_if_needed();

    // Keep the flat LP decompressed while aperture tasks create clones.
    const DecompressionGuard dcomp_guard(target_sys.linear_interface());

    // Resolve α column for the source phase once per iteration.
    const auto* src_alpha_svar = find_alpha_state_var(
        planning_lp().simulation(), scene_index, src_phase_index);
    const auto src_alpha_col = (src_alpha_svar != nullptr)
        ? src_alpha_svar->col()
        : ColIndex {unknown_index};

    auto expected_cut = solve_apertures_for_phase(
        scene_index,
        phase_index,
        src_state,
        src_alpha_col,
        base_scenario,
        all_scenarios,
        effective_defs,
        plp.apertures(),
        total_cuts,
        target_sys,
        plp,
        opts,
        m_label_maker_,
        m_options_.log_directory,
        scene_uid(scene_index),
        phase_uid(phase_index),
        make_aperture_submit_fn(phase_index, iteration_index),
        0.0,
        m_options_.save_aperture_lp,
        m_aperture_cache_,
        iteration_index,
        m_options_.scale_alpha,
        m_options_.cut_coeff_eps,
        m_options_.cut_coeff_max,
        planning_lp().options().scale_objective());

    if (!expected_cut.has_value()) {
      infeasible_phases.push_back(phase_uid(phase_index));
      const auto sa = m_options_.scale_alpha;
      const auto ceps = m_options_.cut_coeff_eps;
      const auto cmax2 = m_options_.cut_coeff_max;
      const auto scale_obj = planning_lp().options().scale_objective();
      // Backward-fallback optimality cut — see the parallel site earlier
      // in this file for the `bcut` naming rationale (distinguishes from
      // the forward-pass elastic `fcut` which is a true feasibility cut).
      auto fallback_cut = build_benders_cut(src_alpha_col,
                                            src_state.outgoing_links,
                                            target_state.forward_full_obj,
                                            sa,
                                            ceps,
                                            scale_obj);
      fallback_cut.class_name = "Sddp";
      fallback_cut.constraint_name = "bcut";
      fallback_cut.context = make_iteration_context(scene_uid(scene_index),
                                                    phase_uid(phase_index),
                                                    iteration_index,
                                                    total_cuts);
      rescale_benders_cut(fallback_cut, src_alpha_col, cmax2);
      filter_cut_coefficients(fallback_cut, src_alpha_col, ceps);

      {
        const auto cut_row = src_li.add_row(fallback_cut);
        store_cut(scene_index,
                  src_phase_index,
                  fallback_cut,
                  CutType::Optimality,
                  cut_row);
      }
      ++total_cuts;

      SPDLOG_TRACE("{}: fallback cut for phase {} rhs={:.4f}",
                   sddp_log("Aperture",
                            iteration_index,
                            scene_uid(scene_index),
                            phase_uid(phase_index)),
                   src_phase_index,
                   fallback_cut.lowb);

      // No re-solve after add_row(): see the parallel fallback path
      // earlier in this file for the rationale.  Cut is built from
      // cached state-variable reduced costs; src_li was already optimal
      // and adding a valid Benders optimality cut cannot break that.

      continue;
    }

    rescale_benders_cut(*expected_cut, src_alpha_col, m_options_.cut_coeff_max);
    filter_cut_coefficients(
        *expected_cut, src_alpha_col, m_options_.cut_coeff_eps);

    {
      const auto cut_row = src_li.add_row(*expected_cut);
      store_cut(scene_index,
                src_phase_index,
                *expected_cut,
                CutType::Optimality,
                cut_row);
    }
    ++total_cuts;

    SPDLOG_TRACE("{}: cut for phase {} rhs={:.4f}",
                 sddp_log("Aperture",
                          iteration_index,
                          scene_uid(scene_index),
                          phase_uid(phase_index)),
                 src_phase_index,
                 expected_cut->lowb);

    // Re-solve source phase after adding the cut to propagate
    // feasibility.
    if (src_phase_index) {
      src_li.set_log_tag(sddp_log("Backward",
                                  iteration_index,
                                  scene_uid(scene_index),
                                  phase_uid(src_phase_index)));
      auto r = src_li.resolve(opts);
      if (r.has_value() && src_li.is_optimal()) {
        update_max_kappa(scene_index, src_phase_index, src_li, iteration_index);
      }
      if (!r.has_value() || !src_li.is_optimal()) {
        SPDLOG_WARN(
            "{}: non-optimal after expected cut (status {}), "
            "skipping further backpropagation",
            sddp_log("Backward",
                     iteration_index,
                     scene_uid(scene_index),
                     phase_uid(src_phase_index)),
            src_li.get_status());
      }
    }
  }

  // Log a single summary for all phases with infeasible apertures
  if (!infeasible_phases.empty()) {
    SPDLOG_WARN(
        "{}: all apertures infeasible at {} phase(s) [{}], "
        "used Benders fallback cuts",
        sddp_log("Aperture", iteration_index, scene_uid(scene_index)),
        infeasible_phases.size(),
        join_values(infeasible_phases));
  }

  return total_cuts;
}

}  // namespace gtopt
