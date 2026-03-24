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

#include <filesystem>
#include <format>
#include <span>

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
  // barrier.  This is especially important when barrier is the default algo:
  // barrier ignores the existing basis entirely.
  if (iteration > IterationIndex {0} && m_options_.warm_start) {
    effective_opts.warm_start = true;
  }

  SPDLOG_DEBUG("SDDP forward: scene {} iter {} starting ({} phases)",
               scene_uid(scene),
               iteration,
               phases.size());

  // Update LP elements for all phases in this scene before solving.
  // Checks iteration skip/force flags and calls SystemLP::update_lp()
  // per phase.  Runs in the per-scene thread.
  dispatch_update_lp(scene, iteration);

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

    // Apply saved forward-pass solution from the previous iteration as
    // warm-start hint.  Dimension mismatches (new cut rows) are handled
    // by set_warm_start_solution() via zero-padding.
    if (effective_opts.warm_start) {
      li.set_warm_start_solution(state.forward_col_sol, state.forward_row_dual);
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
        // Increment infeasibility counter for this (scene, phase)
        ++m_infeasibility_counter_[scene][phase];

        // Cache solution data for the backward pass
        const auto obj = solved_li.get_obj_value();
        state.forward_full_obj = obj;

        const auto rc = solved_li.get_col_cost();
        state.forward_col_cost.assign(rc.begin(), rc.end());

        // Save primal/dual solution for aperture warm-start (pre-padded
        // so set_warm_start_solution can use a subspan without allocation).
        const auto n_links = state.outgoing_links.size();
        assign_padded(state.forward_col_sol, solved_li.get_col_sol(), n_links);
        assign_padded(
            state.forward_row_dual, solved_li.get_row_dual(), 2 * n_links);

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

      // Cache solution data for the backward pass
      const auto obj = li.get_obj_value();
      state.forward_full_obj = obj;

      const auto rc = li.get_col_cost();
      state.forward_col_cost.assign(rc.begin(), rc.end());

      // Save primal/dual solution for aperture warm-start (pre-padded
      // so set_warm_start_solution can use a subspan without allocation).
      const auto n_links = state.outgoing_links.size();
      assign_padded(state.forward_col_sol, li.get_col_sol(), n_links);
      assign_padded(state.forward_row_dual, li.get_row_dual(), 2 * n_links);

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

}  // namespace gtopt
