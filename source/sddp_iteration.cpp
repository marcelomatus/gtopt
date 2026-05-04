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

#include <array>
#include <chrono>
#include <limits>
#include <thread>
#include <vector>

#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_common.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_pool.hpp>
#include <gtopt/solver_stats.hpp>
#include <gtopt/solver_status.hpp>
#include <gtopt/utils.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// Sum `SolverStats` across every `(scene, phase)` LP in the planning
/// grid.  Used to take a pre/post snapshot around the backward pass so
/// the per-iteration delta of the `bwd_*_s` timers can be logged.
[[nodiscard]] SolverStats aggregate_backward_stats(const PlanningLP& planning)
{
  SolverStats total {};
  const auto num_scenes = planning.simulation().scene_count();
  const auto num_phases = planning.simulation().phase_count();
  for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
    for (const auto pi : iota_range<PhaseIndex>(0, num_phases)) {
      total += planning.system(si, pi).linear_interface().solver_stats();
    }
  }
  return total;
}

/// Emit a single INFO line summarising the six backward-step timers for
/// an iteration.  Shown next to the standard "SDDP Iter [iN]: done ..."
/// so the per-iteration growth of each stage stays visible alongside the
/// total backward-pass wall time.
void log_backward_timing_breakdown(IterationIndex iteration_index,
                                   const SolverStats& delta)
{
  if (delta.bwd_step_count == 0) {
    return;
  }
  const auto n = static_cast<double>(delta.bwd_step_count);
  const auto to_ms = [n](double s_sum) noexcept { return 1000.0 * s_sum / n; };
  SPDLOG_INFO(
      "SDDP Backward [i{}]: step-avg(ms) rebuild={:.2f} build={:.2f} "
      "add_row={:.2f} store={:.2f} resolve={:.2f} kappa={:.2f} — "
      "{} step(s), total(s) rebuild={:.2f} build={:.2f} add_row={:.2f} "
      "store={:.2f} resolve={:.2f} kappa={:.2f}",
      gtopt::uid_of(iteration_index),
      to_ms(delta.bwd_lp_rebuild_s),
      to_ms(delta.bwd_cut_build_s),
      to_ms(delta.bwd_add_row_s),
      to_ms(delta.bwd_store_cut_s),
      to_ms(delta.bwd_resolve_s),
      to_ms(delta.bwd_kappa_s),
      delta.bwd_step_count,
      delta.bwd_lp_rebuild_s,
      delta.bwd_cut_build_s,
      delta.bwd_add_row_s,
      delta.bwd_store_cut_s,
      delta.bwd_resolve_s,
      delta.bwd_kappa_s);
}

/// Slack added to each ``alpha`` bucket boundary in
/// ``kZScoreTable`` so that user-supplied round confidence levels —
/// which round-trip through ``1.0 - p`` to e.g.
/// ``alpha = 0.0100000000000000009`` instead of ``0.01`` exactly —
/// land in the bucket the user actually meant.  Without this slack
/// the JSON setting ``convergence_confidence: 0.99`` silently
/// selected ``z = 1.960`` instead of ``2.576``.  ``1e-6`` is several
/// orders of magnitude wider than typical IEEE-754 round-off (~1e-17)
/// and well below any meaningful confidence-level resolution.
inline constexpr double kAlphaBucketSlack = 1e-6;

/// One row of the two-sided z-score lookup table used by the
/// statistical CI convergence test.  ``alpha_max`` is the upper bound
/// (inclusive, plus ``kAlphaBucketSlack``) on
/// ``alpha = 1 - convergence_confidence`` for which ``z`` applies.
struct ZScoreBucket
{
  double alpha_max;  ///< Maximum alpha that selects this bucket.
  double z;  ///< Two-sided ``z_{α/2}`` for the bucket.
};

/// Two-sided z-scores for common confidence levels.  Buckets are
/// scanned in order, so the first row whose ``alpha_max`` envelopes
/// the supplied alpha wins.  The trailing ``infinity`` row is the
/// 80 % fallback that catches every alpha not matched above.
inline constexpr std::array<ZScoreBucket, 4> kZScoreTable {
    ZScoreBucket {.alpha_max = 0.01 + kAlphaBucketSlack, .z = 2.576},  // 99 %
    ZScoreBucket {.alpha_max = 0.05 + kAlphaBucketSlack, .z = 1.960},  // 95 %
    ZScoreBucket {.alpha_max = 0.10 + kAlphaBucketSlack, .z = 1.645},  // 90 %
    ZScoreBucket {.alpha_max = std::numeric_limits<double>::infinity(),
                  .z = 1.282},  // 80 % fallback
};

/// Pick ``z_{α/2}`` for the supplied ``alpha`` from
/// ``kZScoreTable``.  Stepwise resolution (99 / 95 / 90 / 80) — fine
/// for SDDP convergence which only needs the bucketed values from the
/// stats literature, not a continuous inverse-normal.
[[nodiscard]] constexpr auto z_score_for_alpha(double alpha) noexcept -> double
{
  for (const auto& b : kZScoreTable) {
    if (alpha <= b.alpha_max) {
      return b.z;
    }
  }
  return kZScoreTable.back().z;  // unreachable: ∞ row matches everything.
}

}  // namespace

