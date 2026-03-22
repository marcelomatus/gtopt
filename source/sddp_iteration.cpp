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
#include <gtopt/sddp_monitor.hpp>
#include <gtopt/sddp_pool.hpp>
#include <gtopt/sddp_solver.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

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

  // Monitoring setup
  const auto solve_start = std::chrono::steady_clock::now();
  const std::string& status_file = m_options_.api_status_file;
  SolverMonitor monitor(m_options_.api_update_interval);
  if (m_options_.enable_api && !status_file.empty()) {
    monitor.start(*sddp_pool, solve_start, "SDDPMonitor");
  }

  std::vector<SDDPIterationResult> results;
  results.reserve(m_options_.max_iterations);

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
    // Save per-scene cut counts for cut sharing offset tracking
    const auto cuts_before = m_stored_cuts_.size();
    const auto num_scenes_bwd =
        static_cast<Index>(planning_lp().simulation().scenes().size());
    m_scene_cuts_before_.resize(static_cast<std::size_t>(num_scenes_bwd));
    for (Index si = 0; si < num_scenes_bwd; ++si) {
      m_scene_cuts_before_[static_cast<std::size_t>(si)] =
          m_scene_cuts_[SceneIndex {si}].size();
    }
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
    if (m_options_.cut_sharing == CutSharingMode::none) {
      apply_cut_sharing_for_iteration(cuts_before, iter);
    }

    // ── Cut pruning ──
    if (m_options_.max_cuts_per_phase > 0 && m_options_.cut_prune_interval > 0)
    {
      const auto iter_offset = static_cast<int>(iter - m_iteration_offset_);
      if (iter_offset > 0 && iter_offset % m_options_.cut_prune_interval == 0) {
        prune_inactive_cuts();
      }
    }

    // ── Cap stored cuts ──
    cap_stored_cuts();

    // ── Convergence + live-query update ──
    finalize_iteration_result(ir, iter);
    results.push_back(ir);

    SPDLOG_INFO(
        "SDDP: iter {} done in {:.3f}s (fwd {:.2f}s + bwd {:.2f}s) — "
        "UB={:.4f} LB={:.4f} gap={:.6f} cuts={} infeas_cuts={} {}",
        iter,
        ir.iteration_s,
        ir.forward_pass_s,
        ir.backward_pass_s,
        ir.upper_bound,
        ir.lower_bound,
        ir.gap,
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

  // Final cut save — always save at the end of the solve, regardless of
  // save_per_iteration.  This ensures cuts are persisted on convergence,
  // user-stop, or max-iterations even when per-iteration saving is disabled.
  if (!m_options_.cuts_output_file.empty() && !results.empty()) {
    const auto num_scenes_final =
        static_cast<Index>(planning_lp().simulation().scenes().size());
    std::vector<uint8_t> final_feasible(
        static_cast<std::size_t>(num_scenes_final), 1U);
    save_cuts_for_iteration(results.back().iteration, final_feasible);

    // Write the combined output file based on hot_start_mode:
    //  - none/replace: write all cuts to the combined file
    //  - keep:         do not modify the combined file
    //  - append:       append new cuts to the existing file
    const auto mode = m_options_.hot_start_mode;
    if (mode == HotStartMode::append) {
      // Append mode: add newly generated cuts to the existing file.
      const auto& cuts_to_append = m_options_.single_cut_storage
          ? build_combined_cuts()
          : m_stored_cuts_;
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
      SPDLOG_INFO("SDDP: hot_start_mode=keep — combined file not modified");
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
