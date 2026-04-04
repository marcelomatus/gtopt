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

#include <chrono>
#include <filesystem>
#include <format>
#include <span>
#include <thread>

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
/// Round *n* up to the next multiple of *alignment*.
constexpr std::size_t align_up(std::size_t n, std::size_t alignment) noexcept
{
  return (n + alignment - 1) / alignment * alignment;
}

/// Assign *src* into *dst* and pad with zeros to at least *src.size() + extra*,
/// rounded up to a 16-element boundary.
void assign_padded(std::vector<double>& dst,
                   std::span<const double> src,
                   std::size_t extra)
{
  dst.assign(src.begin(), src.end());
  dst.resize(align_up(src.size() + extra, 16), 0.0);
}
}  // namespace

auto SDDPMethod::forward_pass(SceneIndex scene,
                              const SolverOptions& opts,
                              IterationIndex iteration)
    -> std::expected<double, Error>
{
  const auto& phases = planning_lp().simulation().phases();
  auto& phase_states = m_scene_phase_states_[scene];
  double total_opex = 0.0;

  // Apply solve_timeout to the solver options if configured
  auto effective_opts = opts;
  if (m_options_.solve_timeout > 0.0) {
    effective_opts.time_limit = m_options_.solve_timeout;
  }
  // After the first iteration the LP already carries a valid basis from the
  // previous solve.  Enable warm-start (dual simplex + no presolve) so the
  // solver pivots from that basis instead of re-solving from scratch with
  // barrier.  With hot start (offset > 0), reuse_basis is enabled from the
  // first iteration since the LP already has a basis.
  if (iteration > m_iteration_offset_ && m_options_.warm_start) {
    effective_opts.reuse_basis = true;
  }

  [[maybe_unused]] const auto fwd_tid = std::this_thread::get_id();
  SPDLOG_INFO("SDDP forward: scene {} iter {} starting ({} phases) [thread {}]",
              scene_uid(scene),
              iteration,
              phases.size(),
              std::hash<std::thread::id> {}(fwd_tid) % 10000);

  // Check once whether update_lp should run for this iteration
  // (respects explicit skip/force flags and global skip count).
  const bool do_update = should_dispatch_update_lp(iteration);

  for (auto&& [phase, _ph] : enumerate<PhaseIndex>(phases)) {
    if (should_stop()) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format("SDDP forward: cancelled at scene {} phase {}",
                                 scene_uid(scene),
                                 phase_uid(phase)),
      });
    }

    auto& li = planning_lp().system(scene, phase).linear_interface();
    auto& state = phase_states[phase];

    // Propagate state variables from previous phase
    if (phase != PhaseIndex {0}) {
      const auto prev = phase - PhaseIndex {1};
      auto& prev_st = phase_states[prev];
      const auto& prev_sol = planning_lp()
                                 .system(scene, prev)
                                 .linear_interface()
                                 .get_col_sol_raw();

      const auto coeff_mode = m_options_.cut_coeff_mode;
      if (coeff_mode == CutCoeffMode::row_dual) {
        propagate_trial_values_row_dual(prev_st.outgoing_links, prev_sol, li);
      } else {
        propagate_trial_values(prev_st.outgoing_links, prev_sol, li);
      }

      SPDLOG_TRACE(
          "SDDP forward: scene {} phase {} propagated {} state vars from "
          "phase {} ({})",
          scene_uid(scene),
          phase_uid(phase),
          prev_st.outgoing_links.size(),
          phase_uid(prev),
          enum_name(coeff_mode));
    }

    // Update volume-dependent LP coefficients (discharge limits, turbine
    // efficiency, seepage, production factor) AFTER state propagation so
    // that physical_eini reflects the actual forward-pass reservoir volume
    // rather than default_volume.
    if (do_update) {
      const auto updated = update_lp_for_phase(scene, phase);
      if (updated > 0) {
        SPDLOG_TRACE(
            "SDDP forward: updated {} LP elements for scene {} phase {} "
            "(iter {})",
            updated,
            scene_uid(scene),
            phase_uid(phase),
            iteration);
      }
    }

    // If lp_debug is enabled, write LP file (pre-solve state) then optionally
    // submit gzip compression as a fire-and-forget async task.
    // Selective filters (scene/phase range) skip non-matching LPs.
    if (m_options_.lp_debug) {
      const auto su = static_cast<int>(scene_uid(scene));
      const auto pu = static_cast<int>(phase_uid(phase));
      const bool in_range = in_lp_debug_range(su,
                                              pu,
                                              m_options_.lp_debug_scene_min,
                                              m_options_.lp_debug_scene_max,
                                              m_options_.lp_debug_phase_min,
                                              m_options_.lp_debug_phase_max);
      if (in_range) {
        const auto dbg_stem = (std::filesystem::path(m_options_.log_directory)
                               / std::format(sddp_file::debug_lp_fmt,
                                             iteration,
                                             scene_uid(scene),
                                             phase_uid(phase)))
                                  .string();
        m_lp_debug_writer_.write(li, dbg_stem);
      }
    }

    // Apply saved forward-pass solution from the previous iteration as
    // warm-start hint.  Dimension mismatches (new cut rows) are handled
    // by set_warm_start_solution() via zero-padding.
    if (effective_opts.reuse_basis) {
      li.set_warm_start_solution(state.forward_col_sol, state.forward_row_dual);
    }

    // Configure solver log file based on log_mode.
    // Configure per-solve log file when log_mode is detailed.
    if (effective_opts.log_mode.value_or(SolverLogMode::nolog)
            == SolverLogMode::detailed
        && !m_options_.log_directory.empty())
    {
      std::filesystem::create_directories(m_options_.log_directory);
      li.set_log_file((std::filesystem::path(m_options_.log_directory)
                       / std::format("{}_sc{}_ph{}",
                                     li.solver_name(),
                                     scene_uid(scene),
                                     phase_uid(phase)))
                          .string());
    }

    // Solve directly — already running in a pool thread.
    auto result = li.resolve(effective_opts);

    if (!result.has_value() || !li.is_optimal()) {
      // Check for solve timeout: status 1 (abandoned) or 3 (other)
      // when a time limit was set indicates a timeout
      const auto status = li.get_status();
      if (m_options_.solve_timeout > 0.0 && (status == 1 || status == 3)) {
        // Write the timed-out LP for debugging
        if (!m_options_.log_directory.empty()) {
          std::filesystem::create_directories(m_options_.log_directory);
          const auto timeout_stem =
              (std::filesystem::path(m_options_.log_directory)
               / std::format("timeout_scene_{}_phase_{}_iter_{}",
                             scene_uid(scene),
                             phase_uid(phase),
                             iteration))
                  .string();
          if (auto lp_result = li.write_lp(timeout_stem)) {
            spdlog::critical(
                "SDDP forward: solve timeout ({:.1f}s) at iter {} scene {} "
                "phase {} (status {}), LP saved to {}.lp",
                m_options_.solve_timeout,
                iteration,
                scene_uid(scene),
                phase_uid(phase),
                status,
                timeout_stem);
          } else {
            spdlog::critical(
                "SDDP forward: solve timeout ({:.1f}s) at iter {} scene {} "
                "phase {} (status {}). {}",
                m_options_.solve_timeout,
                iteration,
                scene_uid(scene),
                phase_uid(phase),
                status,
                lp_result.error().message);
          }
        } else {
          spdlog::critical(
              "SDDP forward: solve timeout ({:.1f}s) at iter {} scene {} "
              "phase {} (status {})",
              m_options_.solve_timeout,
              iteration,
              scene_uid(scene),
              phase_uid(phase),
              status);
        }
        return std::unexpected(Error {
            .code = ErrorCode::SolverError,
            .message = std::format(
                "SDDP forward: solve timeout ({:.1f}s) at iter {} scene {} "
                "phase {} (status {})",
                m_options_.solve_timeout,
                iteration,
                scene,
                phase,
                status),
            .status = status,
        });
      }

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
        // Track max kappa from elastic solve
        update_max_kappa(scene, phase, solved_li, iteration);
        // Increment infeasibility counter for this (scene, phase)
        ++m_infeasibility_counter_[scene][phase];

        // Cache solution data for the backward pass
        const auto obj = solved_li.get_obj_value();
        state.forward_full_obj = obj;

        const auto rc = solved_li.get_col_cost_raw();
        state.forward_col_cost.assign(rc.begin(), rc.end());

        // Save primal/dual solution for aperture warm-start (pre-padded
        // so set_warm_start_solution can use a subspan without allocation).
        const auto n_links = state.outgoing_links.size();
        assign_padded(
            state.forward_col_sol, solved_li.get_col_sol_raw(), n_links);
        assign_padded(
            state.forward_row_dual, solved_li.get_row_dual_raw(), 2 * n_links);

        const auto sa = m_options_.scale_alpha;
        const auto alpha_val = (state.alpha_col != ColIndex {unknown_index})
            ? solved_li.get_col_sol_raw()[state.alpha_col] * sa
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
          if (auto lp_result = li.write_lp(err_file)) {
            spdlog::warn("SDDP: saved infeasible LP to {}.lp", err_file);
          } else {
            spdlog::warn("{}", lp_result.error().message);
          }
          // LP diagnostic analysis is performed by run_gtopt after exit.
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
      // Track max kappa from forward solve
      update_max_kappa(scene, phase, li, iteration);

      // Cache solution data for the backward pass
      const auto obj = li.get_obj_value();
      state.forward_full_obj = obj;

      const auto rc = li.get_col_cost_raw();
      state.forward_col_cost.assign(rc.begin(), rc.end());

      // Save primal/dual solution for aperture warm-start (pre-padded
      // so set_warm_start_solution can use a subspan without allocation).
      const auto n_links = state.outgoing_links.size();
      assign_padded(state.forward_col_sol, li.get_col_sol_raw(), n_links);
      assign_padded(state.forward_row_dual, li.get_row_dual_raw(), 2 * n_links);

      const auto sa = m_options_.scale_alpha;
      const auto alpha_val = (state.alpha_col != ColIndex {unknown_index})
          ? li.get_col_sol_raw()[state.alpha_col] * sa
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

  SPDLOG_INFO(
      "SDDP forward: scene {} iter {} done, total_opex={:.4f} "
      "[thread {}]",
      scene_uid(scene),
      iteration,
      total_opex,
      std::hash<std::thread::id> {}(fwd_tid) % 10000);
  return total_opex;
}

}  // namespace gtopt
