/**
 * @file      sddp_solver.cpp
 * @brief     SDDP (Stochastic Dual Dynamic Programming) solver implementation
 * @date      2026-03-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the forward/backward iterative decomposition with multi-scene
 * support, iterative feasibility backpropagation, and optimality cut sharing.
 * See sddp_solver.hpp for the algorithm description and the free-function
 * building blocks declared there.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <gtopt/check_lp.hpp>
#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_pool.hpp>
#include <gtopt/sddp_solver.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/utils.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

// ─── Utilities ──────────────────────────────────────────────────────────────

CutSharingMode parse_cut_sharing_mode(std::string_view name)
{
  if (name == "expected") {
    return CutSharingMode::Expected;
  }
  if (name == "accumulate") {
    return CutSharingMode::Accumulate;
  }
  if (name == "max") {
    return CutSharingMode::Max;
  }
  if (name == "none") {
    return CutSharingMode::None;
  }
  // Default to None when unrecognised (matches SDDPOptions default)
  return CutSharingMode::None;
}

ElasticFilterMode parse_elastic_filter_mode(std::string_view name)
{
  if (name == "backpropagate") {
    return ElasticFilterMode::BackpropagateBounds;
  }
  if (name == "multi_cut") {
    return ElasticFilterMode::MultiCut;
  }
  // "single_cut", "cut", or anything else → FeasibilityCut (default)
  return ElasticFilterMode::FeasibilityCut;
}

// ─── Free utility functions ──────────────────────────────────────────────────

std::vector<double> compute_scene_weights(
    std::span<const SceneLP> scenes,
    std::span<const uint8_t> scene_feasible) noexcept
{
  const auto num_scenes = static_cast<int>(scene_feasible.size());
  std::vector<double> weights(static_cast<std::size_t>(num_scenes), 0.0);
  double total = 0.0;

  for (int si = 0; si < num_scenes; ++si) {
    if (scene_feasible[static_cast<std::size_t>(si)] == 0U) {
      continue;  // Infeasible → weight stays 0
    }
    if (std::cmp_less(si, scenes.size())) {
      for (const auto& sc : scenes[static_cast<std::size_t>(si)].scenarios()) {
        weights[static_cast<std::size_t>(si)] += sc.probability_factor();
      }
    }
    if (weights[static_cast<std::size_t>(si)] <= 0.0) {
      weights[static_cast<std::size_t>(si)] = 1.0;  // fallback equal weight
    }
    total += weights[static_cast<std::size_t>(si)];
  }

  if (total > 0.0) {
    for (auto& w : weights) {
      w /= total;
    }
  } else {
    // All infeasible or zero probability → equal weight among feasible
    int feasible_count = 0;
    for (int si = 0; si < num_scenes; ++si) {
      if (scene_feasible[static_cast<std::size_t>(si)] != 0U) {
        ++feasible_count;
      }
    }
    if (feasible_count > 0) {
      const double eq_w = 1.0 / static_cast<double>(feasible_count);
      for (int si = 0; si < num_scenes; ++si) {
        if (scene_feasible[static_cast<std::size_t>(si)] != 0U) {
          weights[static_cast<std::size_t>(si)] = eq_w;
        }
      }
    }
  }

  return weights;
}

double compute_convergence_gap(double upper_bound, double lower_bound) noexcept
{
  const double denom = std::max(1.0, std::abs(upper_bound));
  return (upper_bound - lower_bound) / denom;
}

// ─── Free-function building blocks ──────────────────────────────────────────
// Now implemented in benders_cut.cpp; this file uses them via benders_cut.hpp.

// Now implemented in benders_cut.cpp; this file uses them via benders_cut.hpp.

// ── Helper: local utilities ─────────────────────────────────────────────────

namespace
{

/// Format a vector of ints as a comma-separated string.
[[nodiscard]] std::string join_ints(std::span<const int> values)
{
  std::string result;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      result += ", ";
    }
    result += std::to_string(values[i]);
  }
  return result;
}

}  // namespace

// ─── SDDPSolver ─────────────────────────────────────────────────────────────

SDDPSolver::SDDPSolver(PlanningLP& planning_lp, SDDPOptions opts) noexcept
    : m_planning_lp_(planning_lp)
    , m_options_(std::move(opts))
    , m_label_maker_(planning_lp.options())
{
}

// ── Initialisation ──────────────────────────────────────────────────────────

void SDDPSolver::initialize_alpha_variables(SceneIndex scene)
{
  const auto& phases = planning_lp().simulation().phases();

  auto& phase_states = m_scene_phase_states_[scene];
  phase_states.resize(phases.size());

  // Add α (future-cost) variable to every phase except the last
  for (auto&& [pi, _phase] : enumerate<PhaseIndex>(phases)) {
    if (pi == PhaseIndex {static_cast<Index>(phases.size()) - 1}) {
      break;
    }
    auto& state = phase_states[pi];
    auto& li = planning_lp().system(scene, pi).linear_interface();

    state.alpha_col = li.add_col(sddp_label("sddp", "alpha", scene, pi),
                                 m_options_.alpha_min,
                                 m_options_.alpha_max);
    li.set_obj_coeff(state.alpha_col, 1.0);
  }

  // Last phase: no future cost
  phase_states[PhaseIndex {static_cast<Index>(phases.size()) - 1}].alpha_col =
      ColIndex {unknown_index};
}

void SDDPSolver::collect_state_variable_links(SceneIndex scene)
{
  const auto& sim = planning_lp().simulation();
  const auto& phases = sim.phases();

  auto& phase_states = m_scene_phase_states_[scene];

  for (auto&& [phase, _ph] : enumerate<PhaseIndex>(phases)) {
    auto& state = phase_states[phase];

    // Read column bounds from the source phase LP
    const auto& src_li = planning_lp().system(scene, phase).linear_interface();
    const auto col_lo = src_li.get_col_low();
    const auto col_hi = src_li.get_col_upp();

    const auto next_phase = PhaseIndex {static_cast<Index>(phase) + 1};

    for (const auto& [key, svar] : sim.state_variables(scene, phase)) {
      for (const auto& dep : svar.dependent_variables()) {
        if (dep.phase_index() != next_phase || dep.scene_index() != scene) {
          continue;
        }

        state.outgoing_links.push_back(StateVarLink {
            .source_col = svar.col(),
            .dependent_col = dep.col(),
            .source_phase = phase,
            .target_phase = dep.phase_index(),
            .source_low = col_lo[svar.col()],
            .source_upp = col_hi[svar.col()],
        });
      }
    }

    SPDLOG_TRACE("SDDP: scene {} phase {} has {} outgoing state-variable links",
                 scene,
                 phase,
                 state.outgoing_links.size());
  }
}

// ── Elastic filter via LP clone (PLP pattern) ───────────────────────────────

std::optional<SDDPSolver::ElasticResult> SDDPSolver::elastic_solve(
    SceneIndex scene, PhaseIndex phase, const SolverOptions& opts)
{
  if (phase == PhaseIndex {0}) {
    return std::nullopt;
  }

  const auto& li = planning_lp().system(scene, phase).linear_interface();
  const auto prev = PhaseIndex {static_cast<Index>(phase) - 1};
  const auto& prev_state = m_scene_phase_states_[scene][prev];

  // Delegate to BendersCut member (uses work pool when set)
  auto result = m_benders_cut_.elastic_filter_solve(
      li, prev_state.outgoing_links, m_options_.elastic_penalty, opts);

  if (result.has_value()) {
    SPDLOG_TRACE(
        "SDDP elastic: scene {} phase {} solved via clone "
        "(obj={:.4f})",
        scene,
        phase,
        result->clone.get_obj_value());
  }

  return result;
}

bool SDDPSolver::check_sentinel_stop() const
{
  if (m_options_.sentinel_file.empty()) {
    return false;
  }
  return std::filesystem::exists(m_options_.sentinel_file);
}

bool SDDPSolver::check_api_stop_request() const
{
  if (m_options_.api_stop_request_file.empty()) {
    return false;
  }
  return std::filesystem::exists(m_options_.api_stop_request_file);
}

bool SDDPSolver::should_stop() const
{
  return m_stop_requested_.load() || check_sentinel_stop()
      || check_api_stop_request();
}

// ── Coefficient updates ─────────────────────────────────────────────────────

void SDDPSolver::update_coefficients_for_phase(SceneIndex scene,
                                               PhaseIndex phase,
                                               int iteration)
{
  auto& sys = planning_lp().system(scene, phase);

  const auto updated =
      update_lp_coefficients(sys, planning_lp().options(), iteration, phase);

  if (updated > 0) {
    SPDLOG_TRACE(
        "SDDP: updated {} LP coefficients for scene {} phase {} (iter {})",
        updated,
        scene,
        phase,
        iteration);
  }
}

// ── Forward pass ────────────────────────────────────────────────────────────

// ── SDDP task priority helpers ───────────────────────────────────────────────

namespace
{

/// Build an `SDDPTaskKey` tuple for an SDDP LP solve task.
///
/// The key is `(iteration, is_backward, phase, is_nonlp)` where:
///  - `is_backward`: 0 = forward pass, 1 = backward pass
///  - `is_nonlp`:    0 = LP solve/resolve, 1 = other (e.g. write_lp)
///
/// With the default `std::less<SDDPTaskKey>` comparator (lexicographic),
/// smaller tuples have **higher** execution priority:
///  - Lower iteration → higher priority
///  - Forward pass (0) → higher priority than backward (1)
///  - Lower phase index → higher priority
///  - LP solve (0) → higher priority than non-LP (1)
///
/// Both forward and backward LP solves use `TaskPriority::Medium`.
/// The tuple key alone provides the full SDDP ordering, removing the
/// need for the old High/Medium tier split.

BasicTaskRequirements<SDDPTaskKey> make_forward_lp_task_req(
    int iteration, PhaseIndex phase) noexcept
{
  return BasicTaskRequirements<SDDPTaskKey> {
      .priority = TaskPriority::Medium,
      .priority_key =
          SDDPTaskKey {
              iteration,
              kSDDPKeyForward,
              static_cast<int>(phase),
              kSDDPKeyIsLP,
          },
      .name = {},
  };
}

BasicTaskRequirements<SDDPTaskKey> make_backward_lp_task_req(
    int iteration, PhaseIndex phase) noexcept
{
  return BasicTaskRequirements<SDDPTaskKey> {
      .priority = TaskPriority::Medium,
      .priority_key =
          SDDPTaskKey {
              iteration,
              kSDDPKeyBackward,
              static_cast<int>(phase),
              kSDDPKeyIsLP,
          },
      .name = {},
  };
}

}  // namespace

auto SDDPSolver::forward_pass(SceneIndex scene,
                              int iteration,
                              const SolverOptions& opts)
    -> std::expected<double, Error>
{
  const auto& phases = planning_lp().simulation().phases();
  auto& phase_states = m_scene_phase_states_[scene];
  double total_opex = 0.0;

  SPDLOG_DEBUG("SDDP forward: scene {} iter {} starting ({} phases)",
               scene_uid(scene),
               iteration,
               phases.size());

  for (auto&& [phase, _ph] : enumerate<PhaseIndex>(phases)) {
    auto& li = planning_lp().system(scene, phase).linear_interface();
    auto& state = phase_states[phase];

    // Propagate state variables from previous phase
    if (phase != PhaseIndex {0}) {
      const auto prev = PhaseIndex {static_cast<Index>(phase) - 1};
      auto& prev_st = phase_states[prev];
      const auto& prev_sol =
          planning_lp().system(scene, prev).linear_interface().get_col_sol();
      propagate_trial_values(prev_st.outgoing_links, prev_sol, li);
      SPDLOG_TRACE(
          "SDDP forward: scene {} phase {} propagated {} state vars from "
          "phase {}",
          scene_uid(scene),
          phase_uid(phase),
          prev_st.outgoing_links.size(),
          phase_uid(prev));
    }

    // Update volume-dependent coefficients (turbine efficiency, etc.)
    update_coefficients_for_phase(scene, phase, iteration);

    // If lp_debug is enabled, write LP file (pre-solve state) then optionally
    // submit gzip compression as a fire-and-forget async task.
    if (m_options_.lp_debug) {
      const auto dbg_stem = (std::filesystem::path(m_options_.log_directory)
                             / std::format(sddp_file::debug_lp_fmt,
                                           iteration,
                                           scene_uid(scene),
                                           phase_uid(phase)))
                                .string();
      m_lp_debug_writer_.write(li, dbg_stem);
    }

    // Solve this phase via the work pool with forward-pass priority
    auto result =
        resolve_via_pool(li, opts, make_forward_lp_task_req(iteration, phase));

    if (!result.has_value() || !li.is_optimal()) {
      SPDLOG_WARN(
          "SDDP forward: iter {} scene {} phase {} non-optimal (status {}), "
          "trying elastic solve",
          iteration,
          scene_uid(scene),
          phase_uid(phase),
          li.get_status());
      // Clone the LP, apply elastic filter, and solve the clone.
      // The original LP remains unmodified (PLP clone pattern).
      auto elastic_result = elastic_solve(scene, phase, opts);
      if (elastic_result.has_value()) {
        const LinearInterface& solved_li = elastic_result->clone;
        // Increment infeasibility counter for this (scene, phase)
        ++m_infeasibility_counter_[scene][phase];

        // Cache solution data for the backward pass
        const auto obj = solved_li.get_obj_value();
        state.forward_full_obj = obj;

        const auto rc = solved_li.get_col_cost();
        state.forward_col_cost.assign(rc.begin(), rc.end());

        const auto alpha_val = (state.alpha_col != ColIndex {unknown_index})
            ? solved_li.get_col_sol()[state.alpha_col]
            : 0.0;
        state.forward_objective = obj - alpha_val;
        total_opex += state.forward_objective;

        SPDLOG_INFO(
            "SDDP forward: scene {} phase {} elastic solve ok, "
            "obj={:.4f} alpha={:.4f} opex={:.4f} [infeas_count={}]",
            scene_uid(scene),
            phase_uid(phase),
            obj,
            alpha_val,
            state.forward_objective,
            m_infeasibility_counter_[scene][phase]);
      } else {
        // Save the infeasible LP and run diagnostics only when:
        //  - it's the first phase (the scene will be declared infeasible), or
        //  - trace/debug logging is enabled (developer debugging).
        // During normal SDDP iteration, skip writing/diagnosing error LPs
        // to avoid I/O overhead.
        const bool is_first_phase = (phase == PhaseIndex {0});
        const bool is_trace_debug =
            (spdlog::get_level() <= spdlog::level::debug);
        if (!m_options_.log_directory.empty()
            && (is_first_phase || is_trace_debug))
        {
          std::filesystem::create_directories(m_options_.log_directory);
          const auto err_file =
              (std::filesystem::path(m_options_.log_directory)
               / std::format(
                   sddp_file::error_lp_fmt, scene_uid(scene), phase_uid(phase)))
                  .string();
          li.write_lp(err_file);
          spdlog::warn("SDDP: saved infeasible LP to {}.lp", err_file);
          // Run gtopt_check_lp static analysis and log the diagnostic.
          // Pass the full SolverOptions so the diagnostic uses the same
          // algorithm and tolerance settings as the gtopt solver.
          if (const auto diag = run_check_lp_diagnostic(
                  err_file, /*timeout_seconds=*/10, opts);
              !diag.empty())
          {
            log_diagnostic_lines("error", err_file + ".lp", diag);
          }
        }
        return std::unexpected(Error {
            .code = ErrorCode::SolverError,
            .message = std::format(
                "SDDP forward pass failed at scene {} phase {} (status {})",
                scene,
                phase,
                li.get_status()),
        });
      }
    } else {
      // Phase solved normally – reset infeasibility counter
      m_infeasibility_counter_[scene][phase] = 0;

      // Cache solution data for the backward pass
      const auto obj = li.get_obj_value();
      state.forward_full_obj = obj;

      const auto rc = li.get_col_cost();
      state.forward_col_cost.assign(rc.begin(), rc.end());

      const auto alpha_val = (state.alpha_col != ColIndex {unknown_index})
          ? li.get_col_sol()[state.alpha_col]
          : 0.0;
      state.forward_objective = obj - alpha_val;
      total_opex += state.forward_objective;

      SPDLOG_TRACE(
          "SDDP forward: scene {} phase {} obj={:.4f} alpha={:.4f} opex={:.4f}",
          scene_uid(scene),
          phase_uid(phase),
          obj,
          alpha_val,
          state.forward_objective);
    }
  }

  SPDLOG_DEBUG("SDDP forward: scene {} iter {} done, total_opex={:.4f}",
               scene_uid(scene),
               iteration,
               total_opex);
  return total_opex;
}

