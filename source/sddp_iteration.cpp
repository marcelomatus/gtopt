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
#include <thread>
#include <vector>

#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_pool.hpp>
#include <gtopt/solver_status.hpp>

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

  // lp_only: the LP matrix is already built (PlanningLP constructor).
  // Return an empty results vector immediately — no solving, no initialization.
  if (m_options_.lp_only) {
    SPDLOG_INFO("SDDP: lp_only mode — LP built, skipping all solving");
    return std::vector<SDDPIterationResult> {};
  }

  // Bootstrap LP + initialize α vars, state links, hot-start cuts
  if (auto err = initialize_solver(); !err.has_value()) {
    return std::unexpected(std::move(err.error()));
  }

  // Set up work pools for parallel scene processing:
  //  - sddp_pool: SDDPWorkPool with tuple key for main LP solve ordering
  //  - aux_pool:  AdaptiveWorkPool (int64_t key) for BendersCut aperture
  //               solves and LpDebugWriter (gzip compression).
  //               Only created when apertures or lp_debug are active to
  //               avoid idle threads.
  const auto pool_start = std::chrono::steady_clock::now();
  auto sddp_pool = make_sddp_work_pool(m_options_.pool_cpu_factor,
                                       m_options_.pool_memory_limit_mb);
  const bool need_aux_pool =
      (!m_options_.apertures || !m_options_.apertures->empty())
      || !m_options_.log_directory.empty();
  auto aux_pool = need_aux_pool ? make_solver_work_pool()
                                : std::unique_ptr<AdaptiveWorkPool> {};
  m_pool_ = sddp_pool.get();
  m_aux_pool_ = aux_pool.get();  // nullptr when not needed
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
  // Forward pass disables crossover for speed — duals are lazily
  // computed via LinearInterface::ensure_duals() only when the backward
  // pass actually needs them (e.g. no-aperture Benders cut generation).
  auto fwd_opts = m_options_.forward_solver_options.value_or(lp_opts);
  fwd_opts.crossover = false;
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

  // ── Async scene execution ──
  // When scenes are independent (no cut sharing) and max_async_spread > 0,
  // dispatch each scene's forward/backward loop asynchronously.  The
  // SDDPTaskKey priority queue self-regulates: slower scenes (lower
  // iteration) get higher priority.
  {
    const auto num_scenes_check = planning_lp().simulation().scene_count();
    if (m_options_.max_iterations > 0
        && m_options_.cut_sharing == CutSharingMode::none
        && m_options_.max_async_spread > 0 && num_scenes_check > 1)
    {
      return solve_async(*sddp_pool, fwd_opts, bwd_opts);
    }
  }

  // ── Training iterations (forward + backward passes) ──
  // When max_iterations == 0, this loop is skipped entirely and only the
  // simulation pass below is executed.
  if (m_options_.max_iterations > 0) {
    const auto last_iteration_index =
        m_iteration_offset_ + IterationIndex {m_options_.max_iterations - 1};
    for (const auto iteration_index :
         iota_range(m_iteration_offset_, next(last_iteration_index)))
    {
      const auto iteration_start_time = std::chrono::steady_clock::now();

      if (should_stop()) {
        SPDLOG_INFO("{}: stop requested, halting after {} iterations",
                    sddp_log("Iter", iteration_index),
                    iteration_index - m_iteration_offset_);
        break;
      }

      SPDLOG_INFO("{}: === {}/{} ===",
                  sddp_log("Iter", iteration_index),
                  iteration_index,
                  last_iteration_index);

      SDDPIterationResult ir {
          .iteration_index = iteration_index,
      };
      m_benders_cut_.reset_infeasible_cut_count();

      // ── Forward pass ──
      SPDLOG_DEBUG("{}: starting forward pass",
                   sddp_log("Forward", iteration_index));
      auto fwd =
          run_forward_pass_all_scenes(*sddp_pool, fwd_opts, iteration_index);
      if (!fwd.has_value()) {
        monitor.stop();
        return std::unexpected(std::move(fwd.error()));
      }
      ir.scene_upper_bounds = std::move(fwd->scene_upper_bounds);
      ir.forward_pass_s = fwd->elapsed_s;
      if (fwd->has_feasibility_issue) {
        ir.feasibility_issue = true;
        SPDLOG_INFO("{}: forward pass has feasibility issues",
                    sddp_log("Forward", iteration_index));
      }
      SPDLOG_DEBUG("{}: done in {:.3f}s",
                   sddp_log("Forward", iteration_index),
                   fwd->elapsed_s);

      // ── Scene weights and bounds ──
      const auto& scenes = planning_lp().simulation().scenes();
      const auto prob_mode =
          planning_lp().planning().simulation.probability_rescale.value_or(
              ProbabilityRescaleMode::runtime);
      const auto weights =
          compute_scene_weights(scenes, fwd->scene_feasible, prob_mode);
      compute_iteration_bounds(ir, fwd->scene_feasible, weights);

      // ── Backward pass ──
      SPDLOG_DEBUG("{}: starting backward pass",
                   sddp_log("Backward", iteration_index));
      // Save per-scene cut counts for cut sharing offset tracking
      const auto cuts_before = m_cut_store_.stored_cuts().size();
      const auto num_scenes_bwd = planning_lp().simulation().scene_count();
      m_cut_store_.scene_cuts_before().resize(
          static_cast<std::size_t>(num_scenes_bwd));
      for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes_bwd)) {
        m_cut_store_
            .scene_cuts_before()[static_cast<std::size_t>(scene_index)] =
            m_cut_store_.scene_cuts()[scene_index].size();
      }
      auto bwd = run_backward_pass_all_scenes(
          fwd->scene_feasible, *sddp_pool, bwd_opts, iteration_index);
      ir.cuts_added = bwd.total_cuts;
      ir.infeasible_cuts_added = m_benders_cut_.infeasible_cut_count();
      ir.backward_pass_s = bwd.elapsed_s;
      if (bwd.has_feasibility_issue) {
        ir.feasibility_issue = true;
        SPDLOG_INFO("{}: backward pass has feasibility issues",
                    sddp_log("Backward", iteration_index));
      }
      ir.iteration_s =
          std::chrono::duration<double>(std::chrono::steady_clock::now()
                                        - iteration_start_time)
              .count();
      SPDLOG_DEBUG("{}: done in {:.3f}s, {} cuts added",
                   sddp_log("Backward", iteration_index),
                   bwd.elapsed_s,
                   bwd.total_cuts);

      // ── Cut sharing ──
      if (m_options_.cut_sharing == CutSharingMode::none) {
        apply_cut_sharing_for_iteration(cuts_before, iteration_index);
      }

      // ── Cut pruning ──
      if (m_options_.max_cuts_per_phase > 0
          && m_options_.cut_prune_interval > 0)
      {
        const auto iteration_offset_diff =
            iteration_index - m_iteration_offset_;
        if (iteration_offset_diff > 0
            && iteration_offset_diff % m_options_.cut_prune_interval == 0)
        {
          prune_inactive_cuts();
        }
      }

      // ── Cap stored cuts ──
      cap_stored_cuts();

      // ── Convergence + live-query update ──
      finalize_iteration_result(ir, iteration_index);

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
      const bool past_min_iterations =
          (iteration_index >= m_iteration_offset_
               + IterationIndex {m_options_.min_iterations - 1});

      // Stationary gap tracking: compute gap_change over the look-back
      // window.  Active for gap_stationary and statistical modes.
      // Always compute gap_change when at least 1 prior result exists,
      // using min(window, available) as the look-back distance.
      // The stationary convergence test requires the full window.
      bool gap_is_stationary = false;
      if (mode != ConvergenceMode::gap_only && m_options_.stationary_tol > 0.0
          && m_options_.stationary_window > 0)
      {
        const auto window =
            static_cast<std::size_t>(m_options_.stationary_window);
        if (!results.empty()) {
          const auto lookback = std::min(window, results.size());
          const double old_gap = results[results.size() - lookback].gap;
          ir.gap_change =
              std::abs(ir.gap - old_gap) / std::max(1e-10, std::abs(old_gap));
        }
        gap_is_stationary =
            (results.size() >= window
             && ir.gap_change < m_options_.stationary_tol
             && ir.gap_change < 1.0);  // 1.0 = sentinel / first iteration
      }

      // ── Stationary gap convergence ──
      // Active for gap_stationary and statistical modes.
      // Declare convergence when the gap stops improving (non-zero gap
      // accepted).  In pure Benders (single scene / no apertures), this
      // is the main fallback since the CI test requires N > 1.
      if (!ir.converged && past_min_iterations && gap_is_stationary
          && mode != ConvergenceMode::gap_only)
      {
        ir.converged = true;
        ir.stationary_converged = true;
        m_converged_.store(true);
        SPDLOG_INFO(
            "{}: stationary gap convergence "
            "(gap_change={:.6f} < stationary_tol={:.6f}) [CONVERGED]",
            sddp_log("Iter", iteration_index),
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
          && ir.scene_upper_bounds.size() > 1 && past_min_iterations)
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
              "{}: statistical CI convergence "
              "(UB-LB={:.4f} <= z*σ={:.4f}, z={:.3f}, "
              "σ={:.4f}, N={}) [CONVERGED]",
              sddp_log("Iter", iteration_index),
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
              "{}: statistical + stationary convergence "
              "(UB-LB={:.4f} > z*σ={:.4f} but gap_change={:.6f} "
              "< stationary_tol={:.6f}) [CONVERGED]",
              sddp_log("Iter", iteration_index),
              gap_abs,
              ci_threshold,
              ir.gap_change,
              m_options_.stationary_tol);
        }
      }

      results.push_back(ir);

      SPDLOG_INFO(
          "{}: done in {:.3f}s (fwd {:.2f}s + bwd {:.2f}s) — "
          "UB={:.4f} LB={:.4f} gap={:.6f} gap_change={:.6f} "
          "cuts={} infeas_cuts={} {}",
          sddp_log("Iter", iteration_index),
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
        save_cuts_for_iteration(iteration_index, fwd->scene_feasible);
      }

      // ── Low-memory: release solver backends ──
      // Free solver memory after each iteration.  Backends are
      // reconstructed on demand in the next forward/backward pass.
      if (m_options_.low_memory_mode != LowMemoryMode::off) {
        const auto ns = planning_lp().simulation().scene_count();
        const auto np = planning_lp().simulation().phase_count();
        for (const auto si : iota_range<SceneIndex>(0, ns)) {
          for (const auto pi : iota_range<PhaseIndex>(0, np)) {
            planning_lp().system(si, pi).release_backend();
          }
        }
      }

      // ── Iteration callback ──
      if (m_iteration_callback_ && m_iteration_callback_(ir)) {
        SPDLOG_INFO("{}: callback requested stop",
                    sddp_log("Iter", iteration_index));
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
    const auto final_iteration_index = results.empty()
        ? m_iteration_offset_
        : next(results.back().iteration_index);
    const auto final_start = std::chrono::steady_clock::now();

    SPDLOG_INFO("{}: === simulation pass ===",
                sddp_log("Sim", final_iteration_index));

    // Suppress stop checks so the simulation pass always completes.
    // The stop was already honoured by exiting the iteration loop.
    m_in_simulation_ = true;

    SDDPIterationResult ir {
        .iteration_index = final_iteration_index,
    };
    m_benders_cut_.reset_infeasible_cut_count();

    auto fwd = run_forward_pass_all_scenes(
        *sddp_pool, fwd_opts, final_iteration_index);
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

    finalize_iteration_result(ir, final_iteration_index);

    // The simulation pass does not determine convergence on its own.
    // It inherits the convergence status from the last training iteration.
    // With max_iterations=0, no training ran, so converged stays false.
    ir.converged = !results.empty() && results.back().converged;

    results.push_back(ir);

    SPDLOG_INFO(
        "{}: done in {:.3f}s — "
        "UB={:.4f} LB={:.4f} gap={:.6f} gap_change={:.6f} {}",
        sddp_log("Sim", final_iteration_index),
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
    const auto num_scenes_final = planning_lp().simulation().scene_count();
    std::vector<uint8_t> final_feasible(
        static_cast<std::size_t>(num_scenes_final), 1U);
    save_cuts_for_iteration(results.back().iteration_index, final_feasible);

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

// ── Asynchronous scene execution ──────────────────────────────────────────

auto SDDPMethod::solve_async(SDDPWorkPool& pool,
                             const SolverOptions& fwd_opts,
                             const SolverOptions& bwd_opts)
    -> std::expected<std::vector<SDDPIterationResult>, Error>
{
  const auto num_scenes = planning_lp().simulation().scene_count();
  const auto num_phases = planning_lp().simulation().phase_count();
  const auto max_spread = m_options_.max_async_spread;
  const auto last_iteration_index =
      m_iteration_offset_ + IterationIndex {m_options_.max_iterations - 1};

  SPDLOG_INFO("SDDP async: {} scene(s), max_spread={}, iterations [{}, {}]",
              num_scenes,
              max_spread,
              m_iteration_offset_,
              last_iteration_index);

  // Monitoring setup (same as synchronous path)
  const auto solve_start = std::chrono::steady_clock::now();
  const std::string& status_file = m_options_.api_status_file;
  SolverMonitor monitor(m_options_.api_update_interval);
  if (m_options_.enable_api && !status_file.empty()) {
    monitor.start(pool, solve_start, "SDDPAsyncMonitor");
  }

  // Backward-pass options: enable warm-start for dual simplex
  auto bwd_ws_opts = bwd_opts;
  if (m_options_.warm_start) {
    bwd_ws_opts.reuse_basis = true;
  }

  // ── Per-scene state machine ──
  //
  // Each scene progresses independently through:
  //   idle → forward_running → backward_running → [check convergence]
  //     ↓ (converged or max_iterations)     ↓ (not converged)
  //     simulation_running                  idle (next iteration)
  //     ↓
  //     done (scene finished, outputs written)

  enum class SceneState : uint8_t
  {
    idle,
    forward_running,
    backward_running,
    simulation_running,
    done,
  };

  struct SceneProgress
  {
    SceneState state {SceneState::idle};
    IterationIndex current_iteration_index {};
    std::future<std::expected<double, Error>> fwd_future {};
    std::future<std::expected<int, Error>> bwd_future {};
    double upper_bound {};
    double lower_bound {};
    bool feasible {true};
    bool scene_converged {false};
  };

  std::vector<SceneProgress> progress(static_cast<std::size_t>(num_scenes));
  for (auto& sp : progress) {
    sp.current_iteration_index = m_iteration_offset_;
  }

  SceneIterationTracker tracker(num_scenes, max_spread);
  std::vector<SDDPIterationResult> results;
  results.reserve(static_cast<std::size_t>(m_options_.max_iterations) + 1);

  auto next_converge_iteration_index = m_iteration_offset_;
  const bool use_apertures =
      !m_options_.apertures || !m_options_.apertures->empty();

  // Probability mode for scene weights
  const auto prob_mode =
      planning_lp().planning().simulation.probability_rescale.value_or(
          ProbabilityRescaleMode::runtime);

  // ── Per-scene convergence helper ──
  // A scene is individually converged when its own gap < tol for enough iters.
  const auto check_scene_convergence = [&](SceneIndex scene_index,
                                           IterationIndex iteration_index,
                                           double ub,
                                           double lb) -> bool
  {
    if (!tracker.is_converged(scene_index)) {
      const double scene_gap = compute_convergence_gap(ub, lb);
      const bool past_min =
          (iteration_index >= m_iteration_offset_
               + IterationIndex {m_options_.min_iterations - 1});
      if (scene_gap < m_options_.convergence_tol && past_min) {
        tracker.mark_converged(scene_index, iteration_index);
        SPDLOG_INFO("{}: scene {} converged at iter {} (gap={:.6f})",
                    sddp_log("Async", iteration_index, scene_uid(scene_index)),
                    scene_uid(scene_index),
                    iteration_index,
                    scene_gap);
        return true;
      }
    }
    return tracker.is_converged(scene_index);
  };

  // ── Per-scene simulation + output writing ──
  // Runs a simulation forward pass for a single scene, then writes output.
  const auto run_scene_simulation =
      [&](SceneIndex scene_index,
          IterationIndex sim_iteration_index) -> std::expected<double, Error>
  {
    SPDLOG_INFO("{}: simulation pass for scene {}",
                sddp_log("Sim", sim_iteration_index, scene_uid(scene_index)),
                scene_uid(scene_index));
    auto result = forward_pass(scene_index, fwd_opts, sim_iteration_index);
    if (!result.has_value()) {
      return result;
    }

    // Write output for this scene's phases
    for (const auto phase_index : iota_range<PhaseIndex>(0, num_phases)) {
      planning_lp().system(scene_index, phase_index).write_out();
    }

    SPDLOG_INFO("{}: scene {} outputs written",
                sddp_log("Sim", sim_iteration_index, scene_uid(scene_index)),
                scene_uid(scene_index));
    return result;
  };

  // ── Main orchestration loop ──

  auto is_future_ready = [](const auto& fut)
  {
    return fut.valid()
        && fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
  };

  bool all_done = false;
  const auto cuts_before_simulation = m_cut_store_.stored_cuts().size();

  while (!all_done) {
    for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
      auto& sp = progress[static_cast<std::size_t>(scene)];

      switch (sp.state) {
        case SceneState::idle: {
          // Scene converged or hit max iterations → simulation pass
          const bool scene_finished =
              sp.current_iteration_index > last_iteration_index
              || sp.scene_converged;
          if (scene_finished) {
            // Submit simulation forward pass for this scene
            m_in_simulation_ = true;
            const auto sim_iteration_index = sp.current_iteration_index;
            const BasicTaskRequirements<SDDPTaskKey> sim_req {
                .priority = TaskPriority::High,
                .priority_key = make_sddp_task_key(sim_iteration_index,
                                                   SDDPPassDirection::forward,
                                                   first_phase_index(),
                                                   SDDPTaskKind::lp),
                .name = {},
            };
            auto fut = pool.submit(
                [&run_scene_simulation, scene, sim_iteration_index]
                { return run_scene_simulation(scene, sim_iteration_index); },
                sim_req);
            sp.fwd_future = std::move(fut.value());
            sp.state = SceneState::simulation_running;
            break;
          }

          // Spread limit: don't advance too far ahead of slowest
          // non-converged scene.  When max_spread >= max_iterations this
          // is effectively unlimited — scenes never wait.
          const auto min_iteration_index = tracker.min_completed_iteration();
          if (min_iteration_index >= m_iteration_offset_
              && sp.current_iteration_index
                  > min_iteration_index + IterationIndex {max_spread})
          {
            break;  // wait for slow scenes to catch up
          }
          // Check stop conditions
          if (should_stop()) {
            sp.state = SceneState::done;
            break;
          }

          // Submit forward pass
          m_benders_cut_.reset_infeasible_cut_count();
          const BasicTaskRequirements<SDDPTaskKey> fwd_req {
              .priority = TaskPriority::Medium,
              .priority_key = make_sddp_task_key(sp.current_iteration_index,
                                                 SDDPPassDirection::forward,
                                                 first_phase_index(),
                                                 SDDPTaskKind::lp),
              .name = {},
          };
          auto fut = pool.submit(
              [this,
               scene,
               iteration_index = sp.current_iteration_index,
               &fwd_opts]
              { return forward_pass(scene, fwd_opts, iteration_index); },
              fwd_req);
          sp.fwd_future = std::move(fut.value());
          sp.state = SceneState::forward_running;
          break;
        }

        case SceneState::forward_running: {
          if (!is_future_ready(sp.fwd_future)) {
            break;
          }
          auto fwd = sp.fwd_future.get();
          if (!fwd.has_value()) {
            SPDLOG_WARN(
                "{}: async forward failed: {}",
                sddp_log(
                    "Forward", sp.current_iteration_index, scene_uid(scene)),
                fwd.error().message);
            sp.feasible = false;
            sp.upper_bound = 0.0;
          } else {
            sp.upper_bound = *fwd;
            sp.feasible = true;
          }

          // Submit backward pass (only if feasible)
          if (sp.feasible) {
            const BasicTaskRequirements<SDDPTaskKey> bwd_req {
                .priority = TaskPriority::Medium,
                .priority_key = make_sddp_task_key(sp.current_iteration_index,
                                                   SDDPPassDirection::backward,
                                                   first_phase_index(),
                                                   SDDPTaskKind::lp),
                .name = {},
            };
            auto fut = use_apertures
                ? pool.submit(
                      [this,
                       scene,
                       &bwd_ws_opts,
                       iteration_index = sp.current_iteration_index]
                      {
                        return backward_pass_with_apertures(
                            scene, bwd_ws_opts, iteration_index);
                      },
                      bwd_req)
                : pool.submit(
                      [this,
                       scene,
                       &bwd_ws_opts,
                       iteration_index = sp.current_iteration_index]
                      {
                        return backward_pass(
                            scene, bwd_ws_opts, iteration_index);
                      },
                      bwd_req);
            sp.bwd_future = std::move(fut.value());
            sp.state = SceneState::backward_running;
          } else {
            // Infeasible: skip backward, record bounds, advance
            tracker.report_complete(scene,
                                    sp.current_iteration_index,
                                    0.0,
                                    0.0,
                                    /*feasible=*/false);
            sp.current_iteration_index = next(sp.current_iteration_index);
            sp.state = SceneState::idle;
          }
          break;
        }

        case SceneState::backward_running: {
          if (!is_future_ready(sp.bwd_future)) {
            break;
          }
          auto bwd = sp.bwd_future.get();
          if (!bwd.has_value()) {
            SPDLOG_WARN(
                "{}: async backward failed: {}",
                sddp_log(
                    "Backward", sp.current_iteration_index, scene_uid(scene)),
                bwd.error().message);
          }

          // Get lower bound from phase-0 objective
          sp.lower_bound = planning_lp()
                               .system(scene, first_phase_index())
                               .linear_interface()
                               .get_obj_value();

          tracker.report_complete(scene,
                                  sp.current_iteration_index,
                                  sp.upper_bound,
                                  sp.lower_bound,
                                  sp.feasible);

          {
            const double scene_gap =
                compute_convergence_gap(sp.upper_bound, sp.lower_bound);
            SPDLOG_INFO(
                "{}: completed (ub={:.4f} lb={:.4f} gap={:.6f})",
                sddp_log("Async", sp.current_iteration_index, scene_uid(scene)),
                sp.upper_bound,
                sp.lower_bound,
                scene_gap);
          }

          // Per-scene convergence check
          sp.scene_converged =
              check_scene_convergence(scene,
                                      sp.current_iteration_index,
                                      sp.upper_bound,
                                      sp.lower_bound);

          // Low-memory: release clones and backends for this scene
          if (m_options_.low_memory_mode != LowMemoryMode::off) {
            for (const auto pi : iota_range<PhaseIndex>(0, num_phases)) {
              planning_lp().system(scene, pi).release_backend();
            }
          }

          sp.current_iteration_index = next(sp.current_iteration_index);
          sp.state = SceneState::idle;
          break;
        }

        case SceneState::simulation_running: {
          if (!is_future_ready(sp.fwd_future)) {
            break;
          }
          auto sim = sp.fwd_future.get();
          if (!sim.has_value()) {
            SPDLOG_WARN(
                "{}: simulation failed: {}",
                sddp_log("Sim", sp.current_iteration_index, scene_uid(scene)),
                sim.error().message);
          } else {
            sp.upper_bound = *sim;
          }
          {
            int scenes_still_active = 0;
            for (const auto& sp2 : progress) {
              if (sp2.state != SceneState::done
                  && sp2.state != SceneState::simulation_running)
              {
                ++scenes_still_active;
              }
            }
            SPDLOG_INFO(
                "{}: scene done — converged={} iters={} sim_ub={:.4f} "
                "active={}/{}",
                sddp_log("Async", sp.current_iteration_index, scene_uid(scene)),
                sp.scene_converged,
                sp.current_iteration_index - m_iteration_offset_,
                sp.upper_bound,
                scenes_still_active,
                num_scenes);
          }
          sp.state = SceneState::done;
          break;
        }

        case SceneState::done:
          break;
      }
    }

    // ── Aggregate convergence: process all fully-completed iterations ──
    // This builds the results vector with per-iteration aggregate bounds,
    // used for logging and monitoring.  Individual scene convergence is
    // tracked separately above.
    while (tracker.all_complete(next_converge_iteration_index)) {
      const auto bounds =
          tracker.bounds_for_iteration(next_converge_iteration_index);

      // Build scene_feasible and weights for convergence computation
      std::vector<uint8_t> scene_feasible(static_cast<std::size_t>(num_scenes),
                                          0U);
      for (const auto& [si, b] : std::views::enumerate(bounds)) {
        scene_feasible[si] = b.feasible ? 1U : 0U;
      }

      const auto& scenes = planning_lp().simulation().scenes();
      const auto weights =
          compute_scene_weights(scenes, scene_feasible, prob_mode);

      SDDPIterationResult ir {
          .iteration_index = next_converge_iteration_index,
      };

      // Fill per-scene bounds
      ir.scene_upper_bounds.resize(static_cast<std::size_t>(num_scenes));
      ir.scene_lower_bounds.resize(static_cast<std::size_t>(num_scenes));
      double weighted_upper = 0.0;
      double weighted_lower = 0.0;
      for (const auto& [si, b] : std::views::enumerate(bounds)) {
        ir.scene_upper_bounds[si] = b.upper_bound;
        ir.scene_lower_bounds[si] = b.lower_bound;
        weighted_upper += weights[si] * b.upper_bound;
        weighted_lower += weights[si] * b.lower_bound;
      }
      ir.upper_bound = weighted_upper;
      ir.lower_bound = weighted_lower;

      // Async-specific: record per-scene iteration snapshot
      ir.scene_iterations = tracker.scene_iterations();

      // Convergence gap
      ir.gap = compute_convergence_gap(ir.upper_bound, ir.lower_bound);
      const bool past_min_iterations =
          (next_converge_iteration_index >= m_iteration_offset_
               + IterationIndex {m_options_.min_iterations - 1});
      ir.converged =
          (ir.gap < m_options_.convergence_tol) && past_min_iterations;

      // Stationary gap check (same logic as synchronous path)
      if (m_options_.stationary_tol > 0.0 && m_options_.stationary_window > 0
          && !results.empty())
      {
        const auto window =
            static_cast<std::size_t>(m_options_.stationary_window);
        const auto lookback = std::min(window, results.size());
        const double old_gap = results[results.size() - lookback].gap;
        ir.gap_change =
            std::abs(ir.gap - old_gap) / std::max(1e-10, std::abs(old_gap));

        if (!ir.converged && past_min_iterations && results.size() >= window
            && ir.gap_change < m_options_.stationary_tol && ir.gap_change < 1.0
            && m_options_.convergence_mode != ConvergenceMode::gap_only)
        {
          ir.converged = true;
          ir.stationary_converged = true;
        }
      }

      // Update live-query atomics
      m_current_iteration_.store(next_converge_iteration_index);
      m_current_gap_.store(ir.gap);
      m_current_lb_.store(ir.lower_bound);
      m_current_ub_.store(ir.upper_bound);
      if (ir.converged) {
        m_converged_.store(true);
      }

      // Log aggregate with pool stats
      {
        const auto pool_stats = pool.get_statistics();
        SPDLOG_INFO(
            "{}: async aggregate — "
            "UB={:.4f} LB={:.4f} gap={:.6f} gap_change={:.6f} "
            "spread=[{},{}] converged_scenes={}/{} "
            "pool(active={} pending={} cpu={:.0f}%) {}",
            sddp_log("Iter", next_converge_iteration_index),
            ir.upper_bound,
            ir.lower_bound,
            ir.gap,
            ir.gap_change,
            tracker.min_completed_iteration(),
            tracker.max_completed_iteration(),
            tracker.num_converged(),
            num_scenes,
            pool_stats.tasks_active,
            pool_stats.tasks_pending,
            pool_stats.current_cpu_load,
            ir.converged ? "[CONVERGED]" : "");
      }

      // Monitoring API: build async-aware snapshot
      if (m_options_.enable_api && !status_file.empty()) {
        const auto elapsed = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - solve_start)
                                 .count();
        const auto pool_stats = pool.get_statistics();

        // Build per-scene state labels
        std::vector<std::string> scene_state_labels;
        scene_state_labels.reserve(static_cast<std::size_t>(num_scenes));
        for (const auto& sp2 : progress) {
          switch (sp2.state) {
            case SceneState::done:
              scene_state_labels.emplace_back("done");
              break;
            case SceneState::simulation_running:
              scene_state_labels.emplace_back("simulation");
              break;
            default:
              scene_state_labels.emplace_back("training");
              break;
          }
        }

        std::string solver_id;
        if (!planning_lp().systems().empty()
            && !planning_lp().systems().front().empty())
        {
          solver_id = planning_lp()
                          .systems()
                          .front()
                          .front()
                          .linear_interface()
                          .solver_id();
        }

        const auto min_ci = tracker.min_completed_iteration();
        const auto max_ci = tracker.max_completed_iteration();
        const SolverStatusSnapshot snapshot {
            .iteration = m_current_iteration_.load(),
            .gap = m_current_gap_.load(),
            .lower_bound = m_current_lb_.load(),
            .upper_bound = m_current_ub_.load(),
            .converged = m_converged_.load(),
            .max_iterations = m_options_.max_iterations,
            .min_iterations = m_options_.min_iterations,
            .current_pass = m_current_pass_.load(),
            .scenes_done = m_scenes_done_.load(),
            .solver = std::move(solver_id),
            .method = "sddp",
            .phase_grid = &m_phase_grid_,
            .scene_iterations = tracker.scene_iterations(),
            .scene_states = std::move(scene_state_labels),
            .converged_scenes = tracker.num_converged(),
            .spread = (min_ci >= m_iteration_offset_) ? (max_ci - min_ci) : 0,
            .max_async_spread = max_spread,
            .pool_tasks_pending = static_cast<int>(pool_stats.tasks_pending),
            .pool_tasks_active = static_cast<int>(pool_stats.tasks_active),
            .pool_cpu_load = pool_stats.current_cpu_load,
            .pool_memory_percent = pool_stats.current_memory_percent,
            .pool_process_rss_mb = pool_stats.process_rss_mb,
            .pool_available_memory_mb = pool_stats.available_memory_mb,
            .lp_tasks_dispatched = pool_stats.lp_tasks_dispatched,
            .avg_lp_task_cpu_pct = pool_stats.avg_task_cpu_pct,
            .avg_lp_task_rss_delta_mb = pool_stats.avg_task_rss_delta_mb,
        };
        write_solver_status(status_file, results, elapsed, snapshot, monitor);
      }
      if (m_options_.save_per_iteration) {
        save_cuts_for_iteration(next_converge_iteration_index, scene_feasible);
      }

      results.push_back(std::move(ir));

      if (m_iteration_callback_ && m_iteration_callback_(results.back())) {
        SPDLOG_INFO("{}: callback requested stop",
                    sddp_log("Iter", next_converge_iteration_index));
        m_stop_requested_.store(true);
      }

      next_converge_iteration_index = next(next_converge_iteration_index);
    }

    // Check if all scenes are done
    all_done = true;
    for (const auto& sp : progress) {
      if (sp.state != SceneState::done) {
        all_done = false;
        break;
      }
    }

    // Avoid busy-spin: brief sleep when no futures are ready
    if (!all_done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  m_in_simulation_ = false;

  SPDLOG_INFO(
      "SDDP async: complete — {} aggregate results, "
      "converged_scenes={}/{}, spread=[{},{}]",
      results.size(),
      tracker.num_converged(),
      num_scenes,
      tracker.min_completed_iteration(),
      tracker.max_completed_iteration());

  // Discard simulation feasibility cuts (same as synchronous path)
  if (!m_options_.save_simulation_cuts
      && m_cut_store_.stored_cuts().size() > cuts_before_simulation)
  {
    SPDLOG_INFO("SDDP: discarding {} simulation feasibility cut(s)",
                m_cut_store_.stored_cuts().size() - cuts_before_simulation);
    m_cut_store_.stored_cuts().resize(cuts_before_simulation);
  }

  // ── Cut persistence ──
  if (!m_options_.cuts_output_file.empty() && !results.empty()) {
    const auto num_scenes_final = planning_lp().simulation().scene_count();
    std::vector<uint8_t> final_feasible(
        static_cast<std::size_t>(num_scenes_final), 1U);
    save_cuts_for_iteration(results.back().iteration_index, final_feasible);

    const auto mode = m_options_.cut_recovery_mode;
    if (mode == HotStartMode::append) {
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
