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
#include <ranges>
#include <span>
#include <utility>

#include <gtopt/check_lp.hpp>
#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_aperture.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_monitor.hpp>
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

// ── Cut persistence (delegated to sddp_cut_io.hpp free functions) ───────────

auto SDDPSolver::save_cuts(const std::string& filepath) const
    -> std::expected<void, Error>
{
  return save_cuts_csv(m_stored_cuts_, planning_lp(), filepath);
}

auto SDDPSolver::save_scene_cuts(SceneIndex scene,
                                 const std::string& directory) const
    -> std::expected<void, Error>
{
  return save_scene_cuts_csv(
      m_scene_cuts_[scene], scene, scene_uid(scene), planning_lp(), directory);
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
    -> std::expected<CutLoadResult, Error>
{
  return load_cuts_csv(planning_lp(), filepath, m_label_maker_);
}

auto SDDPSolver::load_scene_cuts_from_directory(const std::string& directory)
    -> std::expected<CutLoadResult, Error>
{
  return gtopt::load_scene_cuts_from_directory(
      planning_lp(), directory, m_label_maker_);
}

auto SDDPSolver::load_boundary_cuts(const std::string& filepath)
    -> std::expected<CutLoadResult, Error>
{
  return load_boundary_cuts_csv(planning_lp(),
                                filepath,
                                m_options_,
                                m_label_maker_,
                                m_scene_phase_states_);
}

auto SDDPSolver::load_named_cuts(const std::string& filepath)
    -> std::expected<CutLoadResult, Error>
{
  return load_named_cuts_csv(planning_lp(),
                             filepath,
                             m_options_,
                             m_label_maker_,
                             m_scene_phase_states_);
}

// ── Monitoring API ───────────────────────────────────────────────────────────
// Implementation moved to sddp_monitor.cpp (write_sddp_api_status free fn).
// maybe_write_api_status below builds the snapshot and delegates.

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

  // ── Load hot-start cuts and track max iteration for offset ────────────────
  m_iteration_offset_ = 0;

  if (!m_options_.cuts_input_file.empty()) {
    auto result = load_cuts(m_options_.cuts_input_file);
    if (result.has_value()) {
      m_iteration_offset_ =
          std::max(m_iteration_offset_, result->max_iteration);
      SPDLOG_INFO("SDDP hot-start: loaded {} cuts (max_iter={})",
                  result->count,
                  result->max_iteration);
    } else {
      SPDLOG_WARN("SDDP hot-start: could not load cuts: {}",
                  result.error().message);
    }
  } else if (!m_options_.cuts_output_file.empty()) {
    const auto cut_dir =
        std::filesystem::path(m_options_.cuts_output_file).parent_path();
    if (!cut_dir.empty() && std::filesystem::exists(cut_dir)) {
      auto result = load_scene_cuts_from_directory(cut_dir.string());
      if (result.has_value() && result->count > 0) {
        m_iteration_offset_ =
            std::max(m_iteration_offset_, result->max_iteration);
        SPDLOG_INFO("SDDP hot-start: loaded {} cuts from {} (max_iter={})",
                    result->count,
                    cut_dir.string(),
                    result->max_iteration);
      }
    }
  }

  // ── Load boundary cuts (future-cost function for last phase) ──────────────
  if (!m_options_.boundary_cuts_file.empty()) {
    auto result = load_boundary_cuts(m_options_.boundary_cuts_file);
    if (result.has_value()) {
      m_iteration_offset_ =
          std::max(m_iteration_offset_, result->max_iteration);
      SPDLOG_INFO("SDDP: loaded {} boundary cuts from {} (max_iter={})",
                  result->count,
                  m_options_.boundary_cuts_file,
                  result->max_iteration);
    } else {
      SPDLOG_WARN("SDDP: could not load boundary cuts: {}",
                  result.error().message);
    }
  }

  // ── Load named hot-start cuts (all phases, named state variables) ─────────
  if (!m_options_.named_cuts_file.empty()) {
    auto result = load_named_cuts(m_options_.named_cuts_file);
    if (result.has_value()) {
      m_iteration_offset_ =
          std::max(m_iteration_offset_, result->max_iteration);
      SPDLOG_INFO("SDDP: loaded {} named hot-start cuts from {} (max_iter={})",
                  result->count,
                  m_options_.named_cuts_file,
                  result->max_iteration);
    } else {
      SPDLOG_WARN("SDDP: could not load named hot-start cuts: {}",
                  result.error().message);
    }
  }

  if (m_iteration_offset_ > 0) {
    SPDLOG_INFO("SDDP: iteration offset set to {} from hot-start cuts",
                m_iteration_offset_);
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
      && (iter >= m_iteration_offset_ + m_options_.min_iterations);

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
  const SDDPStatusSnapshot snapshot {
      .iteration = m_current_iteration_.load(),
      .gap = m_current_gap_.load(),
      .lower_bound = m_current_lb_.load(),
      .upper_bound = m_current_ub_.load(),
      .converged = m_converged_.load(),
      .max_iterations = m_options_.max_iterations,
      .min_iterations = m_options_.min_iterations,
  };
  write_sddp_api_status(status_file, results, elapsed, snapshot, monitor);
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

  const int iter_start_val = m_iteration_offset_ + 1;
  const int iter_end_val = m_iteration_offset_ + m_options_.max_iterations;
  for (int iter = iter_start_val; iter <= iter_end_val; ++iter) {
    const auto iter_start = std::chrono::steady_clock::now();

    if (should_stop()) {
      SPDLOG_INFO("SDDP: stop requested, halting after {} iterations",
                  iter - 1);
      break;
    }

    SPDLOG_INFO("SDDP: === iteration {} / {} ===", iter, iter_end_val);

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

// ── Helper: build the ApertureResolveFunc callback ──────────────────────────

auto SDDPSolver::make_aperture_resolve_fn() -> ApertureResolveFunc
{
  return [this](LinearInterface& clone,
                const SolverOptions& opts,
                PhaseIndex phase) -> std::expected<int, Error>
  {
    return resolve_clone_via_pool(
        clone, opts, make_backward_lp_task_req(0, phase));
  };
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
  const auto& plp = planning_lp().simulation().phases()[phase];

  auto expected_cut =
      solve_apertures_for_phase(scene,
                                phase,
                                src_state,
                                base_scenario,
                                all_scenarios,
                                aperture_defs,
                                plp.aperture_set(),
                                cut_offset,
                                planning_lp().system(scene, phase),
                                plp,
                                opts,
                                m_label_maker_,
                                m_options_.log_directory,
                                scene_uid(scene),
                                phase_uid(phase),
                                make_aperture_resolve_fn());

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

  // No explicit apertures → build synthetic from first N scenarios
  if (aperture_defs.empty()) {
    const int n_aps = (m_options_.num_apertures < 0)
        ? num_all_scenarios
        : std::min(m_options_.num_apertures, num_all_scenarios);

    if (n_aps <= 0 || num_all_scenarios == 0) {
      return backward_pass_single_phase(
          scene, phase, cut_offset, opts, iteration);
    }

    auto synthetic = build_synthetic_apertures(all_scenarios, n_aps);

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

// ── Aperture backward pass ──────────────────────────────────────────────────

auto SDDPSolver::backward_pass_with_apertures(SceneIndex scene,
                                              const SolverOptions& opts,
                                              int iteration)
    -> std::expected<int, Error>
{
  const auto& simulation = planning_lp().simulation();
  const auto& all_scenarios = simulation.scenarios();
  const auto num_all_scenarios = static_cast<int>(all_scenarios.size());
  const auto& aperture_defs = simulation.apertures();

  // Determine the effective aperture definitions to use:
  // explicit aperture_array if present, otherwise synthetic from first N
  // scenarios.
  Array<Aperture> synthetic;
  std::span<const Aperture> effective_defs;

  if (!aperture_defs.empty()) {
    effective_defs = aperture_defs;
  } else {
    // Legacy path: determine the effective aperture count
    const int n_aps = (m_options_.num_apertures < 0)
        ? num_all_scenarios
        : std::min(m_options_.num_apertures, num_all_scenarios);

    if (n_aps <= 0 || num_all_scenarios == 0) {
      return backward_pass(scene, opts, iteration);
    }

    synthetic = build_synthetic_apertures(all_scenarios, n_aps);
    effective_defs = synthetic;
  }

  // ── Common aperture backward loop ─────────────────────────────────────
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
  auto resolve_fn = make_aperture_resolve_fn();

  // Collect phases where all apertures were infeasible for a summary
  std::vector<int> infeasible_phases;

  for (const auto pi : std::views::iota(1, num_phases) | std::views::reverse) {
    const auto phase = PhaseIndex {pi};
    const auto src_phase = PhaseIndex {pi - 1};
    auto& src_li = planning_lp().system(scene, src_phase).linear_interface();
    const auto& src_state = phase_states[src_phase];
    const auto& plp = phases[phase];

    auto expected_cut =
        solve_apertures_for_phase(scene,
                                  phase,
                                  src_state,
                                  base_scenario,
                                  all_scenarios,
                                  effective_defs,
                                  plp.aperture_set(),
                                  total_cuts,
                                  planning_lp().system(scene, phase),
                                  plp,
                                  opts,
                                  m_label_maker_,
                                  m_options_.log_directory,
                                  scene_uid(scene),
                                  phase_uid(phase),
                                  resolve_fn);

    if (!expected_cut.has_value()) {
      infeasible_phases.push_back(phase_uid(phase));

      // Fallback: build a regular Benders cut from the cached
      // forward-pass reduced costs and objective.
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
          "SDDP aperture fallback: scene {} cut for phase {} "
          "rhs={:.4f}",
          scene,
          src_phase,
          fallback_cut.lowb);

      if (pi > 1) {
        auto r = resolve_via_pool(
            src_li, opts, make_backward_lp_task_req(iteration, src_phase));
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

    store_cut(scene, src_phase, *expected_cut);
    src_li.add_row(*expected_cut);
    ++total_cuts;

    SPDLOG_TRACE("SDDP aperture: scene {} cut for phase {} rhs={:.4f}",
                 scene,
                 src_phase,
                 expected_cut->lowb);

    // Re-solve source phase after adding the cut to propagate
    // feasibility.
    if (pi > 1) {
      auto r = resolve_via_pool(
          src_li, opts, make_backward_lp_task_req(iteration, src_phase));
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
        join_ints(infeasible_phases));
  }

  return total_cuts;
}

}  // namespace gtopt