// ── Helper: store a cut for sharing and persistence (thread-safe) ───────────

void SDDPSolver::store_cut(SceneIndex scene,
                           PhaseIndex src_phase,
                           const SparseRow& cut)
{
  StoredCut stored {
      .phase = phase_uid(src_phase),
      .scene = scene_uid(scene),
      .name = cut.name,
      .rhs = cut.lowb,
  };
  for (const auto& [col, coeff] : cut.cmap) {
    stored.coefficients.emplace_back(static_cast<int>(col), coeff);
  }
  // Per-scene storage: no lock needed (each scene writes its own vector)
  m_scene_cuts_[scene].push_back(stored);
  // Shared storage: needs lock for cut sharing and combined persistence
  const std::scoped_lock lock(m_cuts_mutex_);
  m_stored_cuts_.push_back(std::move(stored));
}

// ── Helper: resolve an LP via the work pool (avoids naked direct calls) ─────

auto SDDPSolver::resolve_via_pool(
    LinearInterface& li,
    const SolverOptions& opts,
    const BasicTaskRequirements<SDDPTaskKey>& task_req)
    -> std::expected<int, Error>
{
  if (m_pool_ == nullptr) {
    // No pool available — fall back to direct solve
    return li.resolve(opts);
  }

  auto fut =
      m_pool_->submit([&li, &opts] { return li.resolve(opts); }, task_req);
  if (fut.has_value()) {
    return fut->get();
  }
  // Pool submission failed — fall back to direct solve
  SPDLOG_WARN("resolve_via_pool: pool submit failed, falling back to direct");
  return li.resolve(opts);
}

// ── Helper: resolve a clone via the work pool ───────────────────────────────

auto SDDPSolver::resolve_clone_via_pool(
    LinearInterface& clone,
    const SolverOptions& opts,
    const BasicTaskRequirements<SDDPTaskKey>& task_req)
    -> std::expected<int, Error>
{
  if (m_pool_ == nullptr) {
    return clone.resolve(opts);
  }

  // Submit resolve to the pool.  The clone reference is safe because we
  // call future.get() synchronously before this scope exits.
  auto fut = m_pool_->submit([&clone, &opts] { return clone.resolve(opts); },
                             task_req);
  if (fut.has_value()) {
    return fut->get();
  }
  // Pool submission failed — fall back to direct solve
  SPDLOG_WARN(
      "resolve_clone_via_pool: pool submit failed, falling back to direct");
  return clone.resolve(opts);
}

// ── Helper: iterative feasibility backpropagation ───────────────────────────

auto SDDPSolver::feasibility_backpropagate(SceneIndex scene,
                                           PhaseIndex start_phase,
                                           int total_cuts,
                                           const SolverOptions& opts,
                                           int iteration)
    -> std::expected<int, Error>
{
  auto& phase_states = m_scene_phase_states_[scene];
  int cuts_added = 0;

  // Iterate backward from start_phase to phase 0
  const auto end_phase = static_cast<Index>(start_phase) + 1;
  for (const auto back_pi :
       std::views::iota(Index {0}, end_phase) | std::views::reverse)
  {
    const auto back_phase = PhaseIndex {back_pi};

    if (back_pi > 0) {
      SPDLOG_WARN(
          "SDDP backward: scene {} phase {} infeasible after "
          "cut, backpropagating to phase {}",
          scene,
          back_phase,
          back_pi - 1);
    }

    // Clone the LP, apply elastic filter, solve the clone.
    // The original LP is never modified by the elastic filter.
    auto elastic_result = elastic_solve(scene, back_phase, opts);
    if (elastic_result.has_value()) {
      if (back_pi > 0) {
        // Build a feasibility-like cut for the previous phase
        const auto prev_bp = PhaseIndex {back_pi - 1};
        auto& prev_li = planning_lp().system(scene, prev_bp).linear_interface();
        const auto& prev_state = phase_states[prev_bp];

        if (m_options_.elastic_filter_mode
            == ElasticFilterMode::BackpropagateBounds)
        {
          // PLP mechanism: instead of building a feasibility cut,
          // propagate the elastic-clone dependent-column solution
          // values back as updated bounds on the source columns in
          // the previous phase.  This forces the previous phase to
          // produce a trial point that is known feasible for the
          // current phase, avoiding further infeasibility without
          // adding a cut row.
          const auto& dep_sol = elastic_result->clone.get_col_sol();
          for (const auto& link : prev_state.outgoing_links) {
            const double feasible_val = dep_sol[link.dependent_col];
            prev_li.set_col_low(link.source_col, feasible_val);
            prev_li.set_col_upp(link.source_col, feasible_val);
          }
          SPDLOG_TRACE(
              "SDDP backward (BackpropagateBounds): scene {} phase {} "
              "bounds updated to elastic trial values",
              scene,
              prev_bp);
        } else {
          // single_cut or multi_cut mode:
          // Always add the regular Benders feasibility cut.
          auto feas_cut =
              build_benders_cut(prev_state.alpha_col,
                                prev_state.outgoing_links,
                                elastic_result->clone.get_col_cost(),
                                elastic_result->clone.get_obj_value(),
                                sddp_label("sddp",
                                           "scut",
                                           scene,
                                           back_pi,
                                           iteration,
                                           total_cuts + cuts_added));

          prev_li.add_row(feas_cut);
          ++cuts_added;

          // multi_cut: also add one bound-constraint cut per
          // state variable whose elastic slack was activated.
          // Auto-switch to multi_cut when:
          //   threshold == 0 (always), OR
          //   threshold > 0 and counter > threshold.
          const bool use_multi_cut =
              (m_options_.elastic_filter_mode == ElasticFilterMode::MultiCut)
              || (m_options_.multi_cut_threshold == 0)
              || (m_options_.multi_cut_threshold > 0
                  && m_infeasibility_counter_[scene][back_phase]
                      > m_options_.multi_cut_threshold);

          if (use_multi_cut) {
            auto mc_cuts =
                build_multi_cuts(*elastic_result,
                                 prev_state.outgoing_links,
                                 sddp_label("sddp",
                                            "mcut",
                                            scene,
                                            back_pi,
                                            iteration,
                                            total_cuts + cuts_added));

            for (auto& mc : mc_cuts) {
              prev_li.add_row(mc);
              ++cuts_added;
            }
          }
        }

        // Re-solve the previous phase with updated cuts or bounds
        auto r3 = resolve_via_pool(
            prev_li, opts, make_backward_lp_task_req(0, PhaseIndex {back_pi}));
        if (r3.has_value() && prev_li.is_optimal()) {
          break;  // Feasibility restored
        }
        // Continue backpropagating to back_pi - 1
      } else {
        break;  // Restored at phase 0
      }
    } else if (back_pi == 0) {
      // Phase 0 with no elastic filter available = scene infeasible
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message =
              std::format("SDDP: scene {} is infeasible (backpropagated to "
                          "phase 0)",
                          scene),
      });
    }
  }

  return cuts_added;
}

// ── Per-phase backward-pass step (optimality cut only; no feasibility sharing)

auto SDDPSolver::backward_pass_single_phase(SceneIndex scene,
                                            PhaseIndex phase,
                                            int cut_offset,
                                            const SolverOptions& opts,
                                            int iteration)
    -> std::expected<int, Error>
{
  auto& phase_states = m_scene_phase_states_[scene];
  int cuts_added = 0;

  const auto pi = static_cast<Index>(phase);
  const auto prev_phase = PhaseIndex {pi - 1};
  auto& src_li = planning_lp().system(scene, prev_phase).linear_interface();
  const auto& src_state = phase_states[prev_phase];

  // Use cached forward-pass solution for cut generation.
  const auto& target_state = phase_states[phase];

  auto cut = build_benders_cut(
      src_state.alpha_col,
      src_state.outgoing_links,
      target_state.forward_col_cost,
      target_state.forward_full_obj,
      sddp_label("sddp", "scut", scene, phase, iteration, cut_offset));

  store_cut(scene, prev_phase, cut);

  src_li.add_row(cut);
  ++cuts_added;

  SPDLOG_TRACE("SDDP backward: scene {} cut for phase {} rhs={:.4f}",
               scene_uid(scene),
               phase_uid(src_phase),
               cut.lowb);

  // Re-solve source and handle iterative feasibility backpropagation.
  // Feasibility cuts are never shared between scenes — they stay local.
  if (pi > 0) {
    auto r = resolve_via_pool(
        src_li, opts, make_backward_lp_task_req(iteration, prev_phase));
    if (!r.has_value() || !src_li.is_optimal()) {
      SPDLOG_WARN(
          "SDDP backward: iter {} scene {} phase {} non-optimal after cut "
          "(status {}), starting feasibility backpropagation",
          iteration,
          scene_uid(scene),
          phase_uid(prev_phase),
          src_li.get_status());
      auto bp_result = feasibility_backpropagate(
          scene, prev_phase, cut_offset + cuts_added, opts, iteration);
      if (!bp_result.has_value()) {
        return std::unexpected(std::move(bp_result.error()));
      }
      cuts_added += *bp_result;
    }
  }

  return cuts_added;
}

// ── Backward pass with iterative feasibility backpropagation ────────────────

auto SDDPSolver::backward_pass(SceneIndex scene,
                               const SolverOptions& opts,
                               int iteration) -> std::expected<int, Error>
{
  const auto num_phases =
      static_cast<Index>(planning_lp().simulation().phases().size());
  int total_cuts = 0;

  SPDLOG_DEBUG("SDDP backward: scene {} iter {} starting ({} phases)",
               scene_uid(scene),
               iteration,
               num_phases);

  // Iterate backward from last phase to phase 1
  for (const auto pi : std::views::iota(1, num_phases) | std::views::reverse) {
    auto step_result = backward_pass_single_phase(
        scene, PhaseIndex {pi}, total_cuts, opts, iteration);
    if (!step_result.has_value()) {
      return std::unexpected(std::move(step_result.error()));
    }
    total_cuts += *step_result;
  }

  SPDLOG_DEBUG("SDDP backward: scene {} iter {} done, {} cuts added",
               scene_uid(scene),
               iteration,
               total_cuts);
  return total_cuts;
}

// ── Cut sharing ─────────────────────────────────────────────────────────────

