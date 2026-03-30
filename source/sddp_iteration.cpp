/**
 * @file      sddp_iteration.cpp
 * @brief     SDDP main iteration loop (solve method)
 * @date      2026-03-22
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Extracted from sddp_solver.cpp — implements the main solve() loop
 * that orchestrates forward/backward passes, convergence checks,
 * cut sharing, monitoring, and persistence.
 */

#include <chrono>
#include <vector>

#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_monitor.hpp>
#include <gtopt/sddp_pool.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

auto SDDPMethod::solve(const SolverOptions& lp_opts)
    -> std::expected<std::vector<SDDPIterationResult>, Error>
{
  // Validate preconditions
  if (auto err = validate_inputs()) {
    return std::unexpected(std::move(*err));
  }

  // lp_build: the LP matrix is already built (PlanningLP constructor).
  // Return an empty results vector immediately — no solving, no initialization.
  if (m_options_.lp_build) {
    SPDLOG_INFO("SDDP: lp_build mode — LP built, skipping all solving");
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
  const auto pool_start = std::chrono::steady_clock::now();
  auto sddp_pool = make_sddp_work_pool();
  auto aux_pool = make_solver_work_pool();
  m_pool_ = sddp_pool.get();
  m_aux_pool_ = aux_pool.get();
  m_benders_cut_.set_pool(m_aux_pool_);
  m_lp_debug_writer_ = LpDebugWriter(
      m_options_.log_directory, m_options_.lp_debug_compression, m_aux_pool_);
  const auto pool_create_s = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - pool_start)
                                 .count();
  SPDLOG_INFO("SDDP: work pools created ({:.2f}s)", pool_create_s);

  reset_live_state();

  // Compute per-pass solver options: forward/backward options override
  // the base lp_opts when configured in SDDPOptions.
  const auto fwd_opts = m_options_.forward_solver_options.value_or(lp_opts);
  const auto bwd_opts = m_options_.backward_solver_options.value_or(lp_opts);

  // Monitoring setup
  const auto solve_start = std::chrono::steady_clock::now();
  const std::string& status_file = m_options_.api_status_file;
  SolverMonitor monitor(m_options_.api_update_interval);
  if (m_options_.enable_api && !status_file.empty()) {
    monitor.start(*sddp_pool, solve_start, "SDDPMonitor");
  }

  std::vector<SDDPIterationResult> results;
  results.reserve(static_cast<std::size_t>(m_options_.max_iterations) + 1);

  // ── Training iterations (forward + backward passes) ──
  // When max_iterations == 0, this loop is skipped entirely and only the
  // simulation pass below is executed.
  if (m_options_.max_iterations > 0) {
    const auto iter_last =
        m_iteration_offset_ + IterationIndex {m_options_.max_iterations - 1};
    for (auto iter = m_iteration_offset_; iter <= iter_last; ++iter) {
      const auto iter_start = std::chrono::steady_clock::now();

      if (should_stop()) {
        SPDLOG_INFO("SDDP: stop requested, halting after {} iterations",
                    iter - m_iteration_offset_);
        break;
      }

      SPDLOG_INFO("SDDP: === iteration {} / {} ===", iter, iter_last);

      SDDPIterationResult ir {
          .iteration = iter,
      };
      m_benders_cut_.reset_infeasible_cut_count();

      // ── Forward pass ──
      SPDLOG_DEBUG("SDDP: starting forward pass (iter {})", iter);
      auto fwd = run_forward_pass_all_scenes(*sddp_pool, fwd_opts, iter);
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
      const auto prob_mode =
          planning_lp().planning().simulation.probability_rescale.value_or(
              ProbabilityRescaleMode::runtime);
      const auto weights =
          compute_scene_weights(scenes, fwd->scene_feasible, prob_mode);
      compute_iteration_bounds(ir, fwd->scene_feasible, weights);

      // ── Backward pass ──
      SPDLOG_DEBUG("SDDP: starting backward pass (iter {})", iter);
      // Save per-scene cut counts for cut sharing offset tracking
      const auto cuts_before = m_cut_store_.stored_cuts().size();
      const auto num_scenes_bwd =
          static_cast<Index>(planning_lp().simulation().scenes().size());
      m_cut_store_.scene_cuts_before().resize(
          static_cast<std::size_t>(num_scenes_bwd));
      for (const auto scene : iota_range<SceneIndex>(0, num_scenes_bwd)) {
        m_cut_store_.scene_cuts_before()[static_cast<std::size_t>(scene)] =
            m_cut_store_.scene_cuts()[scene].size();
      }
      auto bwd = run_backward_pass_all_scenes(
          fwd->scene_feasible, *sddp_pool, bwd_opts, iter);
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
      if (m_options_.cut_sharing == CutSharingMode::none) {
        apply_cut_sharing_for_iteration(cuts_before, iter);
      }

      // ── Cut pruning ──
      if (m_options_.max_cuts_per_phase > 0
          && m_options_.cut_prune_interval > 0)
      {
        const auto iter_offset = static_cast<int>(iter - m_iteration_offset_);
        if (iter_offset > 0 && iter_offset % m_options_.cut_prune_interval == 0)
        {
          prune_inactive_cuts();
        }
      }

      // ── Cap stored cuts ──
      cap_stored_cuts();

      // ── Convergence + live-query update ──
      finalize_iteration_result(ir, iter);

      // ── Extended convergence criteria (governed by convergence_mode) ──
      //
      // The primary gap test (gap < convergence_tol) is always active and
      // has already been evaluated by finalize_iteration_result().
      //
      // The additional criteria below are gated by convergence_mode:
      //   gap_only       → nothing extra
      //   gap_stationary → + stationary gap detection
      //   statistical    → + stationary + CI test + CI+stationary fallback

      const auto mode = m_options_.convergence_mode;
      const bool past_min_iters =
          (iter >= m_iteration_offset_
               + IterationIndex {m_options_.min_iterations - 1});

      // Stationary gap tracking: compute gap_change over the look-back
      // window.  Active for gap_stationary and statistical modes.
      bool gap_is_stationary = false;
      if (mode != ConvergenceMode::gap_only && m_options_.stationary_tol > 0.0
          && m_options_.stationary_window > 0)
      {
        const auto window =
            static_cast<std::size_t>(m_options_.stationary_window);
        if (results.size() >= window) {
          const double old_gap = results[results.size() - window].gap;
          ir.gap_change = std::abs(ir.gap - old_gap) / std::max(1e-10, old_gap);
        }
        gap_is_stationary =
            (ir.gap_change < m_options_.stationary_tol
             && ir.gap_change < 1.0);  // 1.0 = default / not yet computed
      }

      // ── Stationary gap convergence ──
      // Active for gap_stationary and statistical modes.
      // Declare convergence when the gap stops improving (non-zero gap
      // accepted).  In pure Benders (single scene / no apertures), this
      // is the main fallback since the CI test requires N > 1.
      if (!ir.converged && past_min_iters && gap_is_stationary
          && mode != ConvergenceMode::gap_only)
      {
        ir.converged = true;
        ir.stationary_converged = true;
        m_converged_.store(true);
        SPDLOG_INFO(
            "SDDP iter {}: stationary gap convergence "
            "(gap_change={:.6f} < stationary_tol={:.6f}) [CONVERGED]",
            iter,
            ir.gap_change,
            m_options_.stationary_tol);
      }

      // ── Statistical CI convergence ──
      // Active only for statistical mode AND multiple scenes (N > 1).
      // Degrades gracefully: when N == 1 (no apertures, pure Benders,
      // or all apertures infeasible), this block is skipped and the
      // solver relies on the primary gap + stationary criteria above.
      //
      // PLP-style (plp-pdconvrg.f lines 84-91):
      //   Error = ABS(ZSPFPromBest - ZSDF)
      //   epsilon = SQRT(ZSPFVar) * UmbIntConf
      //
      // Two sub-criteria:
      //  (a) CI test:  UB - LB <= z_{α/2} * σ
      //  (b) CI + stationarity:  gap > z*σ but gap_change < stationary_tol
      if (!ir.converged && mode == ConvergenceMode::statistical
          && m_options_.convergence_confidence > 0.0
          && ir.scene_upper_bounds.size() > 1 && past_min_iters)
      {
        const auto n = static_cast<double>(ir.scene_upper_bounds.size());
        double mean = 0.0;
        for (const auto ub : ir.scene_upper_bounds) {
          mean += ub;
        }
        mean /= n;
        double var = 0.0;
        for (const auto ub : ir.scene_upper_bounds) {
          var += (ub - mean) * (ub - mean);
        }
        var /= (n - 1.0);
        const auto sigma = std::sqrt(var);

        const auto alpha = 1.0 - m_options_.convergence_confidence;
        const auto z_score = [alpha]
        {
          if (alpha <= 0.01) {
            return 2.576;
          }
          if (alpha <= 0.05) {
            return 1.960;
          }
          if (alpha <= 0.10) {
            return 1.645;
          }
          return 1.282;  // 80%
        }();

        const auto gap_abs = ir.upper_bound - ir.lower_bound;
        const auto ci_threshold = z_score * sigma;

        // (a) Gap within CI: LB inside the UB confidence interval.
        if (gap_abs <= ci_threshold) {
          ir.converged = true;
          ir.statistical_converged = true;
          m_converged_.store(true);
          SPDLOG_INFO(
              "SDDP iter {}: statistical CI convergence "
              "(UB-LB={:.4f} <= z*σ={:.4f}, z={:.3f}, "
              "σ={:.4f}, N={}) [CONVERGED]",
              iter,
              gap_abs,
              ci_threshold,
              z_score,
              sigma,
              ir.scene_upper_bounds.size());
        }
        // (b) Gap exceeds CI but is no longer improving.
        else if (gap_is_stationary)
        {
          ir.converged = true;
          ir.statistical_converged = true;
          ir.stationary_converged = true;
          m_converged_.store(true);
          SPDLOG_INFO(
              "SDDP iter {}: statistical + stationary convergence "
              "(UB-LB={:.4f} > z*σ={:.4f} but gap_change={:.6f} "
              "< stationary_tol={:.6f}) [CONVERGED]",
              iter,
              gap_abs,
              ci_threshold,
              ir.gap_change,
              m_options_.stationary_tol);
        }
      }

      results.push_back(ir);

      SPDLOG_INFO(
          "SDDP: iter {} done in {:.3f}s (fwd {:.2f}s + bwd {:.2f}s) — "
          "UB={:.4f} LB={:.4f} gap={:.6f} gap_change={:.6f} "
          "cuts={} infeas_cuts={} {}",
          iter,
          ir.iteration_s,
          ir.forward_pass_s,
          ir.backward_pass_s,
          ir.upper_bound,
          ir.lower_bound,
          ir.gap,
          ir.gap_change,
          ir.cuts_added,
          ir.infeasible_cuts_added,
          ir.converged ? "[CONVERGED]" : "");

      // ── Monitoring API and cut persistence ──
      maybe_write_api_status(status_file, results, solve_start, monitor);
      if (m_options_.save_per_iteration) {
        save_cuts_for_iteration(iter, fwd->scene_feasible);
      }

      // ── Iteration callback ──
      if (m_iteration_callback_ && m_iteration_callback_(ir)) {
        SPDLOG_INFO("SDDP: callback requested stop at iter {}", iter);
        break;
      }

      if (ir.converged) {
        break;
      }
    }
  }

  // ── Simulation pass ──
  // Always run a simulation (forward-only) pass after training iterations
  // complete (or when max_iterations == 0).  This evaluates the policy
  // with all accumulated cuts, producing the definitive output.
  // No backward pass is run, so no new optimality cuts are generated.
  // Feasibility cuts (from the elastic filter) may still be produced.
  // By default (save_simulation_cuts=false) they are discarded to ensure
  // hot-start reproducibility.
  const auto cuts_before_simulation = m_cut_store_.stored_cuts().size();
  {
    const auto final_iter = results.empty()
        ? m_iteration_offset_
        : IterationIndex {results.back().iteration + IterationIndex {1}};
    const auto final_start = std::chrono::steady_clock::now();

    SPDLOG_INFO("SDDP: === simulation pass (iter {}) ===", final_iter);

    // Suppress stop checks so the simulation pass always completes.
    // The stop was already honoured by exiting the iteration loop.
    m_in_simulation_ = true;

    SDDPIterationResult ir {
        .iteration = final_iter,
    };
    m_benders_cut_.reset_infeasible_cut_count();

    auto fwd = run_forward_pass_all_scenes(*sddp_pool, fwd_opts, final_iter);
    if (!fwd.has_value()) {
      monitor.stop();
      return std::unexpected(std::move(fwd.error()));
    }
    ir.scene_upper_bounds = std::move(fwd->scene_upper_bounds);
    ir.forward_pass_s = fwd->elapsed_s;
    if (fwd->has_feasibility_issue) {
      ir.feasibility_issue = true;
    }

    const auto& scenes = planning_lp().simulation().scenes();
    const auto weights = compute_scene_weights(scenes, fwd->scene_feasible);
    compute_iteration_bounds(ir, fwd->scene_feasible, weights);

    ir.iteration_s = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - final_start)
                         .count();

    finalize_iteration_result(ir, final_iter);

    // The simulation pass does not determine convergence on its own.
    // It inherits the convergence status from the last training iteration.
    // With max_iterations=0, no training ran, so converged stays false.
    ir.converged = !results.empty() && results.back().converged;

    results.push_back(ir);

    SPDLOG_INFO(
        "SDDP: simulation pass done in {:.3f}s — "
        "UB={:.4f} LB={:.4f} gap={:.6f} gap_change={:.6f} {}",
        ir.iteration_s,
        ir.upper_bound,
        ir.lower_bound,
        ir.gap,
        ir.gap_change,
        ir.converged ? "[CONVERGED]" : "");

    m_in_simulation_ = false;
  }

  // Discard feasibility cuts produced during simulation unless explicitly
  // requested.  This ensures hot-start determinism: reloading cuts from
  // training iterations alone reproduces the same policy.
  if (!m_options_.save_simulation_cuts
      && m_cut_store_.stored_cuts().size() > cuts_before_simulation)
  {
    SPDLOG_INFO("SDDP: discarding {} simulation feasibility cut(s)",
                m_cut_store_.stored_cuts().size() - cuts_before_simulation);
    m_cut_store_.stored_cuts().resize(cuts_before_simulation);
  }

  // ── Cut persistence ──
  if (!m_options_.cuts_output_file.empty() && !results.empty()) {
    const auto num_scenes_final =
        static_cast<Index>(planning_lp().simulation().scenes().size());
    std::vector<uint8_t> final_feasible(
        static_cast<std::size_t>(num_scenes_final), 1U);
    save_cuts_for_iteration(results.back().iteration, final_feasible);

    // Write the combined output file based on cut_recovery_mode:
    //  - none/replace: write all cuts to the combined file
    //  - keep:         do not modify the combined file
    //  - append:       append new cuts to the existing file
    const auto mode = m_options_.cut_recovery_mode;
    if (mode == HotStartMode::append) {
      // Append mode: add newly generated cuts to the existing file.
      const auto& cuts_to_append = m_options_.single_cut_storage
          ? build_combined_cuts()
          : m_cut_store_.stored_cuts();
      auto result = save_cuts_csv(cuts_to_append,
                                  planning_lp(),
                                  m_options_.cuts_output_file,
                                  /*append_mode=*/true);
      if (!result.has_value()) {
        SPDLOG_WARN("SDDP: could not append cuts to combined file: {}",
                    result.error().message);
      }
    } else if (mode != HotStartMode::keep) {
      // none or replace: overwrite the combined file with all cuts
      auto result = save_cuts(m_options_.cuts_output_file);
      if (!result.has_value()) {
        SPDLOG_WARN("SDDP: could not save combined cuts: {}",
                    result.error().message);
      }
    } else {
      SPDLOG_INFO("SDDP: cut_recovery_mode=keep — combined file not modified");
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

}  // namespace gtopt
