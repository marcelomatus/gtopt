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
#include <gtopt/lp_context.hpp>
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

  // Per-scene "starting" trace dropped to DEBUG: the aggregate
  // "SDDP Forward [iN]: dispatching M scene(s) ..." line in
  // sddp_method.cpp already covers the user-facing fan-out, and
  // emitting one INFO line per scene per iteration was just noise
  // (16 lines at the same millisecond on this run).
  [[maybe_unused]] const auto fwd_tid = std::this_thread::get_id();
  SPDLOG_DEBUG("SDDP Forward [i{} s{}]: starting ({} phases) [thread {}]",
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
        // Alpha is registered as a state variable; its freshly-captured
        // col_sol() is the LP solution value (already in scaled LP units).
        const auto* alpha_svar = find_alpha_state_var(
            planning_lp().simulation(), scene_index, phase_index);
        const auto alpha_val =
            (alpha_svar != nullptr) ? alpha_svar->col_sol() * sa : 0.0;
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

        // PLP-style: install a Benders feasibility cut on phase p-1
        // right from the forward pass, so the next forward iteration
        // avoids regenerating the same infeasible trial point.  Uses
        // reduced costs on the dependent state-var columns from the
        // elastic clone (cost_raw at optimum of the relaxed LP).  When
        // the per-(scene, phase) infeasibility counter exceeds
        // multi_cut_threshold, additionally install one bound cut per
        // activated slack (`build_multi_cuts`).
        if (phase_index) {
          const auto prev_phase_index = previous(phase_index);
          auto& prev_sys = planning_lp().system(scene_index, prev_phase_index);
          const auto& prev_state = phase_states[prev_phase_index];
          prev_sys.ensure_lp_built();
          auto& prev_li = prev_sys.linear_interface();

          const auto infeas_count =
              m_infeasibility_counter_[scene_index][phase_index];

          // Resolve α column freshly from the registry so we never read
          // a stale cached index (low_memory reconstruct paths).
          const auto* prev_alpha_svar = find_alpha_state_var(
              planning_lp().simulation(), scene_index, prev_phase_index);
          const auto prev_alpha_col = (prev_alpha_svar != nullptr)
              ? prev_alpha_svar->col()
              : ColIndex {unknown_index};

          auto feas_cut = build_benders_cut(prev_alpha_col,
                                            prev_state.outgoing_links,
                                            solved_li.get_col_cost_raw(),
                                            solved_li.get_obj_value(),
                                            m_options_.scale_alpha,
                                            m_options_.cut_coeff_eps);
          feas_cut.class_name = "Sddp";
          feas_cut.constraint_name = "fcut";
          feas_cut.context = make_iteration_context(scene_uid(scene_index),
                                                    phase_uid(phase_index),
                                                    iteration_index,
                                                    infeas_count);
          rescale_benders_cut(
              feas_cut, prev_alpha_col, m_options_.cut_coeff_max);
          filter_cut_coefficients(
              feas_cut, prev_alpha_col, m_options_.cut_coeff_eps);
          {
            const auto cut_row = prev_li.add_row(feas_cut);
            store_cut(scene_index,
                      prev_phase_index,
                      feas_cut,
                      CutType::Feasibility,
                      cut_row);
          }

          const bool use_multi_cut =
              (m_options_.elastic_filter_mode == ElasticFilterMode::multi_cut)
              || (m_options_.multi_cut_threshold == 0)
              || (m_options_.multi_cut_threshold > 0
                  && infeas_count > m_options_.multi_cut_threshold);

          int mc_added = 0;
          if (use_multi_cut) {
            auto mc_cuts =
                build_multi_cuts(*elastic_result,
                                 prev_state.outgoing_links,
                                 make_iteration_context(scene_uid(scene_index),
                                                        phase_uid(phase_index),
                                                        iteration_index,
                                                        infeas_count));
            for (auto& mc : mc_cuts) {
              const auto cut_row = prev_li.add_row(mc);
              store_cut(scene_index,
                        prev_phase_index,
                        mc,
                        CutType::Feasibility,
                        cut_row);
              ++mc_added;
            }
          }

          SPDLOG_INFO(
              "SDDP Forward [i{} s{} p{}]: installed fcut on prev phase {}"
              " (+{} multi-cut bounds) [infeas_count={}]",
              iteration_index,
              scene_uid(scene_index),
              phase_uid(phase_index),
              phase_uid(prev_phase_index),
              mc_added,
              infeas_count);
          // Do not release prev_sys backend here — the backward pass
          // will visit phase p-1 shortly and re-ensure_lp_built() itself.

          // ── Sim-pass compress-mode backward recovery (Proposal 2) ──
          //
          // Under low_memory != off, the inline per-cell `write_out()`
          // has already emitted the previous phase (q = p-1) with its
          // pre-fcut solution — now stale because the fcut we just
          // installed changes q's LP.  Additionally the elastic
          // contribution we just added to total_opex belongs to a
          // relaxed LP, not the real one.
          //
          // This is a single-step sync recovery: re-solve q with the
          // new fcut, then re-solve p with q's new trial values.  If
          // both succeed, replace the stale output and the elastic
          // total_opex contribution with the real values.  If either
          // step fails, abandon the recovery (keep the elastic result
          // on disk) — deeper multi-step recursion is tracked as a
          // future enhancement.
          const bool sim_low_mem = m_in_simulation_
              && m_options_.low_memory_mode != LowMemoryMode::off;
          if (sim_low_mem) {
            auto& q_sys = planning_lp().system(scene_index, prev_phase_index);
            auto& q_li = q_sys.linear_interface();

            // Clear stale output marker before re-solve.  On compress
            // the LP matrix is replayed from the snapshot with the
            // just-added fcut in `m_active_cuts_`; trial-value bounds
            // are reset to defaults and must be re-propagated when
            // q > 0.
            q_sys.clear_output_written();
            q_sys.ensure_lp_built();
            if (prev_phase_index > first_phase_index()) {
              const auto q_prev = previous(prev_phase_index);
              propagate_trial_values(phase_states[q_prev].outgoing_links, q_li);
            }
            q_li.set_log_tag(sddp_log("ForwardRecover-q",
                                      iteration_index,
                                      scene_uid(scene_index),
                                      phase_uid(prev_phase_index)));
            auto q_res = q_li.resolve(effective_opts);

            if (q_res.has_value() && q_li.is_optimal()) {
              // q is now feasible under the new fcut.  Capture fresh
              // state, replace the elastic total_opex contribution
              // for q with the real one, re-emit q.
              const auto q_obj = q_li.get_obj_value();
              const auto q_sol = q_li.get_col_sol_raw();
              const auto q_rc = q_li.get_col_cost_raw();
              capture_state_variable_values(
                  scene_index, prev_phase_index, q_sol, q_rc);
              const auto* q_alpha = find_alpha_state_var(
                  planning_lp().simulation(), scene_index, prev_phase_index);
              const auto q_alpha_val =
                  (q_alpha != nullptr) ? q_alpha->col_sol() * sa : 0.0;
              auto& q_state = phase_states[prev_phase_index];
              const auto q_old_fwd_obj = q_state.forward_objective;
              q_state.forward_objective = q_obj - q_alpha_val;
              q_state.forward_full_obj = q_obj;
              total_opex += (q_state.forward_objective - q_old_fwd_obj);

              q_sys.write_out();
              q_sys.release_backend();

              // Now re-solve p with q's new trial values.  The LP for
              // p already holds the relaxed solution in its backend
              // (we never released between the elastic solve and this
              // recovery attempt inside this iteration) — release it
              // first so ensure_lp_built replays the unmodified p
              // snapshot, then propagate fresh trial values.
              auto& p_sys = planning_lp().system(scene_index, phase_index);
              auto& p_li = p_sys.linear_interface();
              p_sys.clear_output_written();
              p_sys.release_backend();
              p_sys.ensure_lp_built();
              propagate_trial_values(
                  phase_states[prev_phase_index].outgoing_links, p_li);
              p_li.set_log_tag(sddp_log("ForwardRecover-p",
                                        iteration_index,
                                        scene_uid(scene_index),
                                        phase_uid(phase_index)));
              auto p_res = p_li.resolve(effective_opts);

              if (p_res.has_value() && p_li.is_optimal()) {
                // Full recovery: p is optimal on the real LP.  Replace
                // the elastic contribution in total_opex with the
                // real one, refresh state, re-emit p, reset infeas.
                const auto p_obj = p_li.get_obj_value();
                const auto p_sol = p_li.get_col_sol_raw();
                const auto p_rc = p_li.get_col_cost_raw();
                capture_state_variable_values(
                    scene_index, phase_index, p_sol, p_rc);
                const auto* p_alpha = find_alpha_state_var(
                    planning_lp().simulation(), scene_index, phase_index);
                const auto p_alpha_val =
                    (p_alpha != nullptr) ? p_alpha->col_sol() * sa : 0.0;
                const auto p_old_fwd_obj = state.forward_objective;
                state.forward_objective = p_obj - p_alpha_val;
                state.forward_full_obj = p_obj;
                total_opex += (state.forward_objective - p_old_fwd_obj);

                // Elastic trial is no longer in effect — reset counter.
                m_infeasibility_counter_[scene_index][phase_index] = 0;
                m_phase_grid_.record(iteration_index,
                                     scene_uid(scene_index),
                                     phase_index,
                                     GridCell::Forward);
                update_max_kappa(
                    scene_index, phase_index, p_li, iteration_index);

                p_sys.write_out();
                p_sys.release_backend();

                SPDLOG_INFO(
                    "SDDP ForwardRecover [i{} s{} p{}]: recovered "
                    "(q-obj {:.4f}, p-obj {:.4f})",
                    iteration_index,
                    scene_uid(scene_index),
                    phase_uid(phase_index),
                    q_obj,
                    p_obj);
              } else {
                SPDLOG_WARN(
                    "SDDP ForwardRecover [i{} s{} p{}]: p-resolve "
                    "non-optimal after q recovery (status {}); "
                    "keeping elastic result",
                    iteration_index,
                    scene_uid(scene_index),
                    phase_uid(phase_index),
                    p_li.get_status());
                p_sys.release_backend();
              }
            } else {
              SPDLOG_WARN(
                  "SDDP ForwardRecover [i{} s{} p{}]: q-resolve "
                  "non-optimal with new fcut (status {}); keeping "
                  "elastic result",
                  iteration_index,
                  scene_uid(scene_index),
                  phase_uid(phase_index),
                  q_li.get_status());
              q_sys.release_backend();
            }
          }
        }
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
      const auto* alpha_svar = find_alpha_state_var(
          planning_lp().simulation(), scene_index, phase_index);
      const auto alpha_val =
          (alpha_svar != nullptr) ? alpha_svar->col_sol() * sa : 0.0;
      state.forward_objective = obj - alpha_val;

      // Simulation-pass per-cell emit.
      //
      // Under low_memory != off the backend is about to be released and
      // its primal/dual vectors lost, so we MUST write here, while
      // `col_sol` and `row_dual` still carry the real solved values.
      //
      // Under low_memory == off the backend survives past the scene
      // (release_backend is a no-op).  `PlanningLP::write_out` will
      // later walk every cell and emit from the still-live backend —
      // which lets the sim-pass retry loop in `sddp_iteration.cpp`
      // re-solve any phase whose trial values were invalidated by an
      // fcut installed downstream, without needing to first undo a
      // premature write.  Skipping the inline emit here is therefore
      // the hook that enables backward-recovery semantics for the
      // off-mode sim pass (Proposal 1).
      if (m_in_simulation_ && m_options_.low_memory_mode != LowMemoryMode::off)
      {
        system.write_out();
      }

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
  // Diagnostic: phase 0's backend state at end-of-scene forward.
  // compute_iteration_bounds reads phase-0 obj_value for the LB,
  // so if any scene leaves phase 0 unreleased/non-optimal here the
  // LB will read a stale or zeroed cached value.
  {
    auto& p0_sys = planning_lp().system(scene_index, first_phase_index());
    auto& p0_li = p0_sys.linear_interface();
    SPDLOG_INFO(
        "SDDP Forward [i{} s{}]: phase-0 end-of-scene state: released={}"
        " optimal={} obj_value={:.4g}",
        iteration_index,
        scene_uid(scene_index),
        p0_li.is_backend_released(),
        p0_li.is_optimal(),
        p0_li.get_obj_value());
  }
  return total_opex;
}

}  // namespace gtopt