void SDDPSolver::share_cuts_for_phase(
    PhaseIndex phase,
    const StrongIndexVector<SceneIndex, std::vector<SparseRow>>& scene_cuts,
    int iteration)
{
  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());

  if (num_scenes <= 1 || m_options_.cut_sharing == CutSharingMode::None) {
    return;
  }

  if (m_options_.cut_sharing == CutSharingMode::Accumulate) {
    // Accumulate mode: when LP objectives already include probability
    // factors, the correct expected cut is the sum of all individual
    // scene cuts (no averaging needed).  Each cut's coefficients and RHS
    // are already probability-weighted by the LP objective.
    //
    // Reference: Birge & Louveaux (2011) §5.1 — when the subproblem
    // objective is  prob_s · c_s'x_s,  the Benders cut coefficients
    // inherit the probability weighting and should be accumulated.
    std::vector<SparseRow> all_cuts;
    for (auto&& [si, cuts] : enumerate<SceneIndex>(scene_cuts)) {
      all_cuts.insert(all_cuts.end(), cuts.begin(), cuts.end());
    }

    if (all_cuts.empty()) {
      return;
    }

    // Sum all cuts into one accumulated cut
    const auto accumulated = accumulate_benders_cuts(
        all_cuts, sddp_label("sddp", "accum", phase, iteration));

    // Add the accumulated cut to all scenes
    for (Index si = 0; si < num_scenes; ++si) {
      auto& li =
          planning_lp().system(SceneIndex {si}, phase).linear_interface();
      li.add_row(accumulated);
    }

    SPDLOG_TRACE(
        "SDDP sharing: added accumulated cut to phase {} "
        "({} scene cuts summed)",
        phase,
        all_cuts.size());

  } else if (m_options_.cut_sharing == CutSharingMode::Expected) {
    // Expected mode: compute probability-weighted average cut.
    // Correct when LP objectives do NOT include probability factors.

    // Get scenario probability for each scene (sum of all scenario
    // probability_factors in that scene). Scenes with no cuts (infeasible)
    // automatically get weight 0. The weights are then normalised to sum
    // to 1.
    const auto& scenes = planning_lp().simulation().scenes();
    std::vector<double> scene_probs(static_cast<std::size_t>(num_scenes), 0.0);
    double total_prob = 0.0;

    for (Index si = 0; si < num_scenes; ++si) {
      if (scene_cuts[SceneIndex {si}].empty()) {
        // Infeasible or no cuts generated — skip this scene
        continue;
      }
      if (std::cmp_less(si, scenes.size())) {
        for (const auto& sc : scenes[si].scenarios()) {
          scene_probs[static_cast<std::size_t>(si)] += sc.probability_factor();
        }
      }
      if (scene_probs[static_cast<std::size_t>(si)] <= 0.0) {
        // No positive probability weight — fall back to equal weight
        scene_probs[static_cast<std::size_t>(si)] = 1.0;
      }
      total_prob += scene_probs[static_cast<std::size_t>(si)];
    }

    if (total_prob <= 0.0) {
      return;
    }

    // For each scene with positive weight, compute the average of its
    // cuts, then compute the probability-weighted average across scenes.
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
      scene_avg_cuts.push_back(average_benders_cut(
          cuts, sddp_label("sddp", "tmp", phase, iteration)));
      weights.push_back(w);
    }

    if (scene_avg_cuts.empty()) {
      return;
    }

    // Compute probability-weighted average cut
    const auto avg = weighted_average_benders_cut(
        scene_avg_cuts, weights, sddp_label("sddp", "avg", phase, iteration));

    // Add the average cut to all scenes
    for (Index si = 0; si < num_scenes; ++si) {
      auto& li =
          planning_lp().system(SceneIndex {si}, phase).linear_interface();
      li.add_row(avg);
    }

    SPDLOG_TRACE(
        "SDDP sharing: added probability-weighted average cut to phase {} "
        "({} scenes with cuts, total_prob={:.4f})",
        phase,
        scene_avg_cuts.size(),
        total_prob);

  } else if (m_options_.cut_sharing == CutSharingMode::Max) {
    // Max mode: add ALL cuts from ALL scenes to ALL scenes for this phase
    std::vector<SparseRow> all_cuts;
    for (auto&& [si, cuts] : enumerate<SceneIndex>(scene_cuts)) {
      all_cuts.insert(all_cuts.end(), cuts.begin(), cuts.end());
    }

    if (all_cuts.empty()) {
      return;
    }

    for (Index si = 0; si < num_scenes; ++si) {
      auto& li =
          planning_lp().system(SceneIndex {si}, phase).linear_interface();
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

// ── Cut persistence ─────────────────────────────────────────────────────────

auto SDDPSolver::save_cuts(const std::string& filepath) const
    -> std::expected<void, Error>
{
  try {
    // Ensure parent directory exists before writing
    const auto parent = std::filesystem::path(filepath).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    std::ofstream ofs(filepath);
    if (!ofs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message =
              std::format("Cannot open cut file for writing: {}", filepath),
      });
    }

    // CSV format: phase,scene,name,rhs[,col_idx:coeff ...]
    // Both RHS and coefficients are stored in fully physical space so that
    // cuts are portable across runs with different scale_objective or
    // variable scaling configurations:
    //   rhs_csv       = rhs_lp × scale_objective
    //   coeff_csv     = LP_coeff × scale_objective / col_scale
    // The alpha/varphi column (col_scale = 1) stores scale_objective as its
    // physical coefficient; on reload it becomes 1.0 after dividing by the
    // (possibly different) scale_objective of the new run.
    const auto scale_obj = planning_lp().options().scale_objective();
    ofs << "# scale_objective=" << scale_obj << "\n";
    ofs << "phase,scene,name,rhs,coefficients\n";

    // Build phase UID → PhaseIndex lookup for scale retrieval
    const auto& sim = planning_lp().simulation();
    const auto num_phases = static_cast<Index>(sim.phases().size());
    std::unordered_map<int, PhaseIndex> phase_map;
    for (Index pi = 0; pi < num_phases; ++pi) {
      phase_map[static_cast<int>(sim.phases()[pi].uid())] = PhaseIndex {pi};
    }

    for (const auto& cut : m_stored_cuts_) {
      // RHS in physical objective units
      ofs << cut.phase << "," << cut.scene << "," << cut.name << ","
          << (cut.rhs * scale_obj);

      // Look up the LinearInterface to retrieve column scales.
      // Use scene 0 as representative (scales are identical across scenes
      // because the LP structure is built identically per phase).
      auto pit = phase_map.find(cut.phase);
      if (pit != phase_map.end()) {
        const auto& li = planning_lp()
                             .system(SceneIndex {0}, pit->second)
                             .linear_interface();
        const auto& idx_to_name = li.col_index_to_name();
        for (const auto& [col, coeff] : cut.coefficients) {
          const auto scale = li.get_col_scale(ColIndex {col});
          ofs << ",";
          const auto ucol = static_cast<size_t>(col);
          if (ucol < idx_to_name.size() && !idx_to_name[ucol].empty()) {
            ofs << idx_to_name[ucol] << "/";
          }
          ofs << col << ":" << (coeff * scale_obj / scale);
        }
      } else {
        // Phase UID not found — should not happen for well-formed cuts.
        // Write with scale_obj only (assume col_scale = 1.0).
        SPDLOG_WARN(
            "save_cuts: unknown phase UID {} for cut '{}'; "
            "writing without variable scaling",
            cut.phase,
            cut.name);
        for (const auto& [col, coeff] : cut.coefficients) {
          ofs << "," << col << ":" << (coeff * scale_obj);
        }
      }
      ofs << "\n";
    }

    SPDLOG_TRACE("SDDP: saved {} cuts to {}", m_stored_cuts_.size(), filepath);
    return {};

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message =
            std::format("Error saving cuts to {}: {}", filepath, e.what()),
    });
  }
}

auto SDDPSolver::save_scene_cuts(SceneIndex scene,
                                 const std::string& directory) const
    -> std::expected<void, Error>
{
  try {
    std::filesystem::create_directories(directory);

    const auto filepath =
        (std::filesystem::path(directory)
         / std::format(sddp_file::scene_cuts_fmt, scene_uid(scene)))
            .string();

    const auto& cuts = m_scene_cuts_[scene];

    std::ofstream ofs(filepath);
    if (!ofs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message = std::format("Cannot open scene cut file for writing: {}",
                                 filepath),
      });
    }

    // Both RHS and coefficients are stored in fully physical space:
    //   rhs_csv   = rhs_lp × scale_objective
    //   coeff_csv = LP_coeff × scale_objective / col_scale
    const auto scale_obj = planning_lp().options().scale_objective();
    ofs << "# scale_objective=" << scale_obj << "\n";
    ofs << "phase,scene,name,rhs,coefficients\n";

    // Build phase UID → PhaseIndex lookup for scale retrieval
    const auto& sim = planning_lp().simulation();
    const auto num_phases = static_cast<Index>(sim.phases().size());
    std::unordered_map<int, PhaseIndex> phase_map;
    for (Index pi = 0; pi < num_phases; ++pi) {
      phase_map[static_cast<int>(sim.phases()[pi].uid())] = PhaseIndex {pi};
    }

    for (const auto& cut : cuts) {
      // RHS in physical objective units
      ofs << cut.phase << "," << cut.scene << "," << cut.name << ","
          << (cut.rhs * scale_obj);

      // Look up the LinearInterface to retrieve column scales.
      auto pit = phase_map.find(cut.phase);
      if (pit != phase_map.end()) {
        const auto& li =
            planning_lp().system(scene, pit->second).linear_interface();
        const auto& idx_to_name = li.col_index_to_name();
        for (const auto& [col, coeff] : cut.coefficients) {
          const auto scale = li.get_col_scale(ColIndex {col});
          ofs << ",";
          const auto ucol = static_cast<size_t>(col);
          if (ucol < idx_to_name.size() && !idx_to_name[ucol].empty()) {
            ofs << idx_to_name[ucol] << "/";
          }
          ofs << col << ":" << (coeff * scale_obj / scale);
        }
      } else {
        // Phase UID not found — should not happen for well-formed cuts.
        SPDLOG_WARN(
            "save_scene_cuts: unknown phase UID {} for cut '{}'; "
            "writing without variable scaling",
            cut.phase,
            cut.name);
        for (const auto& [col, coeff] : cut.coefficients) {
          ofs << "," << col << ":" << (coeff * scale_obj);
        }
      }
      ofs << "\n";
    }

    SPDLOG_TRACE("SDDP: saved {} cuts for scene UID {} to {}",
                 cuts.size(),
                 scene_uid(scene),
                 filepath);
    return {};

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("Error saving scene UID {} cuts to {}: {}",
                               scene_uid(scene),
                               directory,
                               e.what()),
    });
  }
}

auto SDDPSolver::save_all_scene_cuts(const std::string& directory) const
    -> std::expected<void, Error>
{
  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());

  for (Index si = 0; si < num_scenes; ++si) {
    auto result = save_scene_cuts(SceneIndex {si}, directory);
    if (!result.has_value()) {
      return result;
    }
  }
  return {};
}

auto SDDPSolver::load_cuts(const std::string& filepath)
    -> std::expected<int, Error>
{
  try {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message =
              std::format("Cannot open cut file for reading: {}", filepath),
      });
    }

    std::string line;
    // Skip metadata comments (# ...) and the CSV header line
    while (std::getline(ifs, line)) {
      if (line.empty() || line.starts_with('#')) {
        continue;
      }
      // First non-empty, non-comment line is the header — skip it
      break;
    }

    int cuts_loaded = 0;
    const auto& sim = planning_lp().simulation();
    const auto num_scenes = static_cast<Index>(sim.scenes().size());
    const auto num_phases = static_cast<Index>(sim.phases().size());
    const auto scale_obj = planning_lp().options().scale_objective();

    // Build phase UID → PhaseIndex lookup
    std::unordered_map<int, PhaseIndex> phase_uid_to_index;
    for (Index pi = 0; pi < num_phases; ++pi) {
      phase_uid_to_index[static_cast<int>(sim.phases()[pi].uid())] =
          PhaseIndex {pi};
    }

    // Process data lines
    while (std::getline(ifs, line)) {
      if (line.empty() || line.starts_with('#')) {
        continue;
      }

      // Parse CSV: phase,scene,name,rhs,col1:coeff1,...
      std::istringstream iss(line);
      std::string token;

      std::getline(iss, token, ',');
      const auto phase_val = std::stoi(token);

      std::getline(iss, token, ',');
      // scene is parsed but intentionally ignored: loaded cuts are
      // broadcast to all scenes as warm-start approximations.
      [[maybe_unused]] const auto scene_val = std::stoi(token);

      std::getline(iss, token, ',');
      const auto cut_name = token;

      std::getline(iss, token, ',');
      const auto rhs = std::stod(token);

      // RHS in CSV is in physical objective units; convert to LP space.
      auto row = SparseRow {
          .name = sddp_label("loaded", cut_name),
          .lowb = rhs / scale_obj,
          .uppb = LinearProblem::DblMax,
      };

      // Collect raw physical-space coefficients.
      // Supports two formats:
      //   name/col_idx:coeff  (named — preferred for portability)
      //   col_idx:coeff       (legacy — index-only)
      struct RawCoeff
      {
        std::string name {};  // empty if legacy format
        int col {};
        double coeff {};
      };
      std::vector<RawCoeff> raw_coeffs;
      while (std::getline(iss, token, ',')) {
        const auto colon = token.find(':');
        if (colon == std::string::npos) {
          continue;
        }
        const auto col_part = token.substr(0, colon);
        const auto coeff = std::stod(token.substr(colon + 1));
        if (const auto slash = col_part.find('/'); slash != std::string::npos) {
          // name/col_idx format
          raw_coeffs.push_back(RawCoeff {
              .name = col_part.substr(0, slash),
              .col = std::stoi(col_part.substr(slash + 1)),
              .coeff = coeff,
          });
        } else {
          // legacy col_idx format
          raw_coeffs.push_back(RawCoeff {
              .col = std::stoi(col_part),
              .coeff = coeff,
          });
        }
      }

      // Resolve the phase UID to a PhaseIndex
      auto pit = phase_uid_to_index.find(phase_val);
      if (pit == phase_uid_to_index.end()) {
        SPDLOG_WARN(
            "SDDP load_cuts: unknown phase UID {} in {}, skipping cut '{}'",
            phase_val,
            filepath,
            cut_name);
        continue;
      }
      const auto phase = pit->second;

      // Add the loaded cut to all scenes for this phase.
      // Coefficients in the CSV are in fully physical space; convert to LP
      // space: LP_coeff = phys_coeff × col_scale / scale_objective.
      // When a column name is available, resolve via name first; if the name
      // is not found in the current LP, fall back to the stored column index.
      for (Index si = 0; si < num_scenes; ++si) {
        auto& li =
            planning_lp().system(SceneIndex {si}, phase).linear_interface();
        const auto& name_map = li.col_name_map();
        auto scene_row = row;
        for (const auto& rc : raw_coeffs) {
          auto resolved_col = rc.col;
          if (!rc.name.empty()) {
            if (auto nit = name_map.find(rc.name); nit != name_map.end()) {
              resolved_col = nit->second;
            } else {
              SPDLOG_TRACE(
                  "SDDP load_cuts: col name '{}' not found in LP, "
                  "falling back to stored index {}",
                  rc.name,
                  rc.col);
            }
          }
          const auto col_idx = ColIndex {resolved_col};
          const auto scale = li.get_col_scale(col_idx);
          scene_row[col_idx] = rc.coeff * scale / scale_obj;
        }
        li.add_row(scene_row);
      }
      ++cuts_loaded;
    }

    SPDLOG_TRACE("SDDP: loaded {} cuts from {}", cuts_loaded, filepath);
    return cuts_loaded;

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message =
            std::format("Error loading cuts from {}: {}", filepath, e.what()),
    });
  }
}