auto SDDPMethod::solve(const SolverOptions& lp_opts)
    -> std::expected<std::vector<SDDPIterationResult>, Error>
{
  // Validate preconditions
  if (auto err = validate_inputs()) {
    return std::unexpected(std::move(*err));
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
  // Cell-task headroom = num_scenes: the synchronised backward pass
  // dispatches one cell task per feasible scene; each blocks on its
  // aperture futures while holding a worker slot.  Adding `num_scenes`
  // headroom keeps `cpu_factor × physical_concurrency` slots free for
  // aperture solves regardless of how many cell tasks are mid-wait.
  // See `sddp_pool.hpp::make_sddp_work_pool` for the rationale.
  const auto cell_task_headroom =
      static_cast<int>(planning_lp().simulation().scene_count());
  auto sddp_pool = make_sddp_work_pool(m_options_.pool_cpu_factor,
                                       m_options_.pool_memory_limit_mb,
                                       cell_task_headroom);
  const bool need_aux_pool =
      (!m_options_.apertures || !m_options_.apertures->empty())
      || !m_options_.log_directory.empty();
  auto aux_pool = need_aux_pool
      ? make_solver_work_pool(
            /*cpu_factor=*/2.0,
            /*cpu_threshold_override=*/0.0,
            /*scheduler_interval=*/std::chrono::milliseconds(50),
            /*memory_limit_mb=*/m_options_.pool_memory_limit_mb)
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
        SPDLOG_INFO(
            "SDDP Iter [i{}]: stop requested, halting after {}"
            " iterations",
            gtopt::uid_of(iteration_index),
            iteration_relative(iteration_index, m_iteration_offset_));
        break;
      }

      // 1-based "N of M" reads naturally; the raw indices were
      // confusing (e.g. "=== 0/0 ===" for max_iterations=1).
      SPDLOG_INFO("SDDP Iter [i{}]: === starting ({} of {}) ===",
                  gtopt::uid_of(iteration_index),
                  iteration_relative(iteration_index, m_iteration_offset_) + 1,
                  m_options_.max_iterations);

      SDDPIterationResult ir {
          .iteration_index = gtopt::uid_of(iteration_index),
      };
      m_benders_cut_.reset_infeasible_cut_count();

      // ── Forward pass ──
      SPDLOG_DEBUG("SDDP Forward [i{}]: starting forward pass",
                   iteration_index);
      auto fwd =
          run_forward_pass_all_scenes(*sddp_pool, fwd_opts, iteration_index);
      if (!fwd.has_value()) {
        monitor.stop();
        return std::unexpected(std::move(fwd.error()));
      }
      ir.scene_upper_bounds = std::move(fwd->scene_upper_bounds);
      // Copy (not move) — many downstream reads of ``fwd->scene_feasible``
      // remain in this function (cut storage, save_cuts_for_iteration,
      // backward dispatch).  A 1-byte-per-scene vector is cheap to copy.
      ir.scene_feasible = fwd->scene_feasible;
      ir.forward_pass_s = fwd->elapsed_s;
      // No additional info-level "forward pass has feasibility issues"
      // log: when `forward_infeas_rollback` is on, the
      // `SDDP Forward [iN]: rolled back M cut row(s) across K
      // infeasible scene(s) [s1=8 s3=8 …]` line emitted by
      // `run_forward_pass_all_scenes` already conveys exactly the same
      // signal plus the per-scene breakdown — re-stating "has issues"
      // 2 ms later was pure noise.
      ir.feasibility_issue = fwd->has_feasibility_issue;
      SPDLOG_DEBUG("SDDP Forward [i{}]: done in {:.3f}s",
                   gtopt::uid_of(iteration_index),
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
      SPDLOG_DEBUG("SDDP Backward [i{}]: starting backward pass",
                   iteration_index);
      // Save per-scene cut counts for cut sharing offset tracking.
      // Stored on each `SceneCutStore` (per-scene snapshot) post-step-4
      // of `support/sddp_cut_store_split_plan_2026-04-30.md`; the
      // legacy parallel `m_scene_cuts_before_` vector is gone.
      const auto num_scenes_bwd = planning_lp().simulation().scene_count();
      for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes_bwd)) {
        auto& sc = m_cut_store_.at(scene_index);
        sc.set_cuts_before(sc.size());
      }
      // Snapshot aggregate solver stats so the post-pass diff reveals
      // the six per-step backward-cut timers (rebuild/build/add_row/
      // store/resolve/kappa).  The cost of the aggregation is a loop
      // over (scene × phase) LPs — trivial compared to the backward
      // pass itself.
      const auto bwd_stats_before = aggregate_backward_stats(planning_lp());

      auto bwd = run_backward_pass_all_scenes(
          fwd->scene_feasible, *sddp_pool, bwd_opts, iteration_index);
      ir.cuts_added = bwd.total_cuts;
      ir.infeasible_cuts_added = m_benders_cut_.infeasible_cut_count();
      ir.backward_pass_s = bwd.elapsed_s;
      if (bwd.has_feasibility_issue) {
        ir.feasibility_issue = true;
        // "LP-level infeasibility" — aperture solves failed at
        // some (scene, phase) cells and the fallback bcut was
        // used.  NOT to be confused with feasibility cuts: the
        // backward pass never installs any (all cuts it produces
        // are optimality cuts).  Feasibility cuts are emitted
        // only by the forward pass's elastic branch.
        SPDLOG_INFO(
            "SDDP Backward [i{}]: backward pass had LP-level "
            "infeasibility (aperture fallbacks used)",
            iteration_index);
      }
      ir.iteration_s =
          std::chrono::duration<double>(std::chrono::steady_clock::now()
                                        - iteration_start_time)
              .count();
      SPDLOG_DEBUG("SDDP Backward [i{}]: done in {:.3f}s, {} cuts added",
                   gtopt::uid_of(iteration_index),
                   bwd.elapsed_s,
                   bwd.total_cuts);

      // Diff the post-pass aggregate against the snapshot so each log
      // line shows just this iteration's breakdown.  Useful for
      // tracking how stages scale with the number of cuts installed so
      // far — the suspected cause of the "later iterations are 10x
      // slower than early ones" pattern.
      {
        auto bwd_delta = aggregate_backward_stats(planning_lp());
        bwd_delta -= bwd_stats_before;
        log_backward_timing_breakdown(iteration_index, bwd_delta);
      }

      // ── Cut sharing ──
      // Cut sharing across scenes happens inside the synchronized
      // backward pass at `sddp_method.cpp::share_cuts_for_phase`
      // (per-phase barrier, `sddp_method.cpp:1775`).  The previous
      // post-pass aggregation call `apply_cut_sharing_for_iteration`
      // had an inverted guard that made it a no-op in every mode
      // (none → inner early-return; !none → outer guard skipped it),
      // so it carried no behaviour and was removed.

      // ── Cut pruning ──
      if (m_options_.max_cuts_per_phase > 0
          && m_options_.cut_prune_interval > 0)
      {
        const auto iteration_offset_diff =
            iteration_relative(iteration_index, m_iteration_offset_);
        if (iteration_offset_diff > 0
            && iteration_offset_diff % m_options_.cut_prune_interval == 0)
        {
          prune_inactive_cuts();
        }
      }

      // ── Cap stored cuts ──
      cap_stored_cuts();

      // ── Convergence + live-query update ──
      // finalize_iteration_result() computes ir.gap AND ir.gap_change
      // (from the look-back window over `results`) before logging, so the
      // mid-iteration "SDDP iter N: ..." log shows the same gap_change
      // the final "Iter [iN]: done ..." log carries downstream.
      finalize_iteration_result(ir, iteration_index, results);

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

      // The stationary convergence test requires the full window.  The
      // gap_change value itself is populated by finalize_iteration_result()
      // above; here we only decide whether the window is stationary enough
      // to trigger convergence.
      bool gap_is_stationary = false;
      if (mode != ConvergenceMode::gap_only && m_options_.stationary_tol > 0.0
          && m_options_.stationary_window > 0)
      {
        const auto window =
            static_cast<std::size_t>(m_options_.stationary_window);
        gap_is_stationary =
            (results.size() >= window
             && ir.gap_change < m_options_.stationary_tol
             && ir.gap_change < 1.0);  // 1.0 = sentinel / first iteration
      }

      // ── Stationary gap convergence ──
      // Active for gap_stationary and statistical modes.
      // Declare convergence when the gap stops improving.  Guarded by
      // an absolute-gap ceiling (configurable via
      // ``SDDPOptions::stationary_gap_ceiling``, default 0.5) so a
      // pathological case where LB is frozen (e.g. pre-ef25515b
      // Juan/gtopt_iplp: LB=0, UB>0, gap=1 flat) cannot masquerade
      // as "converged" just because gap_change = 0.  Set the option
      // to 1.0 to effectively disable the ceiling — appropriate for
      // runs where the policy legitimately asymptotes at a high gap
      // because the cheapest feasible plan pays a near-fixed slack.
      const double stationary_gap_ceiling = m_options_.stationary_gap_ceiling;
      // Same negative-gap guard as the primary gap test in
      // `finalize_iteration_result` — refuse to call a stationary
      // run "converged" when LB > UB by more than tolerance (cuts
      // overshooting the optimum, SDDP-theory violation).  Tiny
      // negative gaps (≈ -1e-15 from FP noise when UB ≈ LB) are
      // fine — `kSddpGapFpEpsilon` (sddp_types.hpp) is the shared
      // noise-band constant.
      if (!ir.converged && past_min_iterations && gap_is_stationary
          && ir.gap > -kSddpGapFpEpsilon && ir.gap < stationary_gap_ceiling
          && mode != ConvergenceMode::gap_only)
      {
        ir.converged = true;
        ir.stationary_converged = true;
        update_live_metrics_([](LiveMetrics& m) { m.converged = true; });
        SPDLOG_INFO(
            "SDDP Iter [i{}]: stationary gap convergence "
            "(gap_change={:.6f} < stationary_tol={:.6f}) [CONVERGED]",
            gtopt::uid_of(iteration_index),
            ir.gap_change,
            m_options_.stationary_tol);
      } else if (!ir.converged && past_min_iterations && gap_is_stationary
                 && mode != ConvergenceMode::gap_only)
      {
        // Tripped stationary but gap is still large — emit a loud
        // diagnostic so this pathology is visible rather than silently
        // swallowed.  The solver continues until max_iterations.
        SPDLOG_WARN(
            "SDDP Iter [i{}]: stationary gap_change={:.6f} < tol={:.6f} "
            "but gap={:.4f} >= ceiling={:.2f} — NOT converging "
            "(LB likely frozen; continuing)",
            gtopt::uid_of(iteration_index),
            ir.gap_change,
            m_options_.stationary_tol,
            ir.gap,
            stationary_gap_ceiling);
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
      //  (a) CI test:  UB - LB <= z_{α/2} · σ̂ / √N_feasible
      //  (b) CI + stationarity:  gap > z·σ̂/√N but gap_change < stationary_tol
      //
      // **Layer 4 reformulation (2026-05-02)** — the original code
      // had three statistical bugs that compounded to make the CI
      // test fire on cases that hadn't actually converged:
      //
      //  1. Used the *uniform* sample mean and variance over all
      //     ``scene_upper_bounds`` entries.  For heterogeneous-prob
      //     runs (juan: 16 hydrology scenarios with non-uniform
      //     ``probability_factor``) this gives the wrong σ̂ — the
      //     correct estimator weights each scene by ``p_s``.
      //  2. Included infeasible scenes (whose UB stayed at the
      //     initialised 0.0) in both mean and variance.  This pulled
      //     μ̂ low and inflated σ̂ artificially — exactly the wrong
      //     direction (made the CI threshold ``z·σ̂`` *wider* than
      //     it should be → easier-to-fire false convergence).
      //  3. Threshold was ``z·σ̂`` instead of the textbook
      //     ``z·σ̂/√N`` (standard error of the mean, e.g.
      //     Birge & Louveaux §10.3).  At N=16 that's a 4× wider
      //     threshold than canonical.
      //
      // Combined effect on juan/gtopt_iplp trace_28: declared
      // statistical CI convergence at iter 2 with gap = 25 % because
      // the inflated σ̂ ≈ 77 M and missing /√N let z·σ̂ ≈ 152 M
      // dominate the actual 38 M absolute gap.  After this fix,
      // probability-weighted feasible-only mean / variance computed
      // over 7 of 16 scenes with proper /√N gives a much tighter
      // threshold; the CI test fires only when the gap is genuinely
      // within the standard error of the mean.
      if (!ir.converged && mode == ConvergenceMode::statistical
          && m_options_.convergence_confidence > 0.0
          && ir.scene_upper_bounds.size() > 1 && past_min_iterations)
      {
        // Probability-weighted feasible-only mean and variance.  Sum
        // of probabilities is renormalised to the feasible subset so
        // the weights inside the loop sum to 1.0 — equivalent to
        // computing E[UB | feasible] under the conditional measure.
        // (The Layer 2 ``infeasible_scene_penalty`` path lives in
        // ``compute_iteration_bounds`` and adjusts UB / LB; here we
        // are estimating the variance of the cost-distribution sample,
        // which is unaffected by whether the user opts into a penalty
        // mode for the bound aggregation.)
        const auto& scenes = planning_lp().simulation().scenes();
        double total_p_feasible = 0.0;
        std::size_t n_feasible = 0;
        for (const auto si :
             iota_range<SceneIndex>(0, ir.scene_upper_bounds.size()))
        {
          if (!ir.scene_feasible.empty() && ir.scene_feasible[si] == 0U) {
            continue;
          }
          total_p_feasible += scenes[si].probability_factor();
          ++n_feasible;
        }

        // Need ≥ 2 feasible scenes for an unbiased variance estimator.
        // When N_feasible ≤ 1 we can't compute a meaningful CI; skip
        // the block and fall through to the legacy primary / stationary
        // criteria.
        if (n_feasible < 2 || total_p_feasible <= 0.0) {
          // (the post-CI fallthrough still runs the (b) stationary
          // branch via gap_is_stationary)
          ;  // explicit empty branch — reads cleaner than nesting
        }

        double mean = 0.0;
        if (n_feasible >= 2 && total_p_feasible > 0.0) {
          for (const auto si :
               iota_range<SceneIndex>(0, ir.scene_upper_bounds.size()))
          {
            if (!ir.scene_feasible.empty() && ir.scene_feasible[si] == 0U) {
              continue;
            }
            const double w = scenes[si].probability_factor() / total_p_feasible;
            mean += w * ir.scene_upper_bounds[si];
          }
        }
        double var = 0.0;
        if (n_feasible >= 2 && total_p_feasible > 0.0) {
          for (const auto si :
               iota_range<SceneIndex>(0, ir.scene_upper_bounds.size()))
          {
            if (!ir.scene_feasible.empty() && ir.scene_feasible[si] == 0U) {
              continue;
            }
            const double w = scenes[si].probability_factor() / total_p_feasible;
            const auto d = ir.scene_upper_bounds[si] - mean;
            var += w * d * d;
          }
        }
        const auto sigma = std::sqrt(var);

        const auto alpha = 1.0 - m_options_.convergence_confidence;
        // `z_score_for_alpha` (anonymous namespace above) bucketises
        // alpha against `kZScoreTable` and applies `kAlphaBucketSlack`
        // so user-supplied round confidence levels (0.99, 0.95, 0.90)
        // land in the right bucket despite `1.0 - p` IEEE-754
        // round-off.  Without the slack the JSON setting
        // `convergence_confidence: 0.99` silently selected z = 1.960
        // instead of 2.576 (juan run 2026-05-02 trace_28).
        const auto z_score = z_score_for_alpha(alpha);

        const auto gap_abs = ir.upper_bound - ir.lower_bound;
        // Standard error of the mean: σ̂ / √N_feasible (textbook
        // CI of a sample mean).  Skip this branch entirely if
        // N_feasible < 2 — the variance estimator is undefined and
        // the CI test cannot fire.
        const auto ci_threshold = (n_feasible >= 2)
            ? z_score * sigma / std::sqrt(static_cast<double>(n_feasible))
            : std::numeric_limits<double>::infinity();

        // (a) Gap within CI: LB inside the UB confidence interval.
        // Negative-gap guard mirrors the primary gap test — tiny
        // negative is FP noise (accept), large negative violates
        // SDDP theory (reject).  Uses the shared
        // `kSddpGapFpEpsilon` (sddp_types.hpp) since
        // `convergence_tol` may be a negative sentinel here.
        //
        // Absolute-gap ceiling matches the pure-stationary block above:
        // when scene UBs are wildly heterogeneous (different cuts →
        // different optima), σ inflates and z·σ becomes so wide that
        // *any* sub-50%-of-mean gap qualifies as "noise."  The ceiling
        // refuses to interpret that as convergence — same defensive
        // posture as ``stationary_gap_ceiling`` (default 0.5).  Tighten
        // the option (e.g. 0.05) for runs where σ explosion is expected.
        if (gap_abs > -kSddpGapFpEpsilon && gap_abs <= ci_threshold
            && ir.gap < stationary_gap_ceiling)
        {
          ir.converged = true;
          ir.statistical_converged = true;
          update_live_metrics_([](LiveMetrics& m) { m.converged = true; });
          // Log shows N_feasible (not raw scene count) so an operator
          // can match it against the ``feasible=K/N`` clause in the
          // headline.  σ̂ is the probability-weighted feasible-only
          // sample sd; threshold is z·σ̂/√N_feasible (textbook SE of
          // the mean).
          SPDLOG_INFO(
              "SDDP Iter [i{}]: statistical CI convergence "
              "(UB-LB={:.4f} <= z·σ̂/√N={:.4f}, z={:.3f}, "
              "σ̂={:.4f}, N_feasible={}) [CONVERGED]",
              gtopt::uid_of(iteration_index),
              gap_abs,
              ci_threshold,
              z_score,
              sigma,
              n_feasible);
        }
        // (b) Gap exceeds CI but is no longer improving.
        // Same absolute-gap ceiling as the pure-stationary block above:
        // refuse to convert a frozen-LB pathology into a convergence
        // signal just because gap_change is zero.  Negative-gap guard
        // mirrors the primary test — tiny negative gap is FP noise
        // (accept), large negative violates SDDP theory (reject).
        else if (gap_is_stationary && ir.gap > -kSddpGapFpEpsilon
                 && ir.gap < stationary_gap_ceiling)
        {
          ir.converged = true;
          ir.statistical_converged = true;
          ir.stationary_converged = true;
          update_live_metrics_([](LiveMetrics& m) { m.converged = true; });
          SPDLOG_INFO(
              "SDDP Iter [i{}]: statistical + stationary convergence "
              "(UB-LB={:.4f} > z*σ={:.4f} but gap_change={:.6f} "
              "< stationary_tol={:.6f}) [CONVERGED]",
              gtopt::uid_of(iteration_index),
              gap_abs,
              ci_threshold,
              ir.gap_change,
              m_options_.stationary_tol);
        }
      }

      results.push_back(ir);

      // Bounds rendered with the SI helper (`format_si`, source-local in
      // `sddp_forward_pass.cpp`) and gap as a percentage so operators
      // can read the convergence story at a glance.  `cuts/infeas` are
      // dropped from the headline when both are zero — they're noise on
      // the all-feasible iters.
      //
      // ``feasible=K/N`` exposes the fraction of the original N-scene
      // problem the bounds actually cover.  UB and LB above are computed
      // only over feasible scenes (probability weights renormalised to
      // sum 1.0), so a low K/N means the printed gap is for a
      // sub-problem and converging it does NOT certify a solution to
      // the original N-scene SDDP — operators must see this directly,
      // not have it hidden by silent renormalisation.  The clause is
      // emitted only when K < N (the all-feasible case stays terse).
      const auto n_total = ir.scene_feasible.size();
      const auto n_feasible = static_cast<std::size_t>(
          std::ranges::count(ir.scene_feasible, uint8_t {1}));
      const auto feasibility_clause = (n_total > 0 && n_feasible < n_total)
          ? std::format(" feasible={}/{}", n_feasible, n_total)
          : std::string {};
      SPDLOG_INFO(
          "SDDP Iter [i{}]: done in {:.3f}s (fwd {:.2f}s + bwd {:.2f}s) — "
          "UB={} LB={} gap={:.2f}% Δgap={:.2f}% cuts={}/{}{}{}",
          gtopt::uid_of(iteration_index),
          ir.iteration_s,
          ir.forward_pass_s,
          ir.backward_pass_s,
          format_si(ir.upper_bound),
          format_si(ir.lower_bound),
          100.0 * ir.gap,
          100.0 * ir.gap_change,
          ir.cuts_added,
          ir.infeasible_cuts_added,
          feasibility_clause,
          ir.converged ? " [CONVERGED]" : "");

      // ── Post-iteration housekeeping (timed) ──
      // Single INFO line at the end summarises the breakdown so the
      // inter-iteration gap visible in operator logs (e.g. juan/
      // gtopt_iplp's ~10 s gap) can be attributed to its components
      // — api status write, cut persistence, idempotent backend
      // release safety net, and the user-supplied iteration
      // callback — without rebuilding with TRACE logs on.
      using PostClock = std::chrono::steady_clock;
      const auto post_iter_t0 = PostClock::now();
      const auto post_elapsed_s = [](PostClock::time_point start) noexcept
      {
        return std::chrono::duration<double>(PostClock::now() - start).count();
      };

      const auto t_api = PostClock::now();
      maybe_write_api_status(status_file, results, solve_start, monitor);
      const auto dt_api = post_elapsed_s(t_api);

      double dt_save = 0.0;
      if (m_options_.save_per_iteration) {
        const auto t_save = PostClock::now();
        save_cuts_for_iteration(iteration_index, fwd->scene_feasible);
        dt_save = post_elapsed_s(t_save);
      }

      // ── Low-memory: release solver backends ──
      // After the per-cell release scheme most cells are already
      // released by the backward worker; this bulk loop is the
      // idempotent safety net.  `release_backend` is a no-op when
      // `m_backend_released_` is already set (`linear_interface.cpp:144`),
      // so the cost is dominated by the loop overhead (~µs total).
      //
      // Previously this loop also called `drop_cached_primal_only()`
      // to free the per-cell col_sol/col_cost snapshots, but that
      // broke `physical_eini` under compress: the next iteration's
      // forward `update_lp_for_phase` reads `get_col_sol()` to evaluate
      // turbine production factor / seepage / discharge limit at the
      // previous solve's reservoir volume, and dropping the cache
      // forced the read to fall through to the freshly reconstructed
      // backend whose `col_solution()` is zero/undefined → coefficients
      // pinned at construction-time default_volume → SDDP plateau at
      // gap=133.78% on juan/gtopt_iplp.  The retained col_sol vector
      // costs ~50 KB × num_cells (≈ 40 MB on juan, 2.5 GB on the 816-
      // cell support cases).  ensure_backend now restores cached
      // primal/dual onto the live backend so the read path stays
      // valid across release_backend → ensure_backend cycles.
      const auto t_release = PostClock::now();
      if (m_options_.low_memory_mode != LowMemoryMode::off) {
        const auto ns = planning_lp().simulation().scene_count();
        const auto np = planning_lp().simulation().phase_count();
        for (const auto si : iota_range<SceneIndex>(0, ns)) {
          for (const auto pi : iota_range<PhaseIndex>(0, np)) {
            auto& sys = planning_lp().system(si, pi);
            sys.release_backend();
          }
        }
      }
      const auto dt_release = post_elapsed_s(t_release);

      // ── Iteration callback ──
      const auto t_callback = PostClock::now();
      const bool callback_requested_stop =
          m_iteration_callback_ && m_iteration_callback_(ir);
      const auto dt_callback = post_elapsed_s(t_callback);

      const auto dt_post_iter = post_elapsed_s(post_iter_t0);
      SPDLOG_INFO(
          "SDDP Iter [i{}]: post-iter {:.2f}s — api={:.3f}s save={:.2f}s "
          "release={:.3f}s callback={:.3f}s",
          gtopt::uid_of(iteration_index),
          dt_post_iter,
          dt_api,
          dt_save,
          dt_release,
          dt_callback);

      if (callback_requested_stop) {
        SPDLOG_INFO("SDDP Iter [i{}]: callback requested stop",
                    iteration_index);
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
  // Snapshot per-scene cut counts so sim-pass-added feasibility cuts
  // can be discarded below (unless `save_simulation_cuts` is set).
  std::vector<std::size_t> sim_before_scene_sizes;
  {
    const auto& sc = m_cut_store_.scene_cuts();
    sim_before_scene_sizes.reserve(sc.size());
    for (const auto& scene_vec : sc) {
      sim_before_scene_sizes.push_back(scene_vec.size());
    }
  }
  {
    const auto final_iteration_index = results.empty()
        ? m_iteration_offset_
        : next(results.back().iteration_index);
    const auto final_start = std::chrono::steady_clock::now();

    SPDLOG_INFO("SDDP Sim [i{}]: === simulation pass ===",
                final_iteration_index);

    // Suppress stop checks so the simulation pass always completes.
    // The stop was already honoured by exiting the iteration loop.
    m_in_simulation_ = true;

    SDDPIterationResult ir {
        .iteration_index = final_gtopt::uid_of(iteration_index),
    };
    m_benders_cut_.reset_infeasible_cut_count();

    // ── Simulation stage: cache-backed single-pass ────────────────────
    //
    // Pass 1 (solve + populate cache):
    //   - crossover=true so `release_backend()` caches clean vertex
    //     duals, reduced costs, and primal values via the Phase 2a
    //     getter cache on `LinearInterface`.
    //   - Under low_memory != off, each cell releases aggressively
    //     after solve; collections + backend dropped; snapshot kept
    //     through retry loop and dropped via `drop_sim_snapshots()`
    //     once Pass 1 converges.  Per-cell RAM footprint collapses
    //     to ~400 KB of cache.
    //   - On infeasibility the existing elastic branch installs an
    //     fcut on the previous phase; the retry loop re-runs Pass 1
    //     until no further fcuts are needed or `kMaxSimP1Retries` is
    //     hit.
    //
    // Pass 2 = `PlanningLP::write_out()`: the caller already invokes it
    // right after `SDDPMethod::solve()` returns, and its cell-parallel
    // pool dispatches one write task per (scene, phase).  Under
    // compress the fast-path reads straight from the cache populated
    // in Pass 1 — no re-solve, no backend reconstruct, no snapshot.
    // Under off the cells' live backends supply the values.  Either
    // way the write is clean and happens exactly once per cell.
    //
    // This supersedes the earlier two-pass design where Pass 2 ran a
    // full forward traverse with `run_forward_pass_all_scenes`
    // (needlessly re-solving 619/816 cells on juan/iplp just to refresh
    // their duals).
    auto sim_opts = fwd_opts;
    sim_opts.crossover = true;

    // Bounded retry loop around Pass 1 so deeper infeasibility chains
    // don't loop forever; anything still infeasible after the last
    // attempt shows as status=-1 in solution.csv with no parquet shard.
    constexpr int kMaxSimP1Retries = 3;
    double p1_total_elapsed = 0.0;
    std::expected<ForwardPassOutcome, Error> fwd;
    int p1_attempts = 0;
    for (; p1_attempts <= kMaxSimP1Retries; ++p1_attempts) {
      SPDLOG_INFO(
          "SDDP Sim [i{}]: Pass 1 attempt {}/{} — solve + populate cache",
          final_gtopt::uid_of(iteration_index),
          p1_attempts + 1,
          kMaxSimP1Retries + 1);
      fwd = run_forward_pass_all_scenes(
          *sddp_pool, sim_opts, final_iteration_index);
      if (!fwd.has_value()) {
        monitor.stop();
        return std::unexpected(std::move(fwd.error()));
      }
      p1_total_elapsed += fwd->elapsed_s;
      if (!fwd->has_feasibility_issue) {
        break;
      }
      // If no feasibility cuts were installed this attempt, the LP
      // state is unchanged — retrying will produce the same result.
      // The remaining infeasible cells are terminally infeasible;
      // leave them to be reported as status=-1 in the parquet output.
      if (fwd->n_fcuts_installed == 0) {
        // No new fcuts means the LP state is unchanged across attempts;
        // retrying would just reproduce the same infeasibilities.  The
        // remaining infeasible cells are *terminal* — they will surface
        // as status=-1 rows in the parquet output.  Wording avoids the
        // misleading "attempt N/M … stopping retry loop" pairing of the
        // earlier message.
        const auto n_infeas =
            std::ranges::count(fwd->scene_feasible, uint8_t {0});
        SPDLOG_INFO(
            "SDDP Sim [i{}]: no new fcuts after attempt {} — "
            "{} infeasible scene(s) are terminal (status=-1 in output)",
            final_gtopt::uid_of(iteration_index),
            p1_attempts + 1,
            n_infeas);
        break;
      }
    }

    SPDLOG_INFO("SDDP Sim [i{}]: Pass 1 done after {} attempt(s)",
                final_gtopt::uid_of(iteration_index),
                p1_attempts + 1);

    // Aggressive post-Pass-1 memory release under low_memory: the
    // Phase-2a cache on each LinearInterface holds everything
    // `PlanningLP::write_out` needs (col_sol / col_cost / row_dual),
    // and `rebuild_collections_if_needed` re-flattens from the live
    // `System` element arrays — so the compressed flat-LP snapshot
    // can be dropped per cell here.  Frees a meaningful chunk of RAM
    // before the parallel write pass starts.
    if (m_options_.low_memory_mode != LowMemoryMode::off) {
      planning_lp().drop_sim_snapshots();
    }

    ir.scene_upper_bounds = std::move(fwd->scene_upper_bounds);
    // Copy — same reason as the training-pass move above; downstream
    // sim-pass code (output_skipped marking, save_cuts) reads
    // ``fwd->scene_feasible`` after this point.
    ir.scene_feasible = fwd->scene_feasible;
    ir.forward_pass_s = p1_total_elapsed;
    if (fwd->has_feasibility_issue) {
      ir.feasibility_issue = true;
      SPDLOG_WARN(
          "SDDP Sim [i{}]: residual feasibility issue — affected cells "
          "left unsolved in output",
          final_iteration_index);

      // Mark every (scene, phase) cell of an infeasible scene as
      // output_skipped so PlanningLP::write_out bypasses the
      // rehydrate + re-solve round-trip (which would just hit the
      // same infeasibility and produce meaningless output).  This
      // also covers the max_iterations=0 path: no training runs, the
      // sim pass alone drives which scenes are bad, and write_out
      // respects that single verdict.
      const auto num_scenes_sim = planning_lp().simulation().scene_count();
      const auto num_phases_sim = planning_lp().simulation().phase_count();
      std::size_t n_cells_skipped = 0;
      for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes_sim)) {
        if (scene_index >= std::ssize(fwd->scene_feasible)
            || fwd->scene_feasible[scene_index] != 0U)
        {
          continue;  // scene stayed feasible — leave its cells alone
        }
        for (const auto phase_index : iota_range<PhaseIndex>(0, num_phases_sim))
        {
          planning_lp()
              .system(scene_index, phase_index)
              .set_output_skipped(/*v=*/true);
          ++n_cells_skipped;
        }
      }
      if (n_cells_skipped > 0) {
        SPDLOG_INFO(
            "SDDP Sim [i{}]: {} cell(s) marked output_skipped "
            "(infeasible scenes will not be written out)",
            final_gtopt::uid_of(iteration_index),
            n_cells_skipped);
      }
    }

    const auto& scenes = planning_lp().simulation().scenes();
    const auto weights = compute_scene_weights(scenes, fwd->scene_feasible);
    compute_iteration_bounds(ir, fwd->scene_feasible, weights);

    ir.iteration_s = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - final_start)
                         .count();

    finalize_iteration_result(ir, final_iteration_index, results);

    // The simulation pass does not determine convergence on its own.
    // It inherits the convergence status from the last training iteration.
    // With max_iterations=0, no training ran, so converged stays false.
    ir.converged = !results.empty() && results.back().converged;

    results.push_back(ir);

    {
      // Same ``feasible=K/N`` clause as the per-iteration headline
      // above — only emitted when the sim pass had infeasible scenes,
      // so all-feasible runs stay terse.
      const auto sim_total = ir.scene_feasible.size();
      const auto sim_feasible = static_cast<std::size_t>(
          std::ranges::count(ir.scene_feasible, uint8_t {1}));
      const auto sim_clause = (sim_total > 0 && sim_feasible < sim_total)
          ? std::format(" feasible={}/{}", sim_feasible, sim_total)
          : std::string {};
      SPDLOG_INFO(
          "SDDP Sim [i{}]: done in {:.3f}s — "
          "UB={} LB={} gap={:.2f}% Δgap={:.2f}%{}{}",
          final_gtopt::uid_of(iteration_index),
          ir.iteration_s,
          format_si(ir.upper_bound),
          format_si(ir.lower_bound),
          100.0 * ir.gap,
          100.0 * ir.gap_change,
          sim_clause,
          ir.converged ? " [CONVERGED]" : "");
    }

    m_sim_write_enabled_ = false;
    m_in_simulation_ = false;
  }

  // Discard feasibility cuts produced during simulation unless
  // explicitly requested.  Per-scene resize: sim-pass fcuts land in
  // each scene's own vector (no scuts generated — sim pass has no
  // backward), so we can rewind each scene's vector to its pre-sim
  // size and log how many entries were dropped.
  if (!m_options_.save_simulation_cuts) {
    std::size_t dropped = 0;
    auto& scene_cuts = m_cut_store_.scene_cuts();
    for (std::size_t si_sz = 0;
         si_sz < scene_cuts.size() && si_sz < sim_before_scene_sizes.size();
         ++si_sz)
    {
      const auto si = SceneIndex {static_cast<Index>(si_sz)};
      const auto pre = sim_before_scene_sizes[si_sz];
      if (scene_cuts[si].size() > pre) {
        dropped += scene_cuts[si].size() - pre;
        scene_cuts[si].resize(pre);
      }
    }
    if (dropped > 0) {
      SPDLOG_INFO("SDDP: discarding {} simulation feasibility cut(s)", dropped);
    }
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
      const auto cuts_to_append = build_combined_cuts();
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

  // Advance `m_iteration_offset_` past the last iteration executed
  // (including the trailing simulation pass) so a subsequent call
  // to `solve()` on the same SDDPMethod instance keeps its
  // `(scene, phase, iter, offset)` tuple on each emitted cut
  // disjoint from cuts already in the LP.  Without this, the
  // eager metadata duplicate detector in
  // `LinearInterface::add_row` fires on the first backward-pass
  // cut of the re-entry (same `iter=0, offset=0` as the first
  // run).  Placed here, after the simulation pass, so the
  // convergence-compute inside `record_iteration_result` still
  // sees the old offset for its `past_min_iter` check.
  if (!results.empty()) {
    m_iteration_offset_ = next(results.back().iteration_index);
  }

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
        SPDLOG_INFO(
            "SDDP Async [i{} s{}]: scene {} converged at iter {} (gap={:.6f})",
            gtopt::uid_of(iteration_index),
            uid_of(scene_index),
            uid_of(scene_index),
            gtopt::uid_of(iteration_index),
            scene_gap);
        return true;
      }
    }
    return tracker.is_converged(scene_index);
  };

  // ── Per-scene simulation + output writing ──
  // Runs a simulation forward pass for a single scene, then writes output.
  // Enable crossover: output writing reads row duals (bus marginal
  // prices, etc.), which require vertex duals.  Training forward passes
  // keep crossover off because cuts use reduced costs only.
  auto sim_opts = fwd_opts;
  sim_opts.crossover = true;
  const auto run_scene_simulation =
      [&](SceneIndex scene_index,
          IterationIndex sim_iteration_index) -> std::expected<double, Error>
  {
    SPDLOG_INFO("SDDP Sim [i{} s{}]: simulation pass for scene {}",
                sim_gtopt::uid_of(iteration_index),
                uid_of(scene_index),
                uid_of(scene_index));
    auto result = forward_pass(scene_index, sim_opts, sim_iteration_index);
    if (!result.has_value()) {
      return result;
    }

    // Write output for this scene's phases
    for (const auto phase_index : iota_range<PhaseIndex>(0, num_phases)) {
      planning_lp().system(scene_index, phase_index).write_out();
    }

    SPDLOG_INFO("SDDP Sim [i{} s{}]: scene {} outputs written",
                sim_gtopt::uid_of(iteration_index),
                uid_of(scene_index),
                uid_of(scene_index));
    return result;
  };

  // ── Main orchestration loop ──

  auto is_future_ready = [](const auto& fut)
  {
    return fut.valid()
        && fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
  };

  bool all_done = false;
  // Per-scene cut-count snapshot so sim-pass-added feasibility cuts can
  // be rewound below without affecting training cuts.
  std::vector<std::size_t> sim_before_scene_sizes;
  {
    const auto& sc = m_cut_store_.scene_cuts();
    sim_before_scene_sizes.reserve(sc.size());
    for (const auto& scene_vec : sc) {
      sim_before_scene_sizes.push_back(scene_vec.size());
    }
  }

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
            SPDLOG_WARN("SDDP Forward [i{} s{}]: async forward failed: {}",
                        sp.current_gtopt::uid_of(iteration_index),
                        uid_of(scene),
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
                       &bwd_opts,
                       iteration_index = sp.current_iteration_index]
                      {
                        return backward_pass_with_apertures(
                            scene, bwd_opts, iteration_index);
                      },
                      bwd_req)
                : pool.submit(
                      [this,
                       scene,
                       &bwd_opts,
                       iteration_index = sp.current_iteration_index]
                      {
                        return backward_pass(scene, bwd_opts, iteration_index);
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
            SPDLOG_WARN("SDDP Backward [i{} s{}]: async backward failed: {}",
                        sp.current_gtopt::uid_of(iteration_index),
                        uid_of(scene),
                        bwd.error().message);
          }

          // Get lower bound from phase-0 objective in PHYSICAL ($)
          // units — must match the physical-space upper-bound
          // aggregated in sddp_forward_pass (which uses
          // ``get_obj_value()``).  Reading LP-raw here put
          // UB and LB in different unit spaces, producing a
          // permanent ``≈ scale_objective − 1`` / ``scale_objective``
          // gap that never closed regardless of cut quality.
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
                "SDDP Async [i{} s{}]: completed (ub={:.4f} lb={:.4f}"
                " gap={:.6f})",
                sp.current_gtopt::uid_of(iteration_index),
                uid_of(scene),
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
            SPDLOG_WARN("SDDP Sim [i{} s{}]: simulation failed: {}",
                        sp.current_gtopt::uid_of(iteration_index),
                        uid_of(scene),
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
                "SDDP Async [i{} s{}]: scene done — converged={} iters={}"
                " sim_ub={:.4f} active={}/{}",
                sp.current_gtopt::uid_of(iteration_index),
                uid_of(scene),
                sp.scene_converged,
                iteration_relative(sp.current_gtopt::uid_of(iteration_index),
                                   m_iteration_offset_),
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
      for (const auto& [si, b] : enumerate(bounds)) {
        scene_feasible[si] = b.feasible ? 1U : 0U;
      }

      const auto& scenes = planning_lp().simulation().scenes();
      const auto weights =
          compute_scene_weights(scenes, scene_feasible, prob_mode);

      SDDPIterationResult ir {
          .iteration_index = next_converge_iteration_index,
      };

      // Fill per-scene bounds.  Aggregate UB / LB by plain SUM —
      // each ``b.upper_bound`` already has its scenario probability
      // baked in via ``CostHelper::block_ecost``, so the cross-scene
      // aggregation must not multiply by ``weights[si]`` again
      // (which would double-count probability).  See the comment at
      // ``compute_iteration_bounds`` for the full derivation.  The
      // ``weights`` span computed above stays available for cut-
      // sharing aggregation; it is no longer consulted for bounds.
      (void)weights;  // intentionally unused for bound aggregation
      ir.scene_upper_bounds.resize(static_cast<std::size_t>(num_scenes));
      ir.scene_lower_bounds.resize(static_cast<std::size_t>(num_scenes));
      double sum_upper = 0.0;
      double sum_lower = 0.0;
      for (const auto& [si, b] : enumerate(bounds)) {
        if (scene_feasible[si] == 0U) {
          continue;
        }
        ir.scene_upper_bounds[si] = b.upper_bound;
        ir.scene_lower_bounds[si] = b.lower_bound;
        sum_upper += b.upper_bound;
        sum_lower += b.lower_bound;
      }
      ir.upper_bound = sum_upper;
      ir.lower_bound = sum_lower;

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

        // Negative-gap guard sourced from the shared
        // `kSddpGapFpEpsilon` (sddp_types.hpp).
        if (!ir.converged && past_min_iterations && results.size() >= window
            && ir.gap > -kSddpGapFpEpsilon
            && ir.gap_change < m_options_.stationary_tol && ir.gap_change < 1.0
            && m_options_.convergence_mode != ConvergenceMode::gap_only)
        {
          ir.converged = true;
          ir.stationary_converged = true;
        }
      }

      // Publish a fresh coherent live-metrics snapshot.  Preserves the
      // `converged` flag if an earlier in-iteration check already set
      // it (stationary / CI convergence blocks above).
      const bool was_converged = has_converged();
      publish_live_metrics_(LiveMetrics {
          .iteration = next_converge_iteration_index,
          .gap = ir.gap,
          .lower_bound = ir.lower_bound,
          .upper_bound = ir.upper_bound,
          .converged = ir.converged || was_converged,
      });

      // Log aggregate with pool stats
      {
        const auto pool_stats = pool.get_statistics();
        SPDLOG_INFO(
            "SDDP Iter [i{}]: async aggregate — "
            "UB={} LB={} gap={:.2f}% Δgap={:.2f}% "
            "spread=[{},{}] converged_scenes={}/{} "
            "pool(active={} pending={} cpu={:.0f}%){}",
            next_converge_gtopt::uid_of(iteration_index),
            format_si(ir.upper_bound),
            format_si(ir.lower_bound),
            100.0 * ir.gap,
            100.0 * ir.gap_change,
            tracker.min_completed_iteration(),
            tracker.max_completed_iteration(),
            tracker.num_converged(),
            num_scenes,
            pool_stats.tasks_active,
            pool_stats.tasks_pending,
            pool_stats.current_cpu_load,
            ir.converged ? " [CONVERGED]" : "");
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
        const auto metrics = live_metrics_();
        const SolverStatusSnapshot snapshot {
            .iteration_index = metrics->iteration,
            .gap = metrics->gap,
            .lower_bound = metrics->lower_bound,
            .upper_bound = metrics->upper_bound,
            .converged = metrics->converged,
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
        SPDLOG_INFO("SDDP Iter [i{}]: callback requested stop",
                    next_converge_iteration_index);
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
  if (!m_options_.save_simulation_cuts) {
    std::size_t dropped = 0;
    auto& scene_cuts = m_cut_store_.scene_cuts();
    for (std::size_t si_sz = 0;
         si_sz < scene_cuts.size() && si_sz < sim_before_scene_sizes.size();
         ++si_sz)
    {
      const auto si = SceneIndex {static_cast<Index>(si_sz)};
      const auto pre = sim_before_scene_sizes[si_sz];
      if (scene_cuts[si].size() > pre) {
        dropped += scene_cuts[si].size() - pre;
        scene_cuts[si].resize(pre);
      }
    }
    if (dropped > 0) {
      SPDLOG_INFO("SDDP: discarding {} simulation feasibility cut(s)", dropped);
    }
  }

  // ── Cut persistence ──
  if (!m_options_.cuts_output_file.empty() && !results.empty()) {
    const auto num_scenes_final = planning_lp().simulation().scene_count();
    std::vector<uint8_t> final_feasible(
        static_cast<std::size_t>(num_scenes_final), 1U);
    save_cuts_for_iteration(results.back().iteration_index, final_feasible);

    const auto mode = m_options_.cut_recovery_mode;
    if (mode == HotStartMode::append) {
      const auto cuts_to_append = build_combined_cuts();
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
