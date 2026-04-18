/**
 * @file      sddp_forward_pass.cpp
 * @brief     SDDP forward pass implementation
 * @date      2026-03-22
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Extracted from sddp_solver.cpp — implements the forward pass that
 * solves each phase LP in sequence, propagating state variables and
 * handling elastic fallback for infeasible phases.
 */

#include <cmath>
#include <filesystem>
#include <format>
#include <span>
#include <thread>

#include <gtopt/as_label.hpp>
#include <gtopt/benders_cut.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/system_lp.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{
}  // namespace

auto SDDPMethod::forward_pass(SceneIndex scene_index,
                              const SolverOptions& opts,
                              IterationIndex iteration_index)
    -> std::expected<double, Error>
{
  const auto& phases = planning_lp().simulation().phases();
  auto& phase_states = m_scene_phase_states_[scene_index];
  double total_opex = 0.0;

  // Apply solve_timeout to the solver options if configured
  auto effective_opts = opts;
  if (m_options_.solve_timeout > 0.0) {
    effective_opts.time_limit = m_options_.solve_timeout;
  }

  [[maybe_unused]] const auto fwd_tid = std::this_thread::get_id();
  SPDLOG_INFO("SDDP Forward [i{} s{}]: starting ({} phases) [thread {}]",
              iteration_index,
              scene_uid(scene_index),
              phases.size(),
              std::hash<std::thread::id> {}(fwd_tid) % 10000);

  // Check once whether update_lp should run for this iteration
  // (respects explicit skip/force flags and global skip count).
  const bool do_update = should_dispatch_update_lp(iteration_index);

  for (auto&& [phase_index, _ph] : enumerate<PhaseIndex>(phases)) {
    if (should_stop()) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format("{}: cancelled",
                                 sddp_log("Forward",
                                          iteration_index,
                                          scene_uid(scene_index),
                                          phase_uid(phase_index))),
      });
    }

    auto& system = planning_lp().system(scene_index, phase_index);
    auto& state = phase_states[phase_index];

    // Ensure the LP backend is live.  No-op when mode=off or when a
    // prior task already reloaded this cell; otherwise reconstructs
    // from snapshot (snapshot/compress) or re-flattens from collections
    // (rebuild).  dispatch_update_lp() runs after this point, so
    // coefficient updates are replayed naturally.
    system.ensure_lp_built();

    auto& li = system.linear_interface();

    // Propagate state variables from previous phase.
    // Trial values are read from each link's source StateVariable
    // (col_sol() mirror, populated by the previous phase's solve via
    // `capture_state_variable_values`).  Mode-agnostic: works identically
    // across LowMemoryMode off / snapshot / compress / rebuild.
    if (phase_index) {
      const auto prev_phase_index = previous(phase_index);
      auto& prev_st = phase_states[prev_phase_index];

      propagate_trial_values(prev_st.outgoing_links, li);

      SPDLOG_TRACE(
          "SDDP Forward [i{} s{} p{}]: propagated {} state vars from phase {}",
          iteration_index,
          scene_uid(scene_index),
          phase_uid(phase_index),
          prev_st.outgoing_links.size(),
          phase_uid(prev_phase_index));
    }

    // Update volume-dependent LP coefficients (discharge limits, turbine
    // efficiency, seepage, production factor) AFTER state propagation so
    // that physical_eini reflects the actual forward-pass reservoir volume
    // rather than default_volume.
    if (do_update) {
      const auto updated = update_lp_for_phase(scene_index, phase_index);
      if (updated > 0) {
        SPDLOG_TRACE("SDDP Forward [i{} s{} p{}]: updated {} LP elements",
                     iteration_index,
                     scene_uid(scene_index),
                     phase_uid(phase_index),
                     updated);
      }
    }

    // If lp_debug is enabled, write LP file (pre-solve state) then optionally
    // submit gzip compression as a fire-and-forget async task.
    // Selective filters (scene/phase range) skip non-matching LPs.
    if (m_options_.lp_debug) {
      const bool in_range = in_lp_debug_range(scene_uid(scene_index),
                                              phase_uid(phase_index),
                                              m_options_.lp_debug_scene_min,
                                              m_options_.lp_debug_scene_max,
                                              m_options_.lp_debug_phase_min,
                                              m_options_.lp_debug_phase_max);
      if (in_range) {
        const auto dbg_stem = (std::filesystem::path(m_options_.log_directory)
                               / std::format(sddp_file::debug_lp_fmt,
                                             iteration_index,
                                             scene_uid(scene_index),
                                             phase_uid(phase_index)))
                                  .string();
        m_lp_debug_writer_.write(li, dbg_stem);
      }
    }

    // Configure solver log file based on log_mode.
    // Configure per-solve log file when log_mode is detailed.
    if (effective_opts.log_mode.value_or(SolverLogMode::nolog)
            == SolverLogMode::detailed
        && !m_options_.log_directory.empty())
    {
      std::filesystem::create_directories(m_options_.log_directory);
      li.set_log_file((std::filesystem::path(m_options_.log_directory)
                       / as_label(li.solver_name(),
                                  scene_uid(scene_index),
                                  phase_uid(phase_index)))
                          .string());
    }

    // Tag the LP with the SDDP Forward key so fallback warnings emitted
    // by LinearInterface::resolve() carry the same context as the
    // surrounding SDDP info logs.
    li.set_log_tag(sddp_log("Forward",
                            iteration_index,
                            scene_uid(scene_index),
                            phase_uid(phase_index)));

    // Solve directly — already running in a pool thread.
    auto result = li.resolve(effective_opts);

    // Capture status before any release — release loses specific codes.
    const auto solve_status = li.get_status();

    if (!result.has_value() || solve_status != 0) {
      if (m_options_.solve_timeout > 0.0
          && (solve_status == 1 || solve_status == 3))
      {
        spdlog::critical(
            "SDDP Forward [i{} s{} p{}]: solve timeout ({:.1f}s) (status {})",
            iteration_index,
            scene_uid(scene_index),
            phase_uid(phase_index),
            m_options_.solve_timeout,
            solve_status);
        return std::unexpected(Error {
            .code = ErrorCode::SolverError,
            .message = std::format("{}: solve timeout ({:.1f}s) (status {})",
                                   sddp_log("Forward",
                                            iteration_index,
                                            scene_uid(scene_index),
                                            phase_uid(phase_index)),
                                   m_options_.solve_timeout,
                                   solve_status),
            .status = solve_status,
        });
      }

      SPDLOG_WARN(
          "SDDP Forward [i{} s{} p{}]: non-optimal (status {}), trying elastic"
          " solve",
          iteration_index,
          scene_uid(scene_index),
          phase_uid(phase_index),
          solve_status);
      // Clone the LP, apply elastic filter, and solve the clone.
      // The original LP remains unmodified (PLP clone pattern).
      auto elastic_result = elastic_solve(scene_index, phase_index, opts);

      // Release solver backend — no-op when low_memory is off.
      system.release_backend();

      if (elastic_result.has_value()) {
        auto& solved_li = elastic_result->clone;
        m_phase_grid_.record(iteration_index,
                             scene_uid(scene_index),
                             phase_index,
                             GridCell::Elastic);
        // Track max kappa from elastic solve
        update_max_kappa(scene_index, phase_index, solved_li, iteration_index);
        // Increment infeasibility counter for this (scene, phase)
        ++m_infeasibility_counter_[scene_index][phase_index];

        // Cache solution data for the backward pass
        const auto obj = solved_li.get_obj_value();
        state.forward_full_obj = obj;

        const auto sol = solved_li.get_col_sol_raw();
        const auto rc = solved_li.get_col_cost_raw();

        // Mirror per-state-variable values needed by cut construction
        // and next-phase propagation onto the StateVariable objects.
        //
        // Duals of the elastic clone are NOT written — the clone's LP is
        // modified (relaxed bounds + slack variables) so its duals don't
        // represent the original problem's shadow prices.  Downstream
        // code falls back to reduced-cost cuts (see backward_pass_*).
        capture_state_variable_values(scene_index, phase_index, sol, rc);

        const auto sa = m_options_.scale_alpha;
        const auto alpha_val = safe_alpha_value(state.alpha_col, sol, sa);
        state.forward_objective = obj - alpha_val;
        total_opex += state.forward_objective;

        SPDLOG_INFO(
            "SDDP Forward [i{} s{} p{}]: elastic ok, obj={:.4f} alpha={:.4f}"
            " opex={:.4f} [infeas_count={}]",
            iteration_index,
            scene_uid(scene_index),
            phase_uid(phase_index),
            obj,
            alpha_val,
            state.forward_objective,
            m_infeasibility_counter_[scene_index][phase_index]);
      } else {
        // Save the infeasible LP and run diagnostics only when:
        //  - it's the first phase (the scene will be declared infeasible), or
        //  - trace/debug logging is enabled (developer debugging).
        // During normal SDDP iteration, skip writing/diagnosing error LPs
        // to avoid I/O overhead.
        const bool is_first_phase = !phase_index;
        const bool is_trace_debug =
            (spdlog::get_level() <= spdlog::level::debug);
        if (!m_options_.log_directory.empty()
            && (is_first_phase || is_trace_debug))
        {
          spdlog::warn("SDDP Forward [i{} s{} p{}]: infeasible LP (status {})",
                       iteration_index,
                       scene_uid(scene_index),
                       phase_uid(phase_index),
                       solve_status);
        }
        return std::unexpected(Error {
            .code = ErrorCode::SolverError,
            .message = std::format("{}: failed (status {})",
                                   sddp_log("Forward",
                                            iteration_index,
                                            scene_uid(scene_index),
                                            phase_uid(phase_index)),
                                   solve_status),
        });
      }
    } else {
      // Phase solved normally – reset infeasibility counter
      m_infeasibility_counter_[scene_index][phase_index] = 0;
      m_phase_grid_.record(iteration_index,
                           scene_uid(scene_index),
                           phase_index,
                           GridCell::Forward);
      // Track max kappa from forward solve
      update_max_kappa(scene_index, phase_index, li, iteration_index);

      // Cache solution data for the backward pass — must happen before
      // release_backend() which may discard cached solution vectors.
      const auto obj = li.get_obj_value();
      state.forward_full_obj = obj;

      const auto sol = li.get_col_sol_raw();
      const auto rc = li.get_col_cost_raw();

      // Mirror per-state-variable runtime values onto the persistent
      // StateVariable objects.  These replace the former per-PhaseState
      // full-vector caches for both next-phase trial-value propagation
      // and backward-pass cut construction.  Cuts always use reduced
      // costs, so row duals are never needed here.
      capture_state_variable_values(scene_index, phase_index, sol, rc);

      const auto sa = m_options_.scale_alpha;
      const auto alpha_val = safe_alpha_value(state.alpha_col, sol, sa);
      state.forward_objective = obj - alpha_val;

      // Release solver backend — no-op when low_memory is off.
      system.release_backend();

      // Guard against solver returning "optimal" with NaN values
      // (can happen when inherited cuts cause ill-conditioning).
      if (std::isnan(state.forward_objective)) {
        SPDLOG_WARN(
            "SDDP Forward [i{} s{} p{}]: solve returned optimal but"
            " forward_objective is NaN (obj={}, alpha={})",
            iteration_index,
            scene_uid(scene_index),
            phase_uid(phase_index),
            obj,
            alpha_val);
        return std::unexpected(Error {
            .code = ErrorCode::SolverError,
            .message = std::format("{}: optimal status but NaN in solution",
                                   sddp_log("Forward",
                                            iteration_index,
                                            scene_uid(scene_index),
                                            phase_uid(phase_index))),
        });
      }

      total_opex += state.forward_objective;

      SPDLOG_TRACE(
          "SDDP Forward [i{} s{} p{}]: obj={:.4f} alpha={:.4f}"
          " opex={:.4f}",
          iteration_index,
          scene_uid(scene_index),
          phase_uid(phase_index),
          obj,
          alpha_val,
          state.forward_objective);
    }
  }

  // Guard against NaN in total forward-pass cost.  Can happen when
  // inherited cuts cause solver ill-conditioning (barrier failure with
  // subsequent dual fallback producing NaN values).
  if (std::isnan(total_opex)) {
    SPDLOG_WARN(
        "SDDP Forward [i{} s{}]: total_opex is NaN —"
        " solver ill-conditioning",
        iteration_index,
        scene_uid(scene_index));
    return std::unexpected(Error {
        .code = ErrorCode::SolverError,
        .message = std::format(
            "{}: forward pass total cost is NaN",
            sddp_log("Forward", iteration_index, scene_uid(scene_index))),
    });
  }

  SPDLOG_INFO("SDDP Forward [i{} s{}]: done, total_opex={:.4f} [thread {}]",
              iteration_index,
              scene_uid(scene_index),
              total_opex,
              std::hash<std::thread::id> {}(fwd_tid) % 10000);
  return total_opex;
}

}  // namespace gtopt