auto SDDPSolver::load_scene_cuts_from_directory(const std::string& directory)
    -> std::expected<int, Error>
{
  int total_loaded = 0;

  if (!std::filesystem::exists(directory)) {
    return 0;  // No directory = no cuts to load (not an error)
  }

  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto filename = entry.path().filename().string();

    // Skip error files from infeasible scenes (previous runs)
    if (filename.starts_with("error_")) {
      SPDLOG_INFO("SDDP hot-start: skipping error file {}", filename);
      continue;
    }

    // Only load scene_N.csv files and the combined sddp_cuts.csv
    if (!filename.starts_with("scene_") && filename != sddp_file::combined_cuts)
    {
      continue;
    }
    if (!filename.ends_with(".csv")) {
      continue;
    }

    auto result = load_cuts(entry.path().string());
    if (result.has_value()) {
      total_loaded += *result;
      SPDLOG_TRACE("SDDP hot-start: loaded {} cuts from {}", *result, filename);
    } else {
      SPDLOG_WARN("SDDP hot-start: could not load {}: {}",
                  filename,
                  result.error().message);
    }
  }

  return total_loaded;
}

// ── State-variable name matching helper ─────────────────────────────────────
namespace
{
/// Returns true if *col_name* is a final-state-variable name that can
/// appear in boundary/hot-start cut CSV headers (efin, soc, vfin).
[[nodiscard]] constexpr auto is_final_state_col(std::string_view col_name)
    -> bool
{
  return col_name == "efin" || col_name == "soc" || col_name == "vfin";
}
}  // namespace

// ── Boundary (future-cost) cuts ─────────────────────────────────────────────

auto SDDPSolver::load_boundary_cuts(const std::string& filepath)
    -> std::expected<int, Error>
{
  // ── Mode check ────────────────────────────────────────────────────────────
  const auto& mode = m_options_.boundary_cuts_mode;
  if (mode == "noload") {
    SPDLOG_INFO("SDDP: boundary cuts mode is 'noload' — skipping");
    return 0;
  }

  const bool separated = (mode == "separated");

  try {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message = std::format(
              "Cannot open boundary cuts file for reading: {}", filepath),
      });
    }

    // ── Parse header ────────────────────────────────────────────────────────
    // Expected: name,iteration,scene,rhs,StateVar1,StateVar2,...
    // (Legacy format without iteration column is auto-detected.)
    std::string header_line;
    std::getline(ifs, header_line);

    std::vector<std::string> headers;
    {
      std::istringstream hss(header_line);
      std::string token;
      while (std::getline(hss, token, ',')) {
        headers.push_back(token);
      }
    }

    // Detect whether the CSV has the `iteration` column.
    // New format: name,iteration,scene,rhs,<state_vars>
    // Legacy:     name,scene,rhs,<state_vars>
    const bool has_iteration_col =
        (headers.size() >= 2 && headers[1] == "iteration");
    const int state_var_start = has_iteration_col ? 4 : 3;

    if (static_cast<int>(headers.size()) < state_var_start + 1) {
      return std::unexpected(Error {
          .code = ErrorCode::InvalidInput,
          .message =
              std::format("Boundary cuts CSV must have at least {} columns "
                          "(name,[iteration,]scene,rhs,<state_vars>); got {}",
                          state_var_start + 1,
                          headers.size()),
      });
    }

    // ── Determine last phase and build name→column mapping ──────────────────
    const auto& sim = planning_lp().simulation();
    const auto num_phases = static_cast<Index>(sim.phases().size());
    const auto num_scenes = static_cast<Index>(sim.scenes().size());
    const auto last_phase = PhaseIndex {num_phases - 1};

    // Build scene UID → SceneIndex lookup (for "separated" mode)
    std::unordered_map<int, Index> scene_uid_to_index;
    for (Index si = 0; si < num_scenes; ++si) {
      scene_uid_to_index[static_cast<int>(sim.scenes()[si].uid())] = si;
    }

    // Build element-name → uid lookup from the System.
    // We support Reservoir (via Junction) and Battery state variables.
    const auto& sys = planning_lp().planning().system;

    std::unordered_map<std::string, std::pair<std::string_view, Uid>>
        name_to_class_uid;
    for (const auto& junc : sys.junction_array) {
      name_to_class_uid[junc.name] = {"Junction", junc.uid};
    }
    for (const auto& bat : sys.battery_array) {
      name_to_class_uid[bat.name] = {"Battery", bat.uid};
    }

    // For each state-variable header column, find the corresponding LP column
    // in the last phase (scene 0 is used as representative for column lookup;
    // cuts are broadcast to all scenes).
    const auto& svar_map = sim.state_variables(SceneIndex {0}, last_phase);

    // Map from CSV column index → LP ColIndex in last phase
    const auto num_state_cols =
        static_cast<int>(headers.size()) - state_var_start;
    std::vector<std::optional<ColIndex>> header_col_map;
    header_col_map.reserve(num_state_cols);

    for (int hi = state_var_start; std::cmp_less(hi, headers.size()); ++hi) {
      const auto& hdr = headers[hi];
      std::optional<ColIndex> found_col;

      // Parse "ClassName:ElementName" or plain "ElementName"
      std::string_view class_filter;
      std::string element_name;
      if (const auto colon = hdr.find(':'); colon != std::string::npos) {
        class_filter = std::string_view(hdr).substr(0, colon);
        element_name = hdr.substr(colon + 1);
      } else {
        element_name = hdr;
      }

      // Look up element name → (class_name, uid)
      if (auto it = name_to_class_uid.find(element_name);
          it != name_to_class_uid.end())
      {
        const auto& [cname, elem_uid] = it->second;
        if (!class_filter.empty() && class_filter != cname) {
          SPDLOG_WARN(
              "Boundary cuts: header '{}' class '{}' does not match "
              "element '{}' class '{}'; skipping",
              hdr,
              class_filter,
              element_name,
              cname);
        } else {
          // Find the efin/vfin state variable for this element in the last
          // phase
          for (const auto& [key, svar] : svar_map) {
            if (key.uid == elem_uid && is_final_state_col(key.col_name)) {
              found_col = svar.col();
              break;
            }
          }
        }
      }

      if (!found_col.has_value()) {
        SPDLOG_WARN(
            "Boundary cuts: state variable '{}' not found in the current "
            "model (it may have been removed or renamed since the cuts were "
            "saved); its coefficients will be ignored in the loaded cuts",
            hdr);
      }
      header_col_map.push_back(found_col);
    }

    // ── Pre-scan: collect all iterations for max_iterations filtering ────────
    // If max_iterations > 0, we first read all rows to find the distinct
    // iteration numbers, then keep only those from the last N iterations.
    struct RawCut
    {
      std::string name;
      int iteration;
      int scene;  // scene UID (matched to scene_array in separated mode)
      double rhs;
      std::string coeff_line;  // everything after rhs (the coefficient fields)
    };

    std::vector<RawCut> raw_cuts;
    std::string line;
    while (std::getline(ifs, line)) {
      if (line.empty()) {
        continue;
      }

      std::istringstream iss(line);
      std::string token;

      // Column 0: name
      std::getline(iss, token, ',');
      auto cut_name = token;

      int iteration = 0;
      if (has_iteration_col) {
        // Column 1: iteration
        std::getline(iss, token, ',');
        iteration = std::stoi(token);
      }

      // Next column: scene UID (matched to scene_array in separated mode)
      std::getline(iss, token, ',');
      const auto scene_val = std::stoi(token);

      // Next column: rhs
      std::getline(iss, token, ',');
      const auto rhs = std::stod(token);

      // The rest of the line contains the coefficient values
      std::string remainder;
      std::getline(iss, remainder);

      raw_cuts.push_back(RawCut {
          .name = std::move(cut_name),
          .iteration = iteration,
          .scene = scene_val,
          .rhs = rhs,
          .coeff_line = std::move(remainder),
      });
    }

    // ── Filter by max_iterations ────────────────────────────────────────────
    const auto max_iters = m_options_.boundary_max_iterations;
    if (max_iters > 0 && has_iteration_col) {
      // Find distinct iteration numbers and keep the last N
      std::set<int> distinct_iters;
      for (const auto& rc : raw_cuts) {
        distinct_iters.insert(rc.iteration);
      }
      if (std::cmp_greater(distinct_iters.size(), max_iters)) {
        // Keep only the last max_iters iteration values
        std::set<int> keep_iters;
        auto it = distinct_iters.end();
        for (int i = 0; i < max_iters; ++i) {
          --it;
          keep_iters.insert(*it);
        }
        std::erase_if(raw_cuts,
                      [&keep_iters](const RawCut& rc)
                      { return !keep_iters.contains(rc.iteration); });
        SPDLOG_INFO(
            "SDDP: boundary cuts filtered to last {} iterations ({} cuts)",
            max_iters,
            raw_cuts.size());
      }
    }

    // ── Ensure the last phase has an alpha column ───────────────────────────
    for (Index si = 0; si < num_scenes; ++si) {
      const auto scene = SceneIndex {si};
      auto& state = m_scene_phase_states_[scene][last_phase];
      if (state.alpha_col == ColIndex {unknown_index}) {
        auto& li = planning_lp().system(scene, last_phase).linear_interface();
        state.alpha_col =
            li.add_col(sddp_label("sddp", "alpha", scene, last_phase),
                       m_options_.alpha_min,
                       m_options_.alpha_max);
        li.set_obj_coeff(state.alpha_col, 1.0);
      }
    }

    // ── Add cuts to the LP ──────────────────────────────────────────────────
    // Boundary cuts are expressed in fully physical space (PLP convention).
    // Convert to LP space: rhs_lp = rhs_phys / scale_obj,
    // LP_coeff = phys_coeff × col_scale / scale_obj.
    const auto scale_obj = planning_lp().options().scale_objective();
    int cuts_loaded = 0;
    for (const auto& rc : raw_cuts) {
      // Determine which scenes get this cut
      Index scene_start = 0;
      Index scene_end = num_scenes;
      if (separated) {
        // In "separated" mode, the scene column is a scene UID;
        // look up the corresponding SceneIndex via the scene_array.
        auto it = scene_uid_to_index.find(rc.scene);
        if (it == scene_uid_to_index.end()) {
          SPDLOG_TRACE(
              "Boundary cut '{}' scene UID {} not found in scene_array "
              "— skipping",
              rc.name,
              rc.scene);
          continue;
        }
        scene_start = it->second;
        scene_end = it->second + 1;
      }

      for (Index si = scene_start; si < scene_end; ++si) {
        const auto scene = SceneIndex {si};
        const auto& state = m_scene_phase_states_[scene][last_phase];

        auto row = SparseRow {
            .name = sddp_label("bdr", rc.name),
            .lowb = rc.rhs / scale_obj,
            .uppb = LinearProblem::DblMax,
        };
        row[state.alpha_col] = 1.0;

        // Parse coefficient values from the remainder string.
        // Boundary cuts express coefficients in physical space (e.g. $/dam³).
        // Convert to LP space: LP_coeff = phys_coeff × col_scale / scale_obj.
        // The α variable (col_scale=1) gets coefficient 1.0 structurally above;
        // state-variable columns are adjusted for both variable scale and
        // objective scale so the cut is correct in LP units.
        auto& li = planning_lp().system(scene, last_phase).linear_interface();
        std::istringstream coeff_ss(rc.coeff_line);
        std::string token;
        for (const auto& col_opt : header_col_map) {
          if (!std::getline(coeff_ss, token, ',')) {
            break;
          }
          if (!col_opt.has_value()) {
            continue;
          }
          const auto coeff = std::stod(token);
          if (coeff != 0.0) {
            const auto scale = li.get_col_scale(*col_opt);
            row[*col_opt] = -coeff * scale / scale_obj;
          }
        }

        li.add_row(row);
      }
      ++cuts_loaded;
    }

    SPDLOG_INFO("SDDP: loaded {} boundary cuts from {} (mode={}, max_iters={})",
                cuts_loaded,
                filepath,
                mode,
                max_iters);
    return cuts_loaded;

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format(
            "Error loading boundary cuts from {}: {}", filepath, e.what()),
    });
  }
}

// ── Named hot-start cuts (all phases, named state variables) ────────────────

