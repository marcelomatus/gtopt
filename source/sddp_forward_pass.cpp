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
  const auto fwd_scene_start = std::chrono::steady_clock::now();

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
               uid_of(scene_index),
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
                                          uid_of(scene_index),
                                          uid_of(phase_index))),
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
          uid_of(scene_index),
          uid_of(phase_index),
          prev_st.outgoing_links.size(),
          uid_of(prev_phase_index));
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
                     uid_of(scene_index),
                     uid_of(phase_index),
                     updated);
      }
    }

    // If lp_debug is enabled, write LP file (pre-solve state) then optionally
    // submit gzip compression as a fire-and-forget async task.
    // Selective filters (scene/phase range) skip non-matching LPs.
    if (m_options_.lp_debug) {
      const bool in_range = in_lp_debug_range(uid_of(scene_index),
                                              uid_of(phase_index),
                                              m_options_.lp_debug_scene_min,
                                              m_options_.lp_debug_scene_max,
                                              m_options_.lp_debug_phase_min,
                                              m_options_.lp_debug_phase_max);
      if (in_range) {
        const auto dbg_stem = (std::filesystem::path(m_options_.log_directory)
                               / std::format(sddp_file::debug_lp_fmt,
                                             uid_of(scene_index),
                                             uid_of(phase_index),
                                             iteration_index))
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
      li.set_log_file(
          (std::filesystem::path(m_options_.log_directory)
           / as_label(
               li.solver_name(), uid_of(scene_index), uid_of(phase_index)))
              .string());
    }

    // Tag the LP with the SDDP Forward key so fallback warnings emitted
    // by LinearInterface::resolve() carry the same context as the
    // surrounding SDDP info logs.
    li.set_log_tag(sddp_log(
        "Forward", iteration_index, uid_of(scene_index), uid_of(phase_index)));

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
            uid_of(scene_index),
            uid_of(phase_index),
            m_options_.solve_timeout,
            solve_status);
        return std::unexpected(Error {
            .code = ErrorCode::SolverError,
            .message = std::format("{}: solve timeout ({:.1f}s) (status {})",
                                   sddp_log("Forward",
                                            iteration_index,
                                            uid_of(scene_index),
                                            uid_of(phase_index)),
                                   m_options_.solve_timeout,
                                   solve_status),
            .status = solve_status,
        });
      }

      // Pre-elastic notice demoted to DEBUG: the final outcome line
      // ("elastic ok" + "installed fcut") at INFO level is the one-line
      // summary callers expect per infeasible LP.  Keep the status code
      // accessible under `-v` / trace log for deep post-mortems.
      SPDLOG_DEBUG(
          "SDDP Forward [i{} s{} p{}]: non-optimal (status {}), trying elastic"
          " solve",
          iteration_index,
          uid_of(scene_index),
          uid_of(phase_index),
          solve_status);
      // Clone the LP, apply elastic filter, and solve the clone.
      // The original LP remains unmodified (PLP clone pattern).
      auto elastic_result = elastic_solve(scene_index, phase_index, opts);

      // Release solver backend — no-op when low_memory is off.
      system.release_backend();

      if (elastic_result.has_value()) {
        auto& solved_li = elastic_result->clone;
        m_phase_grid_.record(iteration_index,
                             uid_of(scene_index),
                             phase_index,
                             GridCell::Elastic);
        // Track max kappa from elastic solve
        update_max_kappa(scene_index, phase_index, solved_li, iteration_index);
        // Increment infeasibility counter for this (scene, phase)
        ++m_infeasibility_counter_[scene_index][phase_index];

        // Cache solution data for the backward pass (physical space).
        const auto obj = solved_li.get_obj_value();
        state.forward_full_obj_physical = solved_li.get_obj_value_physical();

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

        // "elastic ok" summary moved below and merged with the fcut-install
        // line so each infeasible LP produces exactly one INFO line.

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

          // Physical-space fcut: rc from the elastic clone, trial from
          // each link's source StateVariable.  `add_row` on the
          // equilibrated prev LP folds col_scales + row-max, so the
          // prior `rescale_benders_cut` pass is no longer needed.
          auto feas_cut =
              build_benders_cut_physical(prev_alpha_col,
                                         prev_state.outgoing_links,
                                         solved_li,
                                         solved_li.get_obj_value_physical(),
                                         m_options_.cut_coeff_eps);
          feas_cut.class_name = sddp_alpha_class_name;
          feas_cut.constraint_name = sddp_fcut_constraint_name;
          feas_cut.context = make_iteration_context(uid_of(scene_index),
                                                    uid_of(phase_index),
                                                    iteration_index,
                                                    infeas_count);
          // Release α's bootstrap pin on the previous phase so the
          // fcut's rhs can represent arbitrary future-cost values.
          free_alpha(scene_index, prev_phase_index);
          {
            const auto cut_row =
                prev_li.add_row(feas_cut, m_options_.cut_coeff_eps);
            store_cut(scene_index,
                      prev_phase_index,
                      feas_cut,
                      CutType::Feasibility,
                      cut_row);
          }

          // Chinneck mode also emits per-bound cuts, but only on the IIS
          // subset (chinneck_filter_solve cleared sup_col/sdn_col on the
          // non-essential links so build_multi_cuts skips them naturally).
          //
          // Comparison is `>=`: a multi_cut_threshold of N means "switch
          // on the Nth fcut event at this (scene, phase)".  The counter
          // is persistent (does not reset on successful solves) so this
          // counts *cumulative* events, not consecutive ones.
          const bool use_multi_cut =
              (m_options_.elastic_filter_mode == ElasticFilterMode::multi_cut)
              || (m_options_.elastic_filter_mode == ElasticFilterMode::chinneck)
              || (m_options_.multi_cut_threshold == 0)
              || (m_options_.multi_cut_threshold > 0
                  && infeas_count >= m_options_.multi_cut_threshold);

          int mc_added = 0;
          if (use_multi_cut) {
            auto mc_cuts =
                build_multi_cuts(*elastic_result,
                                 prev_state.outgoing_links,
                                 make_iteration_context(uid_of(scene_index),
                                                        uid_of(phase_index),
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

          // List the state-variable elements actually present in the
          // installed cut (survivors of cut_coeff_eps filtering), so
          // the diagnostic points at the elements driving the fcut.
          // Format: "Class:uid:col_name, ..." (e.g. "Reservoir:8:efin").
          std::string state_elems;
          for (const auto& link : prev_state.outgoing_links) {
            if (!feas_cut.cmap.contains(link.source_col)) {
              continue;
            }
            if (!state_elems.empty()) {
              state_elems += ", ";
            }
            state_elems += std::format(
                "{}:{}:{}", link.class_name, Index {link.uid}, link.col_name);
          }

          // One-line per infeasible LP, emitted *after* the fcut (and any
          // multi-cuts) have been installed on prev_li.  Timing is
          // deliberate — the log signals a completed cut install, not
          // merely the detection of infeasibility.
          SPDLOG_INFO(
              "SDDP Forward [i{} s{} p{}]: elastic → fcut on p{} "
              "(obj={:.4g} alpha={:.4g} opex={:.4g} infeas_count={}{}) "
              "state=[{}]",
              iteration_index,
              uid_of(scene_index),
              uid_of(phase_index),
              uid_of(prev_phase_index),
              obj,
              alpha_val,
              state.forward_objective,
              infeas_count,
              mc_added > 0 ? std::format(" +{}mc", mc_added) : "",
              state_elems);
          // Do not release prev_sys backend here — the backward pass
          // will visit phase p-1 shortly and re-ensure_lp_built() itself.
          //
          // Two-pass simulation (Pass 1 only): the fcut just installed
          // lives persistently on prev_li's m_active_cuts_, so Pass 2
          // sees it on every cell whether backends stayed alive (off)
          // or were reconstructed from snapshot (compress/rebuild).
          // No in-forward-pass backward recovery needed — Pass 2 is the
          // single recovery step.
        }
      } else {
        // Elastic filter could not produce a feasibility cut.  Two
        // mutually exclusive reasons:
        //   A. phase_index == 0 (no predecessor phase to cut on)
        //   B. the state-variable-relaxed clone was itself infeasible
        //      (infeasibility rooted outside state-variable bounds —
        //       e.g. demand balance, flow limits, or a pre-installed
        //       cut row — which the elastic filter cannot relax)
        // Per SDDP theory, no recovery is possible from either — the
        // scene is declared infeasible for this iteration.  The caller
        // (run_forward_pass_all_scenes) sets scene_feasible[s] = 0 when
        // it sees the returned Error; compute_iteration_bounds then
        // excludes this scene from UB/LB.  Retries on the same state
        // cannot change this outcome.
        //
        // Dump the original (pre-elastic) LP to <log_directory>/
        // error_s<scene_uid>_p<phase_uid>_i<iteration_index>.lp so the
        // user can diagnose offline.  `elastic_solve` never touches the
        // original LP (PLP clone pattern), so `system.linear_interface()`
        // still holds the state that failed.  `release_backend()` above
        // may have dropped the solver object under low_memory; rebuild
        // it before writing.  Failure to write is non-fatal — we log a
        // warning and continue returning the original error.
        std::string saved_error_lp;
        {
          std::error_code mkdir_ec;
          std::filesystem::create_directories(m_options_.log_directory,
                                              mkdir_ec);
          system.ensure_lp_built();
          const auto stem = (std::filesystem::path(m_options_.log_directory)
                             / std::format(sddp_file::error_lp_fmt,
                                           uid_of(scene_index),
                                           uid_of(phase_index),
                                           iteration_index))
                                .string();
          // `linear_interface().write_lp` appends ".lp" but keeps the
          // stem verbatim.  `SystemLP::write_lp` would additionally
          // tack on `_scene_X_phase_Y`, duplicating the UIDs already
          // baked into our stem — so we bypass it.
          if (auto wr = system.linear_interface().write_lp(stem); wr) {
            saved_error_lp = stem + ".lp";
          } else {
            spdlog::warn(
                "SDDP Forward [i{} s{} p{}]: failed to save error LP "
                "to {}: {}",
                iteration_index,
                uid_of(scene_index),
                uid_of(phase_index),
                stem,
                wr.error().message);
          }
        }

        spdlog::warn(
            "SDDP Forward [i{} s{} p{}]: elastic filter produced no "
            "feasibility cut — declaring phase p{} and scene s{} "
            "infeasible for iter i{} (solver status {}, reason: {}){}",
            iteration_index,
            uid_of(scene_index),
            uid_of(phase_index),
            uid_of(phase_index),
            uid_of(scene_index),
            iteration_index,
            solve_status,
            !phase_index ? "no predecessor phase to cut on"
                         : "relaxed clone infeasible",
            saved_error_lp.empty()
                ? std::string {}
                : std::format(" [LP saved to {}]", saved_error_lp));
        return std::unexpected(Error {
            .code = ErrorCode::SolverError,
            .message = std::format("{}: elastic filter produced no "
                                   "feasibility cut (status {})",
                                   sddp_log("Forward",
                                            iteration_index,
                                            uid_of(scene_index),
                                            uid_of(phase_index)),
                                   solve_status),
        });
      }
    } else {
      // Phase solved normally — counter is intentionally NOT reset.
      // Persistent counter (PLP convention): a (scene, phase) that flaps
      // between feasible and infeasible across iterations should still
      // accumulate towards the multi_cut auto-switch threshold; the
      // intermittent successes don't mean the elastic cut history is
      // adequate.  See docs/methods/sddp.md §S5.4.
      m_phase_grid_.record(
          iteration_index, uid_of(scene_index), phase_index, GridCell::Forward);
      // Track max kappa from forward solve
      update_max_kappa(scene_index, phase_index, li, iteration_index);

      // Cache solution data for the backward pass — must happen before
      // release_backend() which may discard cached solution vectors.
      // Store the objective in physical ($) space so the backward pass
      // can call `build_benders_cut_physical` without re-applying
      // `scale_objective`.
      const auto obj = li.get_obj_value();
      state.forward_full_obj_physical = li.get_obj_value_physical();

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

      // Release solver backend — no-op when low_memory is off.
      //
      // The flat-LP snapshot IS retained here even during simulation
      // Pass 1, because a later phase's solve may hit the elastic
      // branch and need to `ensure_lp_built()` an earlier phase to
      // install an fcut on it (see the elastic → fcut path above).
      // That reconstruct needs the earlier phase's snapshot intact.
      //
      // Aggressive snapshot drop runs AFTER Pass 1's retry loop has
      // converged — at that point no further in-pass reconstruct is
      // possible and only `PlanningLP::write_out` remains, which reads
      // from the Phase-2a cache and re-flattens `System` element
      // arrays (no snapshot needed).  See `drop_sim_snapshots()` on
      // PlanningLP called by `SDDPMethod::simulation_pass`.
      system.release_backend();

      // Guard against solver returning "optimal" with NaN values
      // (can happen when inherited cuts cause ill-conditioning).
      if (std::isnan(state.forward_objective)) {
        SPDLOG_WARN(
            "SDDP Forward [i{} s{} p{}]: solve returned optimal but"
            " forward_objective is NaN (obj={}, alpha={})",
            iteration_index,
            uid_of(scene_index),
            uid_of(phase_index),
            obj,
            alpha_val);
        return std::unexpected(Error {
            .code = ErrorCode::SolverError,
            .message = std::format("{}: optimal status but NaN in solution",
                                   sddp_log("Forward",
                                            iteration_index,
                                            uid_of(scene_index),
                                            uid_of(phase_index))),
        });
      }

      total_opex += state.forward_objective;

      SPDLOG_TRACE(
          "SDDP Forward [i{} s{} p{}]: obj={:.4f} alpha={:.4f}"
          " opex={:.4f}",
          iteration_index,
          uid_of(scene_index),
          uid_of(phase_index),
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
        uid_of(scene_index));
    return std::unexpected(Error {
        .code = ErrorCode::SolverError,
        .message = std::format(
            "{}: forward pass total cost is NaN",
            sddp_log("Forward", iteration_index, uid_of(scene_index))),
    });
  }

  const auto fwd_scene_s =
      std::chrono::duration<double>(std::chrono::steady_clock::now()
                                    - fwd_scene_start)
          .count();
  SPDLOG_INFO(
      "SDDP Forward [i{} s{}]: done, total_opex={:.4f} ({:.3f}s) [thread {}]",
      iteration_index,
      uid_of(scene_index),
      total_opex,
      fwd_scene_s,
      std::hash<std::thread::id> {}(fwd_tid) % 10000);
  return total_opex;
}

}  // namespace gtopt