auto SDDPSolver::load_named_cuts(const std::string& filepath)
    -> std::expected<int, Error>
{
  try {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message = std::format("Cannot open named cuts file for reading: {}",
                                 filepath),
      });
    }

    // ── Parse header ──────────────────────────────────────────────────────
    // Format: name,iteration,scene,phase,rhs,StateVar1,StateVar2,...
    std::string header_line;
    std::getline(ifs, header_line);

    std::vector<std::string> headers;
    {
      std::istringstream hss(header_line);
      std::string token;
      while (std::getline(hss, token, ',')) {
        headers.push_back(token);
      }
    }

    // Expect at least: name,iteration,scene,phase,rhs + 1 state var
    constexpr int kFixedCols = 5;  // name,iteration,scene,phase,rhs
    if (std::cmp_less(headers.size(), kFixedCols + 1)) {
      return std::unexpected(Error {
          .code = ErrorCode::InvalidInput,
          .message = std::format(
              "Named cuts CSV must have at least {} columns "
              "(name,iteration,scene,phase,rhs,<state_vars>); got {}",
              kFixedCols + 1,
              headers.size()),
      });
    }

    // Verify the phase column header
    if (headers[3] != "phase") {
      return std::unexpected(Error {
          .code = ErrorCode::InvalidInput,
          .message = std::format(
              "Named cuts CSV: expected column 4 to be 'phase', got '{}'",
              headers[3]),
      });
    }

    // ── Build element-name → uid lookup from the System ───────────────────
    const auto& sim = planning_lp().simulation();
    const auto num_phases = static_cast<Index>(sim.phases().size());
    const auto num_scenes = static_cast<Index>(sim.scenes().size());
    const auto& sys = planning_lp().planning().system;
    // Named cuts are in physical space; divide by scale_obj for LP space.
    const auto scale_obj = planning_lp().options().scale_objective();

    std::unordered_map<std::string, std::pair<std::string_view, Uid>>
        name_to_class_uid;
    for (const auto& junc : sys.junction_array) {
      name_to_class_uid[junc.name] = {"Junction", junc.uid};
    }
    for (const auto& bat : sys.battery_array) {
      name_to_class_uid[bat.name] = {"Battery", bat.uid};
    }

    // Build phase UID → PhaseIndex lookup
    std::unordered_map<int, PhaseIndex> phase_uid_to_index;
    for (Index pi = 0; pi < num_phases; ++pi) {
      phase_uid_to_index[static_cast<int>(sim.phases()[pi].uid())] =
          PhaseIndex {pi};
    }

    // ── Build per-phase state-variable column maps ────────────────────────
    // For each phase, map header column index → LP ColIndex
    const auto num_state_cols = static_cast<int>(headers.size()) - kFixedCols;

    // Cache per-phase column maps (lazy: built on first use)
    std::unordered_map<Index, std::vector<std::optional<ColIndex>>>
        phase_col_maps;

    auto get_col_map =
        [&](PhaseIndex phase) -> const std::vector<std::optional<ColIndex>>&
    {
      auto it = phase_col_maps.find(static_cast<Index>(phase));
      if (it != phase_col_maps.end()) {
        return it->second;
      }

      // Build mapping for this phase by scanning state variables
      const auto& svar_map = sim.state_variables(SceneIndex {0}, phase);

      std::vector<std::optional<ColIndex>> col_map;
      col_map.reserve(num_state_cols);

      for (int hi = kFixedCols; std::cmp_less(hi, headers.size()); ++hi) {
        const auto& hdr = headers[hi];
        std::optional<ColIndex> found_col;

        // Parse "ClassName:ElementName" or plain "ElementName"
        std::string_view class_filter;
        std::string element_name;
        if (const auto colon = hdr.find(':'); colon != std::string::npos) {
          class_filter = std::string_view(hdr).substr(0, colon);
          element_name = hdr.substr(colon + 1);
        } else {
          element_name = hdr;
        }

        if (auto nit = name_to_class_uid.find(element_name);
            nit != name_to_class_uid.end())
        {
          const auto& [cname, elem_uid] = nit->second;
          if (!class_filter.empty() && class_filter != cname) {
            SPDLOG_WARN(
                "Named cuts: header '{}' class '{}' != element '{}' "
                "class '{}'; skipping",
                hdr,
                class_filter,
                element_name,
                cname);
          } else {
            // Find the efin/soc/vfin state variable for this element
            for (const auto& [key, svar] : svar_map) {
              if (key.uid == elem_uid && is_final_state_col(key.col_name)) {
                found_col = svar.col();
                break;
              }
            }
          }
        }

        if (!found_col.has_value()) {
          SPDLOG_TRACE(
              "Named cuts: could not find state variable for header '{}' "
              "in phase {}; column ignored",
              hdr,
              static_cast<Index>(phase));
        }
        col_map.push_back(found_col);
      }

      auto [ins_it, _] =
          phase_col_maps.emplace(static_cast<Index>(phase), std::move(col_map));
      return ins_it->second;
    };

    // ── Read all cut rows ─────────────────────────────────────────────────
    int cuts_loaded = 0;
    std::string line;
    while (std::getline(ifs, line)) {
      if (line.empty()) {
        continue;
      }

      std::istringstream iss(line);
      std::string token;

      // Column 0: name
      std::getline(iss, token, ',');
      auto cut_name = token;

      // Column 1: iteration (parsed but not used for filtering yet)
      std::getline(iss, token, ',');
      [[maybe_unused]] const auto iteration = std::stoi(token);

      // Column 2: scene UID
      std::getline(iss, token, ',');
      [[maybe_unused]] const auto scene_val = std::stoi(token);

      // Column 3: phase UID
      std::getline(iss, token, ',');
      const auto phase_val = std::stoi(token);

      // Column 4: rhs
      std::getline(iss, token, ',');
      const auto rhs = std::stod(token);

      // Resolve phase UID → PhaseIndex
      auto pit = phase_uid_to_index.find(phase_val);
      if (pit == phase_uid_to_index.end()) {
        SPDLOG_WARN(
            "Named cuts: unknown phase UID {} in '{}', skipping cut '{}'",
            phase_val,
            filepath,
            cut_name);
        continue;
      }
      const auto phase = pit->second;

      // Ensure alpha variable exists for this phase in all scenes
      for (Index si = 0; si < num_scenes; ++si) {
        const auto scene = SceneIndex {si};
        auto& state = m_scene_phase_states_[scene][phase];
        if (state.alpha_col == ColIndex {unknown_index}) {
          auto& li = planning_lp().system(scene, phase).linear_interface();
          state.alpha_col =
              li.add_col(sddp_label("sddp", "alpha", scene, phase),
                         m_options_.alpha_min,
                         m_options_.alpha_max);
          li.set_obj_coeff(state.alpha_col, 1.0);
        }
      }

      // Get the column map for this phase
      const auto& col_map = get_col_map(phase);

      // Parse coefficient values and build the cut row
      std::string remainder;
      std::getline(iss, remainder);

      for (Index si = 0; si < num_scenes; ++si) {
        const auto scene = SceneIndex {si};
        const auto& state = m_scene_phase_states_[scene][phase];

        auto row = SparseRow {
            .name = sddp_label("named_hs", cut_name),
            .lowb = rhs / scale_obj,
            .uppb = LinearProblem::DblMax,
        };
        row[state.alpha_col] = 1.0;

        // Parse coefficient values from the remainder string.
        // Named cuts express coefficients in physical space (e.g. $/dam³).
        // Convert to LP space: LP_coeff = phys_coeff × col_scale / scale_obj.
        auto& li = planning_lp().system(scene, phase).linear_interface();
        std::istringstream coeff_ss(remainder);
        std::string ctok;
        for (const auto& col_opt : col_map) {
          if (!std::getline(coeff_ss, ctok, ',')) {
            break;
          }
          if (!col_opt.has_value()) {
            continue;
          }
          const auto coeff = std::stod(ctok);
          if (coeff != 0.0) {
            const auto scale = li.get_col_scale(*col_opt);
            row[*col_opt] = -coeff * scale / scale_obj;
          }
        }

        li.add_row(row);
      }
      ++cuts_loaded;
    }

    SPDLOG_INFO(
        "SDDP: loaded {} named hot-start cuts from {}", cuts_loaded, filepath);
    return cuts_loaded;

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format(
            "Error loading named cuts from {}: {}", filepath, e.what()),
    });
  }
}

// ── Monitoring API ───────────────────────────────────────────────────────────

void SDDPSolver::write_api_status(
    const std::string& status_file,
    const std::vector<SDDPIterationResult>& results,
    double elapsed_s,
    const SolverMonitor& monitor) const
{
  // Build JSON manually using std::format to avoid adding a new dependency.
  // This is monitoring output only — correctness over aesthetics.

  std::string json;
  json.reserve(4096);

  const auto now_ts = std::chrono::duration<double>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

  // Determine current state
  const auto iter = m_current_iteration_.load();
  const auto gap = m_current_gap_.load();
  const auto lb = m_current_lb_.load();
  const auto ub = m_current_ub_.load();
  const auto conv = m_converged_.load();

  const char* status_str = nullptr;
  if (conv) {
    status_str = "converged";
  } else if (iter == 0) {
    status_str = "initializing";
  } else {
    status_str = "running";
  }

  json += "{\n";
  json += std::format("  \"version\": 1,\n");
  json += std::format("  \"timestamp\": {:.3f},\n", now_ts);
  json += std::format("  \"elapsed_s\": {:.3f},\n", elapsed_s);
  json += std::format("  \"status\": \"{}\",\n", status_str);
  json += std::format("  \"iteration\": {},\n", iter);
  json += std::format("  \"lower_bound\": {:.6f},\n", lb);
  json += std::format("  \"upper_bound\": {:.6f},\n", ub);
  json += std::format("  \"gap\": {:.6f},\n", gap);
  json += std::format("  \"converged\": {},\n", conv ? "true" : "false");
  json += std::format("  \"max_iterations\": {},\n", m_options_.max_iterations);
  json += std::format("  \"min_iterations\": {},\n", m_options_.min_iterations);

  // ── Iteration history ──
  json += "  \"history\": [\n";
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& r = results[i];
    json += "    {\n";
    json += std::format("      \"iteration\": {},\n", r.iteration);
    json += std::format("      \"lower_bound\": {:.6f},\n", r.lower_bound);
    json += std::format("      \"upper_bound\": {:.6f},\n", r.upper_bound);
    json += std::format("      \"gap\": {:.6f},\n", r.gap);
    json += std::format("      \"converged\": {},\n",
                        r.converged ? "true" : "false");
    json += std::format("      \"cuts_added\": {},\n", r.cuts_added);
    json += std::format("      \"infeasible_cuts_added\": {},\n",
                        r.infeasible_cuts_added);
    json +=
        std::format("      \"forward_pass_s\": {:.4f},\n", r.forward_pass_s);
    json +=
        std::format("      \"backward_pass_s\": {:.4f},\n", r.backward_pass_s);
    json += std::format("      \"iteration_s\": {:.4f},\n", r.iteration_s);

    // Per-scene upper bounds
    json += "      \"scene_upper_bounds\": [";
    for (std::size_t si = 0; si < r.scene_upper_bounds.size(); ++si) {
      if (si > 0) {
        json += ", ";
      }
      json += std::format("{:.6f}", r.scene_upper_bounds[si]);
    }
    json += "],\n";

    // Per-scene lower bounds
    json += "      \"scene_lower_bounds\": [";
    for (std::size_t si = 0; si < r.scene_lower_bounds.size(); ++si) {
      if (si > 0) {
        json += ", ";
      }
      json += std::format("{:.6f}", r.scene_lower_bounds[si]);
    }
    json += "]\n";

    json += (i + 1 < results.size()) ? "    },\n" : "    }\n";
  }
  json += "  ],\n";

  // ── Real-time workpool monitoring history ──
  monitor.append_history_json(json);

  json += "}\n";

  // Write atomically via SolverMonitor::write_status (write tmp, rename)
  SolverMonitor::write_status(json, status_file);
}

// ─── Private helper method implementations ───────────────────────────────────

auto SDDPSolver::validate_inputs() const -> std::optional<Error>
{
  const auto& sim = planning_lp().simulation();
  if (sim.scenes().empty()) {
    return Error {
        .code = ErrorCode::InvalidInput,
        .message = "No scenes in simulation",
    };
  }
  if (sim.phases().size() < 2) {
    return Error {
        .code = ErrorCode::InvalidInput,
        .message = "SDDP requires at least 2 phases",
    };
  }
  return std::nullopt;
}

auto SDDPSolver::initialize_solver() -> std::expected<void, Error>
{
  if (m_initialized_) {
    SPDLOG_DEBUG("SDDP: already initialized, skipping re-initialization");
    return {};
  }

  SPDLOG_INFO("SDDP: initializing solver (no initial solve pass)");

  const auto& sim = planning_lp().simulation();
  const auto num_scenes = static_cast<Index>(sim.scenes().size());
  const auto num_phases = static_cast<Index>(sim.phases().size());

  SPDLOG_INFO("SDDP: {} scene(s), {} phase(s)", num_scenes, num_phases);

  m_scene_phase_states_.resize(num_scenes);
  m_scene_cuts_.resize(num_scenes);
  m_infeasibility_counter_.resize(num_scenes);
  for (Index si = 0; si < num_scenes; ++si) {
    m_infeasibility_counter_[SceneIndex {si}].resize(num_phases, 0);
  }

  SPDLOG_INFO("SDDP: adding alpha variables and collecting state links");
  for (Index si = 0; si < num_scenes; ++si) {
    const auto scene = SceneIndex {si};
    initialize_alpha_variables(scene);
    collect_state_variable_links(scene);
    SPDLOG_DEBUG("SDDP: scene {} initialized ({} state links)",
                 scene_uid(scene),
                 m_scene_phase_states_[scene].empty()
                     ? 0
                     : m_scene_phase_states_[scene][PhaseIndex {0}]
                           .outgoing_links.size());
  }

  if (!m_options_.cuts_input_file.empty()) {
    auto result = load_cuts(m_options_.cuts_input_file);
    if (result.has_value()) {
      SPDLOG_INFO("SDDP hot-start: loaded {} cuts", *result);
    } else {
      SPDLOG_WARN("SDDP hot-start: could not load cuts: {}",
                  result.error().message);
    }
  } else if (!m_options_.cuts_output_file.empty()) {
    const auto cut_dir =
        std::filesystem::path(m_options_.cuts_output_file).parent_path();
    if (!cut_dir.empty() && std::filesystem::exists(cut_dir)) {
      auto result = load_scene_cuts_from_directory(cut_dir.string());
      if (result.has_value() && *result > 0) {
        SPDLOG_INFO("SDDP hot-start: loaded {} cuts from {}",
                    *result,
                    cut_dir.string());
      }
    }
  }

  // ── Load boundary cuts (future-cost function for last phase) ──────────────
  if (!m_options_.boundary_cuts_file.empty()) {
    auto result = load_boundary_cuts(m_options_.boundary_cuts_file);
    if (result.has_value()) {
      SPDLOG_INFO("SDDP: loaded {} boundary cuts from {}",
                  *result,
                  m_options_.boundary_cuts_file);
    } else {
      SPDLOG_WARN("SDDP: could not load boundary cuts: {}",
                  result.error().message);
    }
  }

  // ── Load named hot-start cuts (all phases, named state variables) ─────────
  if (!m_options_.named_cuts_file.empty()) {
    auto result = load_named_cuts(m_options_.named_cuts_file);
    if (result.has_value()) {
      SPDLOG_INFO("SDDP: loaded {} named hot-start cuts from {}",
                  *result,
                  m_options_.named_cuts_file);
    } else {
      SPDLOG_WARN("SDDP: could not load named hot-start cuts: {}",
                  result.error().message);
    }
  }

  m_initialized_ = true;

  SPDLOG_INFO("SDDP: updating initial LP coefficients for all phases");
  for (Index si = 0; si < num_scenes; ++si) {
    for (Index pi = 0; pi < num_phases; ++pi) {
      update_coefficients_for_phase(SceneIndex {si}, PhaseIndex {pi}, 0);
    }
  }

  SPDLOG_INFO("SDDP: initialization complete");
  return {};
}

void SDDPSolver::reset_live_state() noexcept
{
  m_current_iteration_.store(0);
  m_current_gap_.store(1.0);
  m_current_lb_.store(0.0);
  m_current_ub_.store(0.0);
  m_converged_.store(false);
}

auto SDDPSolver::run_forward_pass_all_scenes(int iter,
                                             SDDPWorkPool& pool,
                                             const SolverOptions& opts)
    -> std::expected<ForwardPassOutcome, Error>
{
  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());

  const auto fwd_start = std::chrono::steady_clock::now();
  std::vector<std::future<std::expected<double, Error>>> futures;
  futures.reserve(num_scenes);

  // Forward-pass scene tasks use High priority; lower iteration = higher key.
  const auto fwd_req = make_forward_lp_task_req(iter, PhaseIndex {0});

  for (Index si = 0; si < num_scenes; ++si) {
    const auto scene = SceneIndex {si};
    auto fut = pool.submit([this, scene, iter, &opts]
                           { return forward_pass(scene, iter, opts); },
                           fwd_req);
    futures.push_back(std::move(fut.value()));
  }

  ForwardPassOutcome out;
  out.scene_upper_bounds.resize(num_scenes, 0.0);
  out.scene_feasible.resize(num_scenes, 1);

  for (Index si = 0; si < num_scenes; ++si) {
    auto fwd = futures[static_cast<std::size_t>(si)].get();
    if (!fwd.has_value()) {
      SPDLOG_WARN("SDDP forward: scene {} failed: {}", si, fwd.error().message);
      out.has_feasibility_issue = true;
      out.scene_feasible[static_cast<std::size_t>(si)] = 0;
      continue;
    }
    out.scene_upper_bounds[static_cast<std::size_t>(si)] = *fwd;
    ++out.scenes_solved;
  }

  out.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now()
                                                - fwd_start)
                      .count();

  if (out.scenes_solved == 0) {
    return std::unexpected(Error {
        .code = ErrorCode::SolverError,
        .message = "SDDP: all scenes infeasible in forward pass",
    });
  }

  return out;
}

auto SDDPSolver::run_backward_pass_all_scenes(
    std::span<const uint8_t> scene_feasible,
    SDDPWorkPool& pool,
    const SolverOptions& opts,
    int iter) -> BackwardPassOutcome
{
  // When cut sharing is enabled, use the phase-synchronized backward pass:
  // all scenes complete a phase before cuts are shared and the next phase
  // is processed.  When sharing is disabled (None), scenes run their full
  // backward pass independently in parallel with no synchronization.
  if (m_options_.cut_sharing != CutSharingMode::None) {
    return run_backward_pass_synchronized(scene_feasible, pool, opts, iter);
  }

  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());

  const auto bwd_start = std::chrono::steady_clock::now();
  std::vector<std::future<std::expected<int, Error>>> futures;
  futures.reserve(num_scenes);

  // Backward-pass scene tasks use Medium priority; scenes with lower index
  // get slightly higher priority_key (phase 0 = lowest phase index).
  const auto bwd_req = make_backward_lp_task_req(iter, PhaseIndex {0});

  for (Index si = 0; si < num_scenes; ++si) {
    if (scene_feasible[static_cast<std::size_t>(si)] == 0U) {
      continue;
    }
    const auto scene = SceneIndex {si};
    auto fut = (m_options_.num_apertures != 0)
        ? pool.submit(
              [this, scene, &opts, iter]
              { return backward_pass_with_apertures(scene, opts, iter); },
              bwd_req)
        : pool.submit([this, scene, &opts, iter]
                      { return backward_pass(scene, opts, iter); },
                      bwd_req);
    futures.push_back(std::move(fut.value()));
  }

  BackwardPassOutcome out;
  for (auto& fut : futures) {
    auto bwd = fut.get();
    if (!bwd.has_value()) {
      SPDLOG_WARN("SDDP backward: failed: {}", bwd.error().message);
      out.has_feasibility_issue = true;
      continue;
    }
    out.total_cuts += *bwd;
  }

  out.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now()
                                                - bwd_start)
                      .count();
  return out;
}

// ── Phase-synchronized backward pass (for cut sharing) ──────────────────────

auto SDDPSolver::run_backward_pass_synchronized(
    std::span<const uint8_t> scene_feasible,
    SDDPWorkPool& pool,
    const SolverOptions& opts,
    int iter) -> BackwardPassOutcome
{
  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());
  const auto num_phases =
      static_cast<Index>(planning_lp().simulation().phases().size());

  const auto bwd_start = std::chrono::steady_clock::now();
  BackwardPassOutcome out;

  // Per-scene cumulative cut count for unique cut labels across phase steps
  std::vector<int> per_scene_cut_count(static_cast<std::size_t>(num_scenes), 0);

  const bool use_apertures = (m_options_.num_apertures != 0);

  // Process phases backward: all scenes complete one phase before
  // sharing cuts and moving to the previous phase.
  for (const auto pi : std::views::iota(1, num_phases) | std::views::reverse) {
    const auto phase = PhaseIndex {pi};
    const auto cuts_before_step = m_stored_cuts_.size();

    // Submit all feasible scenes for this phase step in parallel
    std::vector<std::pair<Index, std::future<std::expected<int, Error>>>>
        futures;
    futures.reserve(num_scenes);

    const auto bwd_req = make_backward_lp_task_req(iter, PhaseIndex {pi});

    for (Index si = 0; si < num_scenes; ++si) {
      if (scene_feasible[static_cast<std::size_t>(si)] == 0U) {
        continue;
      }
      const auto scene = SceneIndex {si};
      const int offset = per_scene_cut_count[static_cast<std::size_t>(si)];

      auto fut = use_apertures
          ? pool.submit(
                [this, scene, phase, offset, &opts, iter]
                {
                  return backward_pass_with_apertures_single_phase(
                      scene, phase, offset, opts, iter);
                },
                bwd_req)
          : pool.submit(
                [this, scene, phase, offset, &opts, iter]
                {
                  return backward_pass_single_phase(
                      scene, phase, offset, opts, iter);
                },
                bwd_req);
      futures.emplace_back(si, std::move(fut.value()));
    }

    // Wait for all scenes to complete this phase step
    for (auto& [si, fut] : futures) {
      auto step_result = fut.get();
      if (!step_result.has_value()) {
        SPDLOG_WARN("SDDP backward synchronized: scene {} phase {} failed: {}",
                    si,
                    pi,
                    step_result.error().message);
        out.has_feasibility_issue = true;
        continue;
      }
      out.total_cuts += *step_result;
      per_scene_cut_count[static_cast<std::size_t>(si)] += *step_result;
    }

    // Share optimality cuts generated in this phase step across all scenes.
    // Feasibility cuts are not stored via store_cut() and thus are not shared.
    const auto src_phase = PhaseIndex {pi - 1};

    StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
    scene_cuts.resize(num_scenes);

    {
      const std::scoped_lock lock(m_cuts_mutex_);
      for (std::size_t ci = cuts_before_step; ci < m_stored_cuts_.size(); ++ci)
      {
        const auto& sc = m_stored_cuts_[ci];
        if (sc.phase != static_cast<int>(src_phase)) {
          continue;
        }
        auto row = SparseRow {
            .name = sc.name,
            .lowb = sc.rhs,
            .uppb = LinearProblem::DblMax,
        };
        for (const auto& [col, coeff] : sc.coefficients) {
          row[ColIndex {col}] = coeff;
        }
        if (sc.scene >= 0 && sc.scene < num_scenes) {
          scene_cuts[SceneIndex {sc.scene}].push_back(std::move(row));
        }
      }
    }

    share_cuts_for_phase(src_phase, scene_cuts, iter);

    SPDLOG_TRACE(
        "SDDP backward synchronized: phase {} cuts shared across {} scenes",
        pi,
        num_scenes);
  }

  out.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now()
                                                - bwd_start)
                      .count();
  return out;
}

void SDDPSolver::compute_iteration_bounds(
    SDDPIterationResult& ir,
    std::span<const uint8_t> scene_feasible,
    std::span<const double> weights) const
{
  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());

  double weighted_upper = 0.0;
  for (Index si = 0; si < num_scenes; ++si) {
    weighted_upper += weights[static_cast<std::size_t>(si)]
        * ir.scene_upper_bounds[static_cast<std::size_t>(si)];
  }
  ir.upper_bound = weighted_upper;

  ir.scene_lower_bounds.resize(num_scenes, 0.0);
  double weighted_lower = 0.0;
  for (Index si = 0; si < num_scenes; ++si) {
    if (scene_feasible[static_cast<std::size_t>(si)] == 0U) {
      continue;
    }
    const double lb_si = planning_lp()
                             .system(SceneIndex {si}, PhaseIndex {0})
                             .linear_interface()
                             .get_obj_value();
    ir.scene_lower_bounds[static_cast<std::size_t>(si)] = lb_si;
    weighted_lower += weights[static_cast<std::size_t>(si)] * lb_si;
  }
  ir.lower_bound = weighted_lower;
}

void SDDPSolver::apply_cut_sharing_for_iteration(std::size_t cuts_before,
                                                 int iteration)
{
  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());
  const auto num_phases =
      static_cast<Index>(planning_lp().simulation().phases().size());

  if (m_options_.cut_sharing == CutSharingMode::None || num_scenes <= 1) {
    return;
  }

  for (Index pi = 0; pi < num_phases - 1; ++pi) {
    StrongIndexVector<SceneIndex, std::vector<SparseRow>> scene_cuts;
    scene_cuts.resize(num_scenes);

    for (std::size_t ci = cuts_before; ci < m_stored_cuts_.size(); ++ci) {
      const auto& sc = m_stored_cuts_[ci];
      if (sc.phase != static_cast<int>(pi)) {
        continue;
      }
      auto row = SparseRow {
          .name = sc.name,
          .lowb = sc.rhs,
          .uppb = LinearProblem::DblMax,
      };
      for (const auto& [col, coeff] : sc.coefficients) {
        row[ColIndex {col}] = coeff;
      }
      if (sc.scene >= 0 && sc.scene < num_scenes) {
        scene_cuts[SceneIndex {sc.scene}].push_back(std::move(row));
      }
    }

    share_cuts_for_phase(PhaseIndex {pi}, scene_cuts, iteration);
  }
}

void SDDPSolver::finalize_iteration_result(SDDPIterationResult& ir, int iter)
{
  ir.gap = compute_convergence_gap(ir.upper_bound, ir.lower_bound);
  // Only declare convergence if both the gap tolerance is met AND
  // we have completed at least min_iterations (default 2).
  ir.converged = (ir.gap < m_options_.convergence_tol)
      && (iter >= m_options_.min_iterations);

  m_current_iteration_.store(iter);
  m_current_gap_.store(ir.gap);
  m_current_lb_.store(ir.lower_bound);
  m_current_ub_.store(ir.upper_bound);
  m_converged_.store(ir.converged);

  SPDLOG_TRACE(
      "SDDP iter {}: LB={:.4f} UB={:.4f} gap={:.6f} cuts={} "
      "infeas_cuts={} fwd={:.3f}s bwd={:.3f}s total={:.3f}s{}",
      iter,
      ir.lower_bound,
      ir.upper_bound,
      ir.gap,
      ir.cuts_added,
      ir.infeasible_cuts_added,
      ir.forward_pass_s,
      ir.backward_pass_s,
      ir.iteration_s,
      ir.converged ? " [CONVERGED]" : "");

  SPDLOG_INFO("SDDP iter {}: gap={:.6f} ({:.3f}s){}",
              iter,
              ir.gap,
              ir.iteration_s,
              ir.converged ? " [CONVERGED]" : "");
}

void SDDPSolver::maybe_write_api_status(
    const std::string& status_file,
    const std::vector<SDDPIterationResult>& results,
    std::chrono::steady_clock::time_point solve_start,
    const SolverMonitor& monitor) const
{
  if (!m_options_.enable_api || status_file.empty()) {
    return;
  }
  const double elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - solve_start)
                             .count();
  write_api_status(status_file, results, elapsed, monitor);
}

void SDDPSolver::save_cuts_for_iteration(
    int iter, std::span<const uint8_t> scene_feasible)
{
  if (m_options_.cuts_output_file.empty()) {
    return;
  }

  auto result = save_cuts(m_options_.cuts_output_file);
  if (!result.has_value()) {
    SPDLOG_WARN("SDDP: could not save cuts at iter {}: {}",
                iter,
                result.error().message);
  }

  const auto cut_dir =
      std::filesystem::path(m_options_.cuts_output_file).parent_path();
  if (cut_dir.empty()) {
    return;
  }

  auto scene_result = save_all_scene_cuts(cut_dir.string());
  if (!scene_result.has_value()) {
    SPDLOG_WARN("SDDP: could not save per-scene cuts at iter {}: {}",
                iter,
                scene_result.error().message);
  }

  const auto num_scenes =
      static_cast<Index>(planning_lp().simulation().scenes().size());
  for (Index si = 0; si < num_scenes; ++si) {
    if (scene_feasible[static_cast<std::size_t>(si)] != 0U) {
      continue;
    }
    const auto suid = scene_uid(SceneIndex {si});
    const auto scene_file =
        cut_dir / std::format(sddp_file::scene_cuts_fmt, suid);
    const auto error_file =
        cut_dir / std::format(sddp_file::error_scene_cuts_fmt, suid);
    std::error_code ec;
    if (std::filesystem::exists(scene_file, ec)) {
      std::filesystem::rename(scene_file, error_file, ec);
      if (!ec) {
        SPDLOG_TRACE("SDDP: renamed cut file for infeasible scene {} to {}",
                     si,
                     error_file.string());
      }
    }
  }
}

// ── Main solve loop ─────────────────────────────────────────────────────────

auto SDDPSolver::solve(const SolverOptions& lp_opts)
    -> std::expected<std::vector<SDDPIterationResult>, Error>
{
  // Validate preconditions
  if (auto err = validate_inputs()) {
    return std::unexpected(std::move(*err));
  }

  // just_build_lp: the LP matrix is already built (PlanningLP constructor).
  // Return an empty results vector immediately — no solving, no initialization.
  if (m_options_.just_build_lp) {
    SPDLOG_INFO("SDDP: just_build_lp mode — LP built, skipping all solving");
    return std::vector<SDDPIterationResult> {};
  }

  // Bootstrap LP + initialize α vars, state links, hot-start cuts
  if (auto err = initialize_solver(); !err.has_value()) {
    return std::unexpected(std::move(err.error()));
  }

  // Set up work pools for parallel scene processing:
  //  - sddp_pool: SDDPWorkPool with tuple key for main LP solve ordering
  //  - aux_pool:  AdaptiveWorkPool (int64_t key) for BendersCut and
  //               LpDebugWriter (gzip compression)
  auto sddp_pool = make_sddp_work_pool();
  auto aux_pool = make_solver_work_pool();
  m_pool_ = sddp_pool.get();
  m_aux_pool_ = aux_pool.get();
  m_benders_cut_.set_pool(m_aux_pool_);
  m_lp_debug_writer_ = LpDebugWriter(
      m_options_.log_directory, m_options_.lp_debug_compression, m_aux_pool_);

  reset_live_state();

  // Monitoring setup
  const auto solve_start = std::chrono::steady_clock::now();
  const std::string& status_file = m_options_.api_status_file;
  SolverMonitor monitor(m_options_.api_update_interval);
  if (m_options_.enable_api && !status_file.empty()) {
    monitor.start(*sddp_pool, solve_start, "SDDPMonitor");
  }

  std::vector<SDDPIterationResult> results;
  results.reserve(m_options_.max_iterations);

  for (int iter = 1; iter <= m_options_.max_iterations; ++iter) {
    const auto iter_start = std::chrono::steady_clock::now();

    if (should_stop()) {
      SPDLOG_INFO("SDDP: stop requested, halting after {} iterations",
                  iter - 1);
      break;
    }

    SPDLOG_INFO(
        "SDDP: === iteration {} / {} ===", iter, m_options_.max_iterations);

    SDDPIterationResult ir {
        .iteration = iter,
    };
    m_benders_cut_.reset_infeasible_cut_count();

    // ── Forward pass ──
    SPDLOG_DEBUG("SDDP: starting forward pass (iter {})", iter);
    auto fwd = run_forward_pass_all_scenes(iter, *sddp_pool, lp_opts);
    if (!fwd.has_value()) {
      monitor.stop();
      return std::unexpected(std::move(fwd.error()));
    }
    ir.scene_upper_bounds = std::move(fwd->scene_upper_bounds);
    ir.forward_pass_s = fwd->elapsed_s;
    if (fwd->has_feasibility_issue) {
      ir.feasibility_issue = true;
      SPDLOG_INFO("SDDP: iter {} forward pass has feasibility issues", iter);
    }
    SPDLOG_DEBUG("SDDP: forward pass done in {:.3f}s", fwd->elapsed_s);

    // ── Scene weights and bounds ──
    const auto& scenes = planning_lp().simulation().scenes();
    const auto weights = compute_scene_weights(scenes, fwd->scene_feasible);
    compute_iteration_bounds(ir, fwd->scene_feasible, weights);

    // ── Backward pass ──
    SPDLOG_DEBUG("SDDP: starting backward pass (iter {})", iter);
    const auto cuts_before = m_stored_cuts_.size();
    auto bwd = run_backward_pass_all_scenes(
        fwd->scene_feasible, *sddp_pool, lp_opts, iter);
    ir.cuts_added = bwd.total_cuts;
    ir.infeasible_cuts_added = m_benders_cut_.infeasible_cut_count();
    ir.backward_pass_s = bwd.elapsed_s;
    if (bwd.has_feasibility_issue) {
      ir.feasibility_issue = true;
      SPDLOG_INFO("SDDP: iter {} backward pass has feasibility issues", iter);
    }
    ir.iteration_s = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - iter_start)
                         .count();
    SPDLOG_DEBUG("SDDP: backward pass done in {:.3f}s, {} cuts added",
                 bwd.elapsed_s,
                 bwd.total_cuts);

    // ── Cut sharing ──
    // When cut sharing is enabled (non-None), the phase-synchronized backward
    // pass (run_backward_pass_synchronized) already shares cuts at each phase.
    // Only apply post-hoc sharing when scenes ran independently (None mode),
    // which is a no-op anyway since apply_cut_sharing_for_iteration returns
    // early for None.  This guard prevents double-sharing of cuts.
    if (m_options_.cut_sharing == CutSharingMode::None) {
      apply_cut_sharing_for_iteration(cuts_before, iter);
    }

    // ── Convergence + live-query update ──
    finalize_iteration_result(ir, iter);
    results.push_back(ir);

    SPDLOG_INFO(
        "SDDP: iter {} done in {:.3f}s — UB={:.4f} LB={:.4f} "
        "gap={:.6f} cuts={} infeas_cuts={} {}",
        iter,
        ir.iteration_s,
        ir.upper_bound,
        ir.lower_bound,
        ir.gap,
        ir.cuts_added,
        ir.infeasible_cuts_added,
        ir.converged ? "[CONVERGED]" : "");

    // ── Monitoring API and cut persistence ──
    maybe_write_api_status(status_file, results, solve_start, monitor);
    save_cuts_for_iteration(iter, fwd->scene_feasible);

    // ── Iteration callback ──
    if (m_iteration_callback_ && m_iteration_callback_(ir)) {
      SPDLOG_INFO("SDDP: callback requested stop at iter {}", iter);
      break;
    }

    if (ir.converged) {
      break;
    }
  }

  monitor.stop();
  m_lp_debug_writer_.drain();
  m_benders_cut_.set_pool(nullptr);
  m_pool_ = nullptr;
  m_aux_pool_ = nullptr;
  m_lp_debug_writer_ = {};

  return results;
}

// ─── SDDPPlanningSolver ─────────────────────────────────────────────────────

SDDPPlanningSolver::SDDPPlanningSolver(SDDPOptions opts) noexcept
    : m_sddp_opts_(std::move(opts))
{
}

auto SDDPPlanningSolver::solve(PlanningLP& planning_lp,
                               const SolverOptions& opts)
    -> std::expected<int, Error>
{
  const auto num_scenes = static_cast<int>(planning_lp.systems().size());
  const auto num_phases = num_scenes > 0
      ? static_cast<int>(planning_lp.systems().front().size())
      : 0;
  SPDLOG_INFO(
      "SDDPSolver: starting {} scene(s) × {} phase(s)", num_scenes, num_phases);

  // just_build_lp: LP already built in PlanningLP constructor — skip all
  // solving.
  if (m_sddp_opts_.just_build_lp) {
    SPDLOG_INFO("SDDP: just_build_lp mode — LP built, skipping solve");
    return 0;
  }

  SDDPSolver sddp(planning_lp, m_sddp_opts_);
  auto results = sddp.solve(opts);

  if (!results.has_value()) {
    return std::unexpected(std::move(results.error()));
  }

  m_last_results_ = std::move(*results);

  // Return 1 if converged, 0 otherwise
  if (!m_last_results_.empty() && m_last_results_.back().converged) {
    return 1;
  }

  return std::unexpected(Error {
      .code = ErrorCode::SolverError,
      .message = std::format(
          "SDDP did not converge after {} iterations (gap={:.6f})",
          m_last_results_.empty() ? 0 : m_last_results_.back().iteration,
          m_last_results_.empty() ? 1.0 : m_last_results_.back().gap),
  });
}

// ── Aperture per-phase implementation (shared by single-phase and full pass)

auto SDDPSolver::backward_pass_aperture_phase_impl(
    SceneIndex scene,
    PhaseIndex phase,
    int cut_offset,
    const ScenarioLP& base_scenario,
    std::span<const ScenarioLP> all_scenarios,
    std::span<const Aperture> aperture_defs,
    const SolverOptions& opts,
    int iteration) -> std::expected<int, Error>
{
  auto& phase_states = m_scene_phase_states_[scene];
  int cuts_added = 0;

  const auto pi = static_cast<Index>(phase);
  const auto src_phase = PhaseIndex {pi - 1};
  auto& src_li = planning_lp().system(scene, src_phase).linear_interface();
  const auto& src_state = phase_states[src_phase];
  const auto& phase_lp = planning_lp().simulation().phases()[phase];

  auto expected_cut = solve_apertures_for_phase(scene,
                                                phase,
                                                src_state,
                                                base_scenario,
                                                all_scenarios,
                                                aperture_defs,
                                                phase_lp.aperture_set(),
                                                cut_offset,
                                                opts);

  if (!expected_cut.has_value()) {
    // Fallback: build a regular Benders cut from the cached
    // forward-pass reduced costs and objective (same as backward_pass).
    const auto& target_state = phase_states[phase];
    auto fallback_cut = build_benders_cut(
        src_state.alpha_col,
        src_state.outgoing_links,
        target_state.forward_col_cost,
        target_state.forward_full_obj,
        sddp_label("sddp", "fcut", scene, pi, iteration, cut_offset));

    store_cut(scene, src_phase, fallback_cut);
    src_li.add_row(fallback_cut);
    ++cuts_added;

    SPDLOG_TRACE("SDDP aperture fallback: scene {} cut for phase {} rhs={:.4f}",
                 scene,
                 src_phase,
                 fallback_cut.lowb);

    if (pi > 1) {
      auto r = resolve_via_pool(
          src_li, opts, make_backward_lp_task_req(iteration, src_phase));
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

  store_cut(scene, src_phase, *expected_cut);
  src_li.add_row(*expected_cut);
  ++cuts_added;

  SPDLOG_TRACE("SDDP aperture: scene {} cut for phase {} rhs={:.4f}",
               scene,
               src_phase,
               expected_cut->lowb);

  // Re-solve source phase after adding the cut to propagate feasibility.
  // Feasibility cuts are never shared between scenes.
  if (pi > 1) {
    auto r = resolve_via_pool(
        src_li, opts, make_backward_lp_task_req(iteration, src_phase));
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

auto SDDPSolver::backward_pass_with_apertures_single_phase(
    SceneIndex scene,
    PhaseIndex phase,
    int cut_offset,
    const SolverOptions& opts,
    int iteration) -> std::expected<int, Error>
{
  const auto& simulation = planning_lp().simulation();
  const auto& all_scenarios = simulation.scenarios();
  const auto num_all_scenarios = static_cast<int>(all_scenarios.size());
  const auto& aperture_defs = simulation.apertures();

  // No apertures → delegate to the regular single-phase step
  if (aperture_defs.empty()) {
    const int n_aps = (m_options_.num_apertures < 0)
        ? num_all_scenarios
        : std::min(m_options_.num_apertures, num_all_scenarios);

    if (n_aps <= 0 || num_all_scenarios == 0) {
      return backward_pass_single_phase(
          scene, phase, cut_offset, opts, iteration);
    }

    // Build synthetic aperture definitions from the first N scenarios
    Array<Aperture> synthetic;
    synthetic.reserve(static_cast<size_t>(n_aps));
    const double prob = 1.0 / static_cast<double>(n_aps);
    for (int i = 0; i < n_aps; ++i) {
      const Uid scen_uid =
          static_cast<Uid>(all_scenarios[ScenarioIndex {i}].uid());
      synthetic.push_back(Aperture {
          .uid = scen_uid,
          .source_scenario = scen_uid,
          .probability_factor = prob,
      });
    }

    const auto& scene_lp = simulation.scenes()[scene];
    const auto& scene_scenarios = scene_lp.scenarios();
    if (scene_scenarios.empty()) {
      return backward_pass_single_phase(
          scene, phase, cut_offset, opts, iteration);
    }

    return backward_pass_aperture_phase_impl(
        scene,
        phase,
        cut_offset,
        scene_scenarios.front(),
        all_scenarios,
        std::span<const Aperture>(synthetic),
        opts,
        iteration);
  }

  // Explicit aperture array
  const auto& scene_lp = simulation.scenes()[scene];
  const auto& scene_scenarios = scene_lp.scenarios();
  if (scene_scenarios.empty()) {
    return backward_pass_single_phase(
        scene, phase, cut_offset, opts, iteration);
  }

  return backward_pass_aperture_phase_impl(scene,
                                           phase,
                                           cut_offset,
                                           scene_scenarios.front(),
                                           all_scenarios,
                                           aperture_defs,
                                           opts,
                                           iteration);
}

// ── Aperture backward pass
// ────────────────────────────────────────────────────

auto SDDPSolver::backward_pass_with_apertures(SceneIndex scene,
                                              const SolverOptions& opts,
                                              int iteration)
    -> std::expected<int, Error>
{
  const auto& simulation = planning_lp().simulation();
  const auto& all_scenarios = simulation.scenarios();
  const auto num_all_scenarios = static_cast<int>(all_scenarios.size());
  const auto& aperture_defs = simulation.apertures();

  // When an explicit aperture_array is present, use it directly.
  // Otherwise fall back to the legacy num_apertures-based behaviour
  // (first N scenarios treated as apertures with equal weight).
  if (aperture_defs.empty()) {
    // Legacy path: determine the effective aperture count
    const int n_aps = (m_options_.num_apertures < 0)
        ? num_all_scenarios
        : std::min(m_options_.num_apertures, num_all_scenarios);

    if (n_aps <= 0 || num_all_scenarios == 0) {
      return backward_pass(scene, opts, iteration);
    }

    // Build synthetic aperture definitions from the first N scenarios
    Array<Aperture> synthetic;
    synthetic.reserve(static_cast<size_t>(n_aps));
    const double prob = 1.0 / static_cast<double>(n_aps);
    for (int i = 0; i < n_aps; ++i) {
      const Uid scen_uid =
          static_cast<Uid>(all_scenarios[ScenarioIndex {i}].uid());
      synthetic.push_back(Aperture {
          .uid = scen_uid,
          .source_scenario = scen_uid,
          .probability_factor = prob,
      });
    }

    // Recurse with the synthetic aperture defs loaded into a temporary
    // — reuse the aperture-aware path below by inlining the logic.
    const auto& phases = simulation.phases();
    const auto num_phases = static_cast<Index>(phases.size());
    auto& phase_states = m_scene_phase_states_[scene];
    int total_cuts = 0;

    const auto& scene_lp = simulation.scenes()[scene];
    const auto& scene_scenarios = scene_lp.scenarios();
    if (scene_scenarios.empty()) {
      return backward_pass(scene, opts, iteration);
    }
    const auto& base_scenario = scene_scenarios.front();

    // Collect phases where all apertures were infeasible for a single summary
    std::vector<int> infeasible_phases;

    for (const auto pi : std::views::iota(1, num_phases) | std::views::reverse)
    {
      const auto phase = PhaseIndex {pi};
      const auto src_phase = PhaseIndex {pi - 1};
      auto& src_li = planning_lp().system(scene, src_phase).linear_interface();
      const auto& src_state = phase_states[src_phase];
      const auto& phase_lp = phases[phase];

      auto expected_cut = solve_apertures_for_phase(scene,
                                                    phase,
                                                    src_state,
                                                    base_scenario,
                                                    all_scenarios,
                                                    synthetic,
                                                    phase_lp.aperture_set(),
                                                    total_cuts,
                                                    opts);

      if (!expected_cut.has_value()) {
        infeasible_phases.push_back(phase_uid(phase));

        // Fallback: build a regular Benders cut from the cached
        // forward-pass reduced costs and objective (same as backward_pass).
        const auto& target_state = phase_states[phase];
        auto fallback_cut = build_benders_cut(
            src_state.alpha_col,
            src_state.outgoing_links,
            target_state.forward_col_cost,
            target_state.forward_full_obj,
            sddp_label("sddp", "fcut", scene, pi, iteration, total_cuts));

        store_cut(scene, src_phase, fallback_cut);
        src_li.add_row(fallback_cut);
        ++total_cuts;

        SPDLOG_TRACE(
            "SDDP aperture fallback: scene {} cut for phase {} rhs={:.4f}",
            scene,
            src_phase,
            fallback_cut.lowb);

        if (pi > 1) {
          auto r = resolve_via_pool(
              src_li, opts, make_backward_lp_task_req(iteration, src_phase));
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

        continue;
      }

      store_cut(scene, src_phase, *expected_cut);
      src_li.add_row(*expected_cut);
      ++total_cuts;

      SPDLOG_TRACE("SDDP aperture: scene {} cut for phase {} rhs={:.4f}",
                   scene,
                   src_phase,
                   expected_cut->lowb);

      if (pi > 1) {
        auto r = resolve_via_pool(
            src_li, opts, make_backward_lp_task_req(iteration, src_phase));
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
    }

    // Log a single summary for all phases with infeasible apertures
    if (!infeasible_phases.empty()) {
      SPDLOG_WARN(
          "SDDP aperture: scene {} — all apertures infeasible at {} phase(s) "
          "[{}], used Benders fallback cuts",
          scene_uid(scene),
          infeasible_phases.size(),
          join_ints(infeasible_phases));
    }

    return total_cuts;
  }

  // ── Aperture-array aware path ───────────────────────────────────────────
  const auto& phases = simulation.phases();
  const auto num_phases = static_cast<Index>(phases.size());
  auto& phase_states = m_scene_phase_states_[scene];
  int total_cuts = 0;

  const auto& scene_lp = simulation.scenes()[scene];
  const auto& scene_scenarios = scene_lp.scenarios();
  if (scene_scenarios.empty()) {
    return backward_pass(scene, opts, iteration);
  }
  const auto& base_scenario = scene_scenarios.front();

  // Collect phases where all apertures were infeasible for a single summary
  std::vector<int> infeasible_phases;

  // Iterate backward from last phase to phase 1 using ranges
  for (const auto pi : std::views::iota(1, num_phases) | std::views::reverse) {
    const auto phase = PhaseIndex {pi};
    const auto src_phase = PhaseIndex {pi - 1};
    auto& src_li = planning_lp().system(scene, src_phase).linear_interface();
    const auto& src_state = phase_states[src_phase];
    const auto& phase_lp = phases[phase];

    auto expected_cut = solve_apertures_for_phase(scene,
                                                  phase,
                                                  src_state,
                                                  base_scenario,
                                                  all_scenarios,
                                                  aperture_defs,
                                                  phase_lp.aperture_set(),
                                                  total_cuts,
                                                  opts);

    if (!expected_cut.has_value()) {
      infeasible_phases.push_back(phase_uid(phase));

      // Fallback: build a regular Benders cut from the cached
      // forward-pass reduced costs and objective (same as backward_pass).
      const auto& target_state = phase_states[phase];
      auto fallback_cut = build_benders_cut(
          src_state.alpha_col,
          src_state.outgoing_links,
          target_state.forward_col_cost,
          target_state.forward_full_obj,
          sddp_label("sddp", "fcut", scene, pi, iteration, total_cuts));

      store_cut(scene, src_phase, fallback_cut);
      src_li.add_row(fallback_cut);
      ++total_cuts;

      SPDLOG_TRACE(
          "SDDP aperture fallback: scene {} cut for phase {} rhs={:.4f}",
          scene,
          src_phase,
          fallback_cut.lowb);

      if (pi > 1) {
        auto r = resolve_via_pool(
            src_li, opts, make_backward_lp_task_req(iteration, src_phase));
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

      continue;
    }

    store_cut(scene, src_phase, *expected_cut);

    // Add the expected cut to the source phase LP
    src_li.add_row(*expected_cut);
    ++total_cuts;

    SPDLOG_TRACE("SDDP aperture: scene {} cut for phase {} rhs={:.4f}",
                 scene,
                 src_phase,
                 expected_cut->lowb);

    // Re-solve source phase after adding the cut (same as regular backward
    // pass) to propagate feasibility.
    if (pi > 1) {
      auto r = resolve_via_pool(
          src_li, opts, make_backward_lp_task_req(iteration, src_phase));
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
  }

  // Log a single summary for all phases with infeasible apertures
  if (!infeasible_phases.empty()) {
    SPDLOG_WARN(
        "SDDP aperture: scene {} — all apertures infeasible at {} phase(s) "
        "[{}], used Benders fallback cuts",
        scene_uid(scene),
        infeasible_phases.size(),
        join_ints(infeasible_phases));
  }

  return total_cuts;
}

// ── Helper: solve all apertures for a single phase ──────────────────────────

auto SDDPSolver::solve_apertures_for_phase(
    SceneIndex scene,
    PhaseIndex phase,
    const PhaseStateInfo& src_state,
    const ScenarioLP& base_scenario,
    std::span<const ScenarioLP> all_scenarios,
    std::span<const Aperture> aperture_defs,
    std::span<const Uid> phase_apertures,
    int total_cuts,
    const SolverOptions& opts) -> std::optional<SparseRow>
{
  const auto pi = static_cast<Index>(phase);
  auto& sys = planning_lp().system(scene, phase);
  const auto& phase_li = sys.linear_interface();
  const auto& phase_lp = planning_lp().simulation().phases()[phase];

  std::vector<SparseRow> aperture_cuts;
  std::vector<double> aperture_weights;
  double total_weight = 0.0;

  // Build the effective aperture list for this phase.
  //
  // Each entry is a pair: (aperture reference, repetition count N).
  // When phase_apertures contains duplicates (e.g. [1,2,3,3,3]),
  // each unique aperture is solved only once but its weight is
  // scaled by N (the number of occurrences).
  struct ApertureEntry
  {
    std::reference_wrapper<const Aperture> aperture;
    int count {};
  };

  std::vector<ApertureEntry> effective_apertures;

  if (phase_apertures.empty()) {
    // Use all aperture definitions (each with count = 1)
    for (const auto& ap : aperture_defs) {
      if (ap.is_active()) {
        effective_apertures.push_back({.aperture = std::cref(ap), .count = 1});
      }
    }
  } else {
    // Count occurrences of each UID in the phase_apertures list.
    // Preserving order of first appearance keeps results deterministic.
    std::vector<std::pair<Uid, int>> uid_counts;
    for (const auto ap_uid : phase_apertures) {
      bool found = false;
      for (auto& [uid, cnt] : uid_counts) {
        if (uid == ap_uid) {
          ++cnt;
          found = true;
          break;
        }
      }
      if (!found) {
        uid_counts.emplace_back(ap_uid, 1);
      }
    }

    // Map each unique UID to its aperture definition
    for (const auto& [uid, cnt] : uid_counts) {
      for (const auto& ap : aperture_defs) {
        if (ap.uid == uid && ap.is_active()) {
          effective_apertures.push_back(
              {.aperture = std::cref(ap), .count = cnt});
          break;
        }
      }
    }
  }

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

    // Find the scenario corresponding to this aperture's source_scenario UID
    const ScenarioLP* aperture_scenario_ptr = nullptr;
    for (const auto& scen : all_scenarios) {
      if (static_cast<Uid>(scen.uid()) == aperture.source_scenario) {
        aperture_scenario_ptr = &scen;
        break;
      }
    }

    if (aperture_scenario_ptr == nullptr) {
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

    const auto& aperture_scenario = *aperture_scenario_ptr;

    // Clone the phase LP (state variables already fixed from forward pass)
    auto clone = phase_li.clone();

    // Update flow column bounds for this aperture's scenario
    auto& flow_collection = std::get<Collection<FlowLP>>(sys.collections());
    for (auto& flow_lp : flow_collection.elements()) {
      for (const auto& stage : phase_lp.stages()) {
        [[maybe_unused]] const auto ok = flow_lp.update_aperture_bounds(
            clone, base_scenario, aperture_scenario, stage);
      }
    }

    // Solve the clone via the work pool with backward priority
    auto result = resolve_clone_via_pool(
        clone, opts, make_backward_lp_task_req(0, phase));
    if (!result.has_value() || !clone.is_optimal()) {
      ++n_infeasible;
      SPDLOG_DEBUG(
          "SDDP aperture: scene {} phase {} aperture uid {} infeasible "
          "(status {}), skipping",
          scene,
          phase,
          ap_uid,
          clone.get_status());

      // Save the infeasible aperture LP for later inspection only in
      // trace/debug mode — aperture infeasibility is expected in some
      // scenarios and writing LPs during normal SDDP iteration is expensive.
      if (!m_options_.log_directory.empty()
          && spdlog::get_level() <= spdlog::level::debug)
      {
        std::filesystem::create_directories(m_options_.log_directory);
        const auto err_stem = (std::filesystem::path(m_options_.log_directory)
                               / std::format("error_aperture_sc_{}_ph_{}_ap_{}",
                                             scene_uid(scene),
                                             phase_uid(phase),
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
        sddp_label("sddp", "aper_cut", scene, pi, ap_uid, total_cuts);
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
        "SDDP aperture: scene {} phase {} — {}/{} feasible, {} infeasible, "
        "{} skipped",
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
  const auto expected_name = sddp_label("sddp", "ecut", scene, pi, total_cuts);
  return weighted_average_benders_cut(
      aperture_cuts, aperture_weights, expected_name);
}

}  // namespace gtopt
