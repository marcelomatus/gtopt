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
#include <utility>
#include <vector>

#include <gtopt/enum_option.hpp>
#include <gtopt/log_format.hpp>
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
    ZScoreBucket {
        .alpha_max = std::numeric_limits<double>::infinity(),
        .z = 1.282,
    },  // 80 % fallback
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

/// Decomposition of the stationary-convergence test into its three
/// individual signals plus a combined verdict.  Used by both the
/// synchronous ``SDDPMethod::iterate()`` and asynchronous
/// ``SDDPMethod::solve_async()`` paths so the convergence semantics
/// stay in lockstep — the prior pair of inline checks was easy to
/// drift apart when one site was tweaked but not the other.
///
/// Semantics (all AND'd, after 2026-05-14 rewrite — ΔUB-only):
///
///   1. ``past_min_iterations`` — caller's iter index has cleared the
///      ``SDDPOptions::min_iterations`` bootstrap floor.
///   2. ``stationary_ok`` — relative ΔUB lookback is below
///      ``stationary_tol`` (carried in
///      ``SDDPIterationResult::gap_change`` — see field comment).
///      Guarded only against the ``1.0`` default sentinel so an
///      uninitialised iter cannot trigger.  **No window-fill guard**:
///      under the multi-cut overshoot regime UB stabilises long
///      before LB does, and waiting for a full window just wastes
///      wall time at expensive levels (juan/full_network ≈ 13 min
///      per iter).  Always ``false`` when ``convergence_mode ==
///      gap_only`` (the stationary path is intentionally inert there).
///
/// The former ``gap_ok`` clause (``|ir.gap| < stationary_gap_ceiling``)
/// was REMOVED (2026-06-16): it was AND'd into ``converged()`` and so
/// acted as a hard convergence gate, contradicting its documented role
/// as a mere "safety net".  Under the multi-cut / multicut overshoot
/// regime the signed gap stabilises at a large NEGATIVE value (LB > UB
/// because the shared cuts do not perfectly match each scene's own
/// value function) — a stationary, non-improving state that the ceiling
/// nonetheless refused to call converged, wasting every remaining
/// iteration at expensive levels.  ΔUB stationarity is the sole signal:
/// once the upper bound stops moving the level has converged regardless
/// of the residual signed-gap magnitude.
struct StationaryCheck
{
  bool past_min_iterations {false};
  bool stationary_ok {false};

  /// AND of both signals — the canonical stop predicate.
  [[nodiscard]] auto converged() const noexcept -> bool
  {
    return past_min_iterations && stationary_ok;
  }
};

[[nodiscard]] inline auto evaluate_stationary_check(
    const SDDPIterationResult& ir,
    IterationIndex iteration_index,
    IterationIndex iteration_offset,
    std::size_t /*results_count*/,
    const SDDPOptions& opts) noexcept -> StationaryCheck
{
  StationaryCheck c;

  c.past_min_iterations = iteration_index
      >= iteration_offset + IterationIndex {opts.min_iterations - 1};

  if (opts.convergence_mode != ConvergenceMode::gap_only
      && opts.stationary_tol > 0.0 && opts.stationary_window > 0)
  {
    // ΔUB stationarity fires as soon as the lookback Δ drops below
    // tol.  The ``< 1.0`` sentinel guard still blocks the very first
    // iter (when ``ir.gap_change`` keeps its default).  No
    // ``results_count >= window`` guard — see StationaryCheck docs.
    c.stationary_ok =
        (ir.gap_change < opts.stationary_tol && ir.gap_change < 1.0);
  }

  return c;
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
  const auto cell_task_headroom = planning_lp().simulation().scene_count();
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
            /*memory_limit_mb=*/m_options_.pool_memory_limit_mb,
            /*pool_label=*/"SDDP aux pool")
      : std::unique_ptr<AdaptiveWorkPool> {};
  m_pool_ = sddp_pool.get();
  m_solver_tier_.set_pool(m_pool_);
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
  // Forward pass crossover.  The INTENT is barrier WITHOUT crossover (none) —
  // fast interior solves, vertex duals recovered later only where needed.
  // But the no-aperture Benders path consumes this solve's duals DIRECTLY as
  // the cut, and the interior (analytic-center) duals there make some
  // SDDP/cascade runs overshoot (LB>UB) — see test_cascade_integration and
  // test_sddp_async.  Until that cut path recovers VERTEX duals (a crossover
  // or dual-simplex re-solve), the forward stays `automatic` (CPLEX crosses
  // over → vertex duals), which is also the real pre-fix behaviour (the old
  // `crossover=false` → `BarCrossAlg=-1` was silently rejected by CPLEX).
  // `none` remains available opt-in via `forward_solver_options`.
  auto fwd_opts = m_options_.forward_solver_options.value_or(lp_opts);
  // Crossover is only meaningful — and only needed for vertex duals — when the
  // forward LP is solved by BARRIER (or the solver's default, which picks
  // barrier on these LPs).  The simplex methods (primal/dual) already produce
  // a vertex basis with vertex duals, so forcing crossover on them is a no-op
  // at best and, by pinning `automatic`, would suppress a warm dual-simplex
  // resolve.  Force crossover only for the barrier/default case; respect an
  // explicit `forward_solver_options.algorithm = primal|dual` (settable via
  // `--set sddp_options.forward_solver_options.algorithm=dual`), which is what
  // the basis warm-start (`basis_cross_mode`) needs to actually exploit the
  // seeded basis.
  if (fwd_opts.algorithm == LPAlgo::barrier
      || fwd_opts.algorithm == LPAlgo::default_algo)
  {
    fwd_opts.crossover = CrossoverMode::automatic;
  }
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
          .iteration_index = iteration_index,
      };
      m_benders_cut_.reset_infeasible_cut_count();

      // ── Forward pass ──
      SPDLOG_DEBUG("SDDP Forward [i{}]: starting forward pass",
                   gtopt::uid_of(iteration_index));
      auto fwd =
          run_forward_pass_all_scenes(*sddp_pool, fwd_opts, iteration_index);
      planning_lp().log_lp_memory_breakdown("post-forward");
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
                   gtopt::uid_of(iteration_index));
      // Save per-scene cut counts for cut sharing offset tracking.
      // Stored on each `SceneCutStore` (per-scene snapshot) post-step-4
      // of
      // `docs/analysis/investigations/sddp/sddp_cut_store_split_plan_2026-04-30.md`;
      // the legacy parallel `m_scene_cuts_before_` vector is gone.
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
            gtopt::uid_of(iteration_index));
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

      // ── Convergence (single decision site) ──
      //
      // After the 2026-05-14 rewrite the convergence decision lives in
      // exactly one place: ``StationaryCheck::converged()`` returned by
      // ``evaluate_stationary_check`` (anonymous namespace, top of this
      // TU).  ``finalize_iteration_result`` no longer sets
      // ``ir.converged`` — it only computes ``ir.gap`` and
      // ``ir.gap_change`` and emits the negative-gap WARN.
      //
      // The convergence signal is **ΔUB stationarity** alone; the
      // ``|gap| < stationary_gap_ceiling`` gate was removed (2026-06-16)
      // because it blocked stationary multicut-overshoot states from
      // ever converging.  See the StationaryCheck docs for the rationale
      // (multi-cut overshoot makes the signed gap non-monotone and can
      // leave it stationary at a large negative value; UB is the
      // policy-cost MC estimate we are optimising).
      //
      // ``convergence_mode`` gating:
      //   gap_only       → stationary path inert (stationary_ok=false);
      //                    only the pathology safety net is active.
      //                    With the primary gap-vs-tol exit removed
      //                    this mode is effectively "never converge",
      //                    kept only for legacy / regression tests.
      //   gap_stationary → ΔUB stationarity drives convergence (default).
      //   statistical    → adds the CI test block below.

      const auto mode = m_options_.convergence_mode;

      const auto stationary = evaluate_stationary_check(
          ir, iteration_index, m_iteration_offset_, results.size(), m_options_);
      const bool past_min_iterations = stationary.past_min_iterations;
      const bool gap_is_stationary = stationary.stationary_ok;

      if (!ir.converged && stationary.converged()) {
        ir.converged = true;
        ir.stationary_converged = true;
        update_live_metrics_([](LiveMetrics& m) { m.converged = true; });
        SPDLOG_INFO(
            "SDDP Iter [i{}]: ΔUB stationary convergence "
            "(ΔUB={:.6f} < tol={:.6f}; signed gap={:.4f}) [CONVERGED]",
            gtopt::uid_of(iteration_index),
            ir.gap_change,
            m_options_.stationary_tol,
            ir.gap);
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
        // The ``stationary_gap_ceiling`` clause was removed here too
        // (2026-06-16) — see the StationaryCheck docs: it gated
        // convergence on |gap| and so refused stationary multicut
        // overshoot states.  The CI test (``gap_abs <= ci_threshold``)
        // and the negative-gap FP guard remain.
        if (gap_abs > -kSddpGapFpEpsilon && gap_abs <= ci_threshold) {
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
        // (b) Gap exceeds CI but is no longer improving.  ΔUB
        // stationarity alone (the gap_ceiling magnitude clause was
        // removed 2026-06-16 — see the StationaryCheck docs).
        else if (gap_is_stationary)
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
      // Surface kappa observed THIS ITER only (not the historical
      // max), so the per-iter log clause reflects the current LP's
      // condition rather than repeating an old LP-construction probe
      // forever.  Under CPLEX barrier without crossover the per-iter
      // resolve produces no basis ⇒ no fresh kappa ⇒ the clause is
      // suppressed (honest "no signal" vs misleading stale value).
      const auto gk = current_iter_max_kappa(iteration_index);
      const auto kappa_clause =
          (gk >= 0.0) ? std::format(" kappa={:.2e}", gk) : std::string {};
      // ── Three-line iteration summary ──
      // Line 1: header with elapsed & ETA so operators can tell
      //         "are we 30% done?" at a glance.
      // Line 2: objective + convergence telemetry.
      // Line 3: wall-time breakdown + cut count.
      // Each line carries `iter N` so a grep for the iteration index
      // returns the whole block.
      const auto iter_id = gtopt::uid_of(iteration_index);
      const auto elapsed_s = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - solve_start)
                                 .count();
      const auto avg_iter_s = elapsed_s / static_cast<double>(iter_id + 1);
      const auto max_iters = m_options_.max_iterations;
      const auto remaining_iters = (max_iters > 0 && iter_id + 1 < max_iters)
          ? static_cast<int>(max_iters) - 1 - static_cast<int>(iter_id)
          : 0;
      const auto eta_s = (remaining_iters > 0 && !ir.converged)
          ? remaining_iters * avg_iter_s
          : 0.0;
      const auto eta_clause = (eta_s > 0.0)
          ? std::format("  ETA={}", gtopt::log::dur(eta_s))
          : std::string {};
      const auto conv_clause =
          ir.converged ? std::string {"  [CONVERGED]"} : std::string {};
      SPDLOG_INFO("─── iter {} ───  elapsed={}{}{}",
                  iter_id,
                  gtopt::log::dur(elapsed_s),
                  eta_clause,
                  conv_clause);
      SPDLOG_INFO("    iter {}  obj   ub={}  lb={}  gap={}  Δgap={}{}{}",
                  iter_id,
                  gtopt::log::money(ir.upper_bound),
                  gtopt::log::money(ir.lower_bound),
                  gtopt::log::pct(ir.gap),
                  gtopt::log::pct(ir.gap_change),
                  kappa_clause,
                  feasibility_clause);
      SPDLOG_INFO("    iter {}  time  total={}  (fwd={} + bwd={})  cuts={}{}",
                  iter_id,
                  gtopt::log::dur(ir.iteration_s),
                  gtopt::log::dur(ir.forward_pass_s),
                  gtopt::log::dur(ir.backward_pass_s),
                  ir.cuts_added,
                  ir.infeasible_cuts_added > 0
                      ? std::format(" (+{} feas)", ir.infeasible_cuts_added)
                      : std::string {});

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
                    gtopt::uid_of(iteration_index));
        break;
      }

      if (ir.converged) {
        break;
      }
    }
  }

  // ── Simulation pass ──
  // Normally run a simulation (forward-only) pass after training
  // iterations complete (or when max_iterations == 0).  This evaluates
  // the policy with all accumulated cuts, producing the definitive
  // output.  No backward pass is run, so no new optimality cuts are
  // generated.  Feasibility cuts (from the elastic filter) may still
  // be produced.  By default (save_simulation_cuts=false) they are
  // discarded to ensure hot-start reproducibility.
  //
  // Cascade intermediate levels set `skip_simulation_pass = true` —
  // their state-variable targets are read from the last training
  // forward pass and their per-cell outputs would be overwritten by
  // the next level's sim pass anyway, so the simulation here is pure
  // overhead.  When skipped, control falls through to the tail
  // (cuts persistence, monitor.stop, m_iteration_offset_ advance);
  // skipping any of those would leak state into the next cascade
  // level's solver instance.
  if (m_options_.skip_simulation_pass) {
    SPDLOG_INFO("SDDP: simulation pass skipped (skip_simulation_pass=true)");
  } else {
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

      SPDLOG_INFO("{}: === simulation pass ===",
                  sddp_log("Sim", gtopt::uid_of(final_iteration_index)));

      // Suppress stop checks so the simulation pass always completes.
      // The stop was already honoured by exiting the iteration loop.
      m_in_simulation_ = true;

      SDDPIterationResult ir {
          .iteration_index = final_iteration_index,
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
      sim_opts.crossover = CrossoverMode::primal;

      // Bounded retry loop around Pass 1 so deeper infeasibility chains
      // don't loop forever; anything still infeasible after the last
      // attempt shows as status=-1 in solution.csv with no parquet shard.
      constexpr int kMaxSimP1Retries = 3;
      double p1_total_elapsed = 0.0;
      std::expected<ForwardPassOutcome, Error> fwd;
      int p1_attempts = 0;
      for (; p1_attempts <= kMaxSimP1Retries; ++p1_attempts) {
        SPDLOG_INFO("{}: Pass 1 attempt {}/{} — solve + populate cache",
                    sddp_log("Sim", gtopt::uid_of(final_iteration_index)),
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
              "{}: no new fcuts after attempt {} — "
              "{} infeasible scene(s) are terminal (status=-1 in output)",
              sddp_log("Sim", gtopt::uid_of(final_iteration_index)),
              p1_attempts + 1,
              n_infeas);
          break;
        }
      }

      SPDLOG_INFO("{}: Pass 1 done after {} attempt(s)",
                  sddp_log("Sim", gtopt::uid_of(final_iteration_index)),
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
            "{}: residual feasibility issue — affected cells "
            "left unsolved in output",
            sddp_log("Sim", gtopt::uid_of(final_iteration_index)));

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
        for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes_sim))
        {
          if (scene_index >= std::ssize(fwd->scene_feasible)
              || fwd->scene_feasible[scene_index] != 0U)
          {
            continue;  // scene stayed feasible — leave its cells alone
          }
          for (const auto phase_index :
               iota_range<PhaseIndex>(0, num_phases_sim))
          {
            planning_lp()
                .system(scene_index, phase_index)
                .set_output_skipped(/*v=*/true);
            ++n_cells_skipped;
          }
        }
        if (n_cells_skipped > 0) {
          SPDLOG_INFO(
              "{}: {} cell(s) marked output_skipped "
              "(infeasible scenes will not be written out)",
              sddp_log("Sim", gtopt::uid_of(final_iteration_index)),
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
      // ``finalize_iteration_result`` just published live-metrics with
      // ``converged=false`` (its only data point — sim ir starts fresh).
      // Re-publish the inherited flag so ``has_converged()`` agrees with
      // ``results->back().converged`` post-sim.  Pre-2026-05-14 this was
      // implicit because ``finalize_iteration_result`` also computed
      // ``ir.converged`` from the primary gap test before publishing.
      if (ir.converged) {
        update_live_metrics_([](LiveMetrics& m) { m.converged = true; });
      }

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
            "{}: done in {:.3f}s — "
            "ub={} lb={} gap={:.2f}% Δgap={:.2f}%{}{}",
            sddp_log("Sim", gtopt::uid_of(final_iteration_index)),
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
        SPDLOG_INFO("SDDP: discarding {} simulation feasibility cut(s)",
                    dropped);
      }
    }
  }  // end if (!skip_simulation_pass)

  // Per-step timing trace for the solve() tail.  Profile on
  // `cases/sddp_hydro_3phase` showed a ~298 ms gap between the last
  // SDDP iteration log line and `Optimization time` on a 0.5-s wall
  // run — almost none of it inside the work-pool's shutdown
  // (verified at <2 ms via the per-pool shutdown trace landed in
  // d2e1a364).  These traces partition that gap into
  // cut-persistence / monitor.stop / lp_debug drain / pool reset
  // + planning_lp tail so a future profiler can localise where
  // the time actually goes.
  using TailClock = std::chrono::steady_clock;
  const auto t_tail_begin = TailClock::now();
  const auto tail_elapsed_ms =
      [](TailClock::time_point a, TailClock::time_point b)
  { return std::chrono::duration<double, std::milli>(b - a).count(); };

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
      auto result = save_cuts_parquet(cuts_to_append,
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
  const auto t_after_cuts = TailClock::now();

  monitor.stop();
  const auto t_after_monitor = TailClock::now();

  m_lp_debug_writer_.drain();
  const auto t_after_drain = TailClock::now();

  m_benders_cut_.set_pool(nullptr);
  m_pool_ = nullptr;
  m_solver_tier_.set_pool(nullptr);
  m_aux_pool_ = nullptr;
  m_lp_debug_writer_ = {};
  const auto t_after_reset = TailClock::now();

  spdlog::trace(
      "SDDP solve() tail: cuts_persistence={:.1f}ms monitor.stop={:.1f}ms "
      "lp_debug.drain={:.1f}ms pool_reset+lp_debug_dtor={:.1f}ms",
      tail_elapsed_ms(t_tail_begin, t_after_cuts),
      tail_elapsed_ms(t_after_cuts, t_after_monitor),
      tail_elapsed_ms(t_after_monitor, t_after_drain),
      tail_elapsed_ms(t_after_drain, t_after_reset));

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

  // Surface α / varphi_s + the rebase constant c̄ on any FutureCost element so
  // they are written to the solution.  Done after the iterations (which reset
  // the per-cell FutureCostLP); read-only w.r.t. the LP.
  populate_future_cost_output();

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

  // Per-iter accumulator for Benders cuts added across all scenes.  The
  // synchronous `iterate()` path sets `ir.cuts_added = bwd.total_cuts`
  // from `run_backward_pass_all_scenes`, but in the async path the
  // backward pass is dispatched per-scene as a future and the per-scene
  // counts (the `int` carried by `bwd_future`) had nowhere to land —
  // every iteration result reported `cuts_added = 0` and downstream
  // consumers (cascade `new_cuts=...` log, level_stats, regression
  // tests) silently lost the count.  Indexed by `iteration_relative()`
  // to stay aligned with the async tracker's per-iter sparse fills.
  std::vector<int> async_cuts_per_iter(
      static_cast<std::size_t>(m_options_.max_iterations) + 1, 0);

  auto next_converge_iteration_index = m_iteration_offset_;
  const bool use_apertures =
      !m_options_.apertures || !m_options_.apertures->empty();

  // Probability mode for scene weights
  const auto prob_mode =
      planning_lp().planning().simulation.probability_rescale.value_or(
          ProbabilityRescaleMode::runtime);

  // ── Per-scene convergence helper ──
  // A scene is individually converged when its own gap < tol for enough
  // iters.
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
      // Two-sided window: ``scene_gap`` is computed as
      // ``(UB - LB) / max(1, |UB|)`` and goes strongly negative
      // (e.g. ``-11.84``) when the per-scene LB transiently
      // overshoots its UB — a routine SDDP behaviour while the
      // phase-0 LP objective floats above the realised rollout
      // cost.  Without the lower-bound guard, ``scene_gap < tol``
      // evaluates to ``true`` for every negative gap (since
      // ``-11.84 < 1e-3``), silently declaring an unconverged
      // scene "converged".  Observed on juan/IPLP hs9: 9 dry
      // scenarios (s1, s3, s7, s10-13, s15-16) marked converged
      // at iter 54 with gaps in [-11.84, -0.077] while their
      // aggregate gap was 12.22 % — far above the 1e-3
      // convergence_tol.  The downstream effect was the SDDP
      // outer loop terminating after 3 aggregate iterations
      // because every "completed" scene short-circuits the
      // ``tracker.all_complete()`` gate (sddp_iteration.cpp:1553).
      //
      // Every other convergence site (aggregate stationary at
      // line 438, statistical CI at 594/604/632/1622, synchronous
      // gap at sddp_method_iteration.cpp:1675/1701) already
      // carries the same ``> -kSddpGapFpEpsilon`` guard; the
      // async per-scene path was added later and missed it.
      const bool gap_in_window = scene_gap < m_options_.convergence_tol
          && scene_gap > -kSddpGapFpEpsilon;
      if (gap_in_window && past_min) {
        tracker.mark_converged(scene_index, iteration_index);
        SPDLOG_INFO(
            "{}: converged (gap={:.6f})",
            sddp_log(
                "Async", gtopt::uid_of(iteration_index), uid_of(scene_index)),
            scene_gap);
        // NOTE: do NOT drop the per-phase compressed flat-LP
        // snapshots here.  The original (reverted) version of this
        // block did so on the assumption that "the sim-pass write_out
        // runs on cached scalars without reconstruct_backend()" — but
        // `run_scene_simulation` first calls `forward_pass`
        // (sddp_iteration.cpp:1207) which requires a live backend
        // (a real crossover solve, with row duals for output).  Under
        // `LowMemoryMode::compress` that solve goes through
        // `ensure_backend()` → `reconstruct_backend()`, which would
        // throw with "snapshot reconstruct returned without restoring
        // the backend" if we had dropped the snapshot at convergence
        // time.  This is the cascade-method test failure mode (see
        // `test_hydro_4b_cascade_gtopt_solve`).
        //
        // The snapshot is dropped later anyway by
        // `PlanningLP::write_out`'s per-cell `clear_snapshot()` after
        // each cell's parquet output is emitted (see planning_lp.cpp
        // ~line 1484), so total run-end RSS is unchanged.  We only
        // lose the brief mid-run window of "scene converged but other
        // scenes still iterating" — typically a small fraction of
        // total wall time, and never the peak.
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
  sim_opts.crossover = CrossoverMode::primal;
  const auto run_scene_simulation =
      [&](SceneIndex scene_index,
          IterationIndex sim_iteration_index) -> std::expected<double, Error>
  {
    SPDLOG_INFO(
        "{}: simulation pass",
        sddp_log(
            "Sim", gtopt::uid_of(sim_iteration_index), uid_of(scene_index)));
    auto result = forward_pass(scene_index, sim_opts, sim_iteration_index);
    if (!result.has_value()) {
      return result;
    }

    // Output is NOT written here.  Writing per-scene inline (serial over
    // this scene's 51 phases, with `num_scenes` scene-tasks concurrent)
    // rebuilt the full ~per-cell collections for every written cell and
    // kept them resident — RSS grew as ~num_cells × per-cell (measured:
    // ~34 GB on the 2-year case, the dominant floor + livelock trigger) —
    // AND capped write parallelism at the scene count, leaving cores idle
    // on low-scene cases.
    //
    // Instead, the forward solve (crossover=true) leaves every cell
    // `release_backend()`-ed with its primal/dual/reduced-cost cache
    // retained.  The final `PlanningLP::write_out` flush then writes ALL
    // (scene, phase) cells through its hybrid CHUNKED, memory-gated,
    // cell-parallel pool — Fast-path-B emits from the cache without a
    // reconstruct and drops each cell after.  That uses every core
    // regardless of scene count and bounds resident memory to the active
    // chunk set.  See `PlanningLP::write_out` (planning_lp.cpp).
    SPDLOG_INFO(
        "{}: simulation solved (write deferred to chunked flush)",
        sddp_log(
            "Sim", gtopt::uid_of(sim_iteration_index), uid_of(scene_index)));
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
  // be rewound below without affecting training cuts.  Sized lazily —
  // each scene captures its OWN snapshot when it transitions from
  // training (`backward_running` / `idle`) into the simulation pass
  // (`simulation_running`), so the snapshot reflects the
  // post-training cut count for that scene specifically.  The
  // previous implementation captured at function start (= 0 cuts for
  // every scene) and the tail discard then resized every scene back
  // to 0, silently wiping every Optimality cut produced during
  // training — observed on juan/iplp_plain as
  // `Cascade [...]: new_cuts=1600` followed by
  // `stored_cuts.size()=0 num_stored_cuts(direct)=0` at the cascade
  // transition.  Default `std::numeric_limits<std::size_t>::max()`
  // sentinel marks "scene never reached sim pass" — the tail discard
  // treats those as no-op (no shrink) so non-finishing scenes keep
  // every backward cut.
  std::vector<std::size_t> sim_before_scene_sizes(
      static_cast<std::size_t>(num_scenes),
      std::numeric_limits<std::size_t>::max());

  // Per-scene cut-store snapshot at the moment `m_stop_requested_`
  // fires from aggregate convergence (line ~2125).  Any cuts added
  // by scenes that finished their in-flight forward+backward pass
  // AFTER the stop signal but BEFORE the orchestration loop noticed
  // are race-condition cuts: they depend on the timing of when the
  // pool drained each task vs. when the master picked up the
  // results.  Without truncation, those cuts get serialised to the
  // next cascade level and the inherited cut count is
  // non-deterministic between runs of the same case (drain-pass
  // races change the per-scene count).  Truncating back to this
  // snapshot at the end of orchestrate_async makes the inherited
  // cut set match exactly what was certified at the converged iter.
  //
  // Same default sentinel as `sim_before_scene_sizes`: "stop never
  // fired for this scene" → no truncation.
  std::vector<std::size_t> stop_before_scene_sizes(
      static_cast<std::size_t>(num_scenes),
      std::numeric_limits<std::size_t>::max());

  while (!all_done) {
    for (const auto scene : iota_range<SceneIndex>(0, num_scenes)) {
      auto& sp = progress[static_cast<std::size_t>(scene)];

      switch (sp.state) {
        case SceneState::idle: {
          // Scene finished training when any of these fires:
          //   * per-scene convergence flag (`sp.scene_converged`)
          //   * exhausted iteration budget
          //   * aggregate convergence has flipped `m_stop_requested_`.
          //
          // The third condition is what catches the cascade's
          // last-active level: aggregate convergence fires (see
          // `if (... results.back().converged ...)` at the bottom of
          // this loop), `m_stop_requested_` is set, and on the next
          // orchestration tick every idle scene needs to enter its
          // sim pass — otherwise the stop-check below would short-
          // circuit straight to `done`, the sim pass would never
          // run, and the per-cell parquet output would carry only
          // last-training-forward primal data (no row duals, since
          // training forward passes run with crossover=false).
          //
          // **Important**: we read `m_stop_requested_` DIRECTLY here
          // rather than calling `should_stop()`.  `should_stop()` has
          // an `if (m_in_simulation_) return false;` guard so the
          // sync-iter loop ignores stops once its sim pass starts.
          // In the async path, the FIRST scene to hit idle after
          // aggregate convergence sets `m_in_simulation_ = true`
          // when submitting its sim task — and from that moment on
          // every other scene that returns to idle would see
          // `should_stop()=false`, skip the scene_finished branch,
          // and fall through to the forward-submit path (blocking
          // forever at the spread gate behind whichever scene is
          // slowest).  Reading the atomic directly side-steps the
          // sim-guard so all 16 scenes correctly drain through the
          // sim pass.
          //
          // Cascade intermediate levels still funnel through this
          // branch but the `skip_simulation_pass` guard inside
          // transitions them to `done` immediately, so the drain
          // semantics carried by the stop-check below are
          // preserved for them.  Sentinel / api-file stops still
          // route to the `should_stop()` branch below (no sim pass,
          // straight to done) — pre-existing behaviour.
          const bool scene_finished =
              sp.current_iteration_index > last_iteration_index
              || sp.scene_converged || m_stop_requested_.load();
          if (scene_finished) {
            // Cascade intermediate levels skip the sim pass entirely:
            // jump straight to `done` so the orchestration loop reports
            // the scene as complete without dispatching a final forward
            // solve, write_out, or per-cell parquet emission.  See the
            // sync path's `skip_simulation_pass` guard around the
            // analogous block for the matching architectural argument.
            if (m_options_.skip_simulation_pass) {
              sp.state = SceneState::done;
              break;
            }
            // Capture this scene's post-training cut count NOW (before
            // the sim pass starts adding any feasibility cuts) so the
            // tail discard at the end of solve_async knows where to
            // truncate.  See the `sim_before_scene_sizes` comment above
            // for the rationale — capturing at function start (the
            // pre-fix behaviour) gave every scene a snapshot of 0 and
            // the discard then wiped every backward Optimality cut.
            sim_before_scene_sizes[static_cast<std::size_t>(scene)] =
                m_cut_store_.at(scene).size();
            // Submit simulation forward pass for this scene.
            //
            // Ordering is `Medium` like every other SDDP task — the
            // `iteration_index` in the `SDDPTaskKey` tuple already gives
            // older iterations strict precedence under the lexicographic
            // comparator, so a sim task at iter N+k is never scheduled
            // ahead of a training task at iter N+1.  The sim task's *only*
            // special need was to bypass the CPU-saturation gate: under
            // sustained 100 % CPU a gated sim task at the queue head parks
            // every worker on `cv_.wait`, blocking lower-priority chunk
            // tasks underneath that *could* run (the juan/IPLP scene-12
            // wedge, 2026-05-16, gtopt_142.log: 6 min stall at 0 % CPU).
            // Historically that need was met with `TaskPriority::High`,
            // which also reordered sim ahead of same-iteration training
            // peers (the overload trap).  We now express it with the
            // independent `gate_bypass` flag: Medium ordering + CPU-gate
            // bypass, no reordering side effect.
            m_in_simulation_ = true;
            const auto sim_iteration_index = sp.current_iteration_index;
            const BasicTaskRequirements<SDDPTaskKey> sim_req {
                .priority = TaskPriority::Medium,
                .priority_key = make_sddp_task_key(sim_iteration_index,
                                                   SDDPPassDirection::forward,
                                                   SDDPTaskKind::lp),
                .gate_bypass = true,
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

          // ── Stop check BEFORE spread gate ──
          //
          // The spread gate is a TRAINING optimization: it prevents
          // any single scene from running too far ahead of the
          // slowest active scene so cuts stay broadly aligned.
          // Under ``m_stop_requested_`` we are NOT training anymore —
          // we're winding down, and every idle scene should
          // transition to ``done`` so the outer ``while(!all_done)``
          // loop can exit.
          //
          // Previous ordering (spread check first) had a deadlock:
          // when aggregate-convergence fires with N scenes in
          // ``backward_running`` and the cancellation cascade leaves
          // some scene's ``m_scene_completed_iter_`` lagging the
          // fast scenes (e.g. iter 25 vs 33), the spread gate
          // ``current_iter > min + max_spread`` blocks every fast
          // scene from transitioning past the gate.  ``should_stop``
          // is below the gate, so it never fires for those scenes.
          // The work pool drains to 0/0/Done=N, the main thread
          // parks in the 1 ms sleep at the bottom of the loop,
          // ``all_done`` never flips.  Observed on juan/IPLP L1
          // uninodal twice (2026-05-15 sessions).
          //
          // Moving the stop check first guarantees a stop signal
          // drains every idle scene to ``done`` in O(N_scenes) loop
          // iterations regardless of tracker state.
          if (should_stop()) {
            sp.state = SceneState::done;
            break;
          }

          // Spread limit: don't advance too far ahead of slowest
          // non-converged scene.  When max_spread >= max_iterations this
          // is effectively unlimited — scenes never wait.
          //
          // Use ``min_completed_iteration_among_active()`` (NOT the
          // global ``min_completed_iteration()``) because converged
          // scenes have their ``m_scene_completed_iter_`` frozen at
          // the convergence iteration.  Including them in the min
          // pinned the gate at the convergence iter (e.g. 51) and
          // permanently blocked active scenes from advancing past
          // ``min + max_spread`` (= 53 with default spread=2).  The
          // active scenes then ``break`` out of the dispatch loop
          // forever, leaving the outer ``while(!all_done)`` spinning
          // with no work in flight.  Observed on juan/IPLP hs8: 6
          // wettest scenes converged at iter 51, 9 dry scenes wedged
          // at iter 54 against a frozen ``min=51``.
          const auto min_iteration_index =
              tracker.min_completed_iteration_among_active();
          if (min_iteration_index >= m_iteration_offset_
              && sp.current_iteration_index
                  > min_iteration_index + IterationIndex {max_spread})
          {
            break;  // wait for slow scenes to catch up
          }

          // Submit forward pass
          m_benders_cut_.reset_infeasible_cut_count();
          const BasicTaskRequirements<SDDPTaskKey> fwd_req {
              .priority = TaskPriority::Medium,
              .priority_key = make_sddp_task_key(sp.current_iteration_index,
                                                 SDDPPassDirection::forward,
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
                        sp.current_iteration_index,
                        uid_of(scene),
                        fwd.error().message);
            sp.feasible = false;
            sp.upper_bound = 0.0;
          } else {
            // Per-scene UB = Σ forward_objective + α-rebase offset.
            // The offset is zero unless
            // `SDDPOptions::boundary_cuts_mean_shift` was enabled
            // (see `m_scene_alpha_offsets_` and the install pass in
            // `source/sddp_boundary_cuts.cpp`).  Without this term,
            // shifted boundary cuts would silently push UB lower by
            // `c̄_scene` and break the UB ↔ LB symmetry across the
            // mean-shift flag.
            sp.upper_bound = *fwd + scene_alpha_offset(scene);
            sp.feasible = true;
          }

          // Submit backward pass (only if feasible)
          if (sp.feasible) {
            const BasicTaskRequirements<SDDPTaskKey> bwd_req {
                .priority = TaskPriority::Medium,
                .priority_key = make_sddp_task_key(sp.current_iteration_index,
                                                   SDDPPassDirection::backward,
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
                        // Async/cascade path runs the scene driver ON a pool
                        // worker, so keep aperture chunks + slot-release on
                        // the pool (exec_pool = m_pool_).
                        return backward_pass_with_apertures(
                            scene, bwd_opts, iteration_index, m_pool_);
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
                        sp.current_iteration_index,
                        uid_of(scene),
                        bwd.error().message);
          } else {
            // Accumulate this scene's per-iter Benders cut count into
            // the iter slot.  When the aggregate iteration result is
            // finalized below (around the `tracker.all_complete(...)`
            // gate), the slot is read into `ir.cuts_added` and reset.
            const auto rel = iteration_relative(sp.current_iteration_index,
                                                m_iteration_offset_);
            if (rel >= 0 && std::cmp_less(rel, async_cuts_per_iter.size())) {
              async_cuts_per_iter[static_cast<std::size_t>(rel)] += *bwd;
            }
          }

          // Get lower bound from phase-0 objective in PHYSICAL ($)
          // units — must match the physical-space upper-bound
          // aggregated in sddp_forward_pass (which uses
          // ``get_obj_value()``).  Reading LP-raw here put
          // UB and LB in different unit spaces, producing a
          // permanent ``≈ scale_objective − 1`` / ``scale_objective``
          // gap that never closed regardless of cut quality.
          // Per-scene LB = master.get_obj_value() + α-rebase offset.
          // Mirrors the UB adjustment above: when the mean-shift
          // opt-in fired, the master LP's `get_obj_value()` is short
          // by `c̄_scene`, so we add it back here for display
          // symmetry.  `scene_alpha_offset()` returns 0 when no
          // offset applies.
          sp.lower_bound = planning_lp()
                               .system(scene, first_phase_index())
                               .linear_interface()
                               .get_obj_value()
              + scene_alpha_offset(scene);

          tracker.report_complete(scene,
                                  sp.current_iteration_index,
                                  sp.upper_bound,
                                  sp.lower_bound,
                                  sp.feasible);

          {
            const double scene_gap =
                compute_convergence_gap(sp.upper_bound, sp.lower_bound);
            SPDLOG_INFO("{}: completed  ub={}  lb={}  gap={}",
                        sddp_log("Async",
                                 gtopt::uid_of(sp.current_iteration_index),
                                 uid_of(scene)),
                        gtopt::log::money(sp.upper_bound),
                        gtopt::log::money(sp.lower_bound),
                        gtopt::log::pct(scene_gap));
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
            SPDLOG_WARN("{}: simulation failed: {}",
                        sddp_log("Sim",
                                 gtopt::uid_of(sp.current_iteration_index),
                                 uid_of(scene)),
                        sim.error().message);
          } else {
            // Simulation pass UB also picks up the α-rebase offset
            // (zero unless `boundary_cuts_mean_shift` is enabled).
            // Same rationale as the iteration UB site above.
            sp.upper_bound = *sim + scene_alpha_offset(scene);
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
            // Note: ``ub``/``lb``/``gap`` are intentionally NOT repeated
            // here — they are already emitted by the per-scene "completed"
            // line above (state ``iteration_running -> done``).  This
            // sim-done line fires from a different state transition
            // (``simulation_running -> idle``) and only needs to carry
            // the fields that are unique to the sim-pass context:
            // per-scene convergence flag, relative iteration count, and
            // the active-scene counter for pool sizing.
            SPDLOG_INFO("{}: scene done — converged={} iters={} active={}/{}",
                        sddp_log("Async",
                                 gtopt::uid_of(sp.current_iteration_index),
                                 uid_of(scene)),
                        sp.scene_converged,
                        iteration_relative(sp.current_iteration_index,
                                           m_iteration_offset_),
                        scenes_still_active,
                        num_scenes);
          }

          // Mid-run snapshot drop — POST-sim-pass.  At this point
          // `run_scene_simulation` has finished: forward_pass solved
          // every phase with crossover but output writing is deferred
          // to the final `PlanningLP::write_out()` flush (see the
          // "Output is NOT written here" comment above — the forward
          // solve leaves each cell's primal/dual cache retained for the
          // chunked post-solve pool).  No further consumer of this
          // scene's flat-LP snapshot exists, so we can drop it here.
          //
          // Dropping here recovers the savings the previous reverted
          // commit (04638b53) tried for at convergence-time but
          // crashed cascade because the sim-pass forward solve still
          // needed a live snapshot.  Now we drop strictly AFTER the
          // sim solve has consumed it.
          if (sim.has_value()
              && m_options_.low_memory_mode != LowMemoryMode::off)
          {
            for (const auto pi : iota_range<PhaseIndex>(0, num_phases)) {
              auto& post_sys = planning_lp().system(scene, pi);
              post_sys.linear_interface().clear_snapshot();
            }
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

      // Drain the per-iter cut accumulator into the iter result.  The
      // sync path sets this from `run_backward_pass_all_scenes`; the
      // async path summed per-scene `bwd_future.get()` returns into
      // `async_cuts_per_iter` as scenes completed.  Without this assign
      // every async iteration silently reported `cuts_added=0`, which
      // (a) made cascade `new_cuts=...` log meaningless on async-eligible
      // runs and (b) caused `CascadeLevelStats::cuts_added` to be 0
      // even when hundreds of optimality cuts had been installed in
      // the LP — confirmed by trace logs (`active_cuts ≥ 1`,
      // `save_cuts_for_iteration` running each iter) on juan-scale.
      {
        const auto rel = iteration_relative(next_converge_iteration_index,
                                            m_iteration_offset_);
        if (rel >= 0 && std::cmp_less(rel, async_cuts_per_iter.size())) {
          ir.cuts_added = async_cuts_per_iter[static_cast<std::size_t>(rel)];
        }
      }

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
      // Mirror of the synchronous ``finalize_iteration_result`` rewrite
      // (sddp_method_iteration.cpp, 2026-05-14): the primary
      // gap-vs-tol exit is gone.  ΔUB stationarity is the sole
      // convergence trigger, evaluated below via
      // ``evaluate_stationary_check``; the ``|gap| <
      // stationary_gap_ceiling`` gate was removed (2026-06-16).

      // Stationary gap check — uses the same ``evaluate_stationary_check``
      // helper as the synchronous ``iterate()`` path so the
      // convergence semantics (UB-stationarity only) stay in lockstep.
      // ``ir.gap_change`` is recomputed
      // here (the sync path delegates to ``finalize_iteration_result``
      // upstream) because the async aggregate result is assembled
      // inline; everything after that delegates to the shared helper.
      //
      // Cross-level seed: on iter 1 of a non-first cascade level the
      // in-level ``results`` vector is empty (per-level reset).
      // When :func:`SDDPMethod::seed_prior_bounds` has been called by
      // the cascade orchestrator with the last N iters of the
      // previous level (oldest-first), walk a combined index over
      // ``[seed ⧺ results]`` so a ``stationary_window=N``
      // configuration actually exercises an N-iter lookback at iter 1
      // — previously the seed was a single point and any window > 1
      // silently degraded to a 1-iter lookback at every cascade
      // transition.  Pair with ``min_iterations >= 2`` to avoid
      // converging instantly when the inherited envelope nearly
      // matches the previous level's UB (see the sync companion at
      // :func:`SDDPMethod::finalize_iteration_result`).
      if (m_options_.stationary_tol > 0.0 && m_options_.stationary_window > 0) {
        const auto window =
            static_cast<std::size_t>(m_options_.stationary_window);
        const std::size_t seed_n = m_seed_prior_history_.size();
        const std::size_t avail = seed_n + results.size();
        const std::size_t lookback = std::min(window, avail);
        if (lookback > 0) {
          // Index over the combined sequence, counted from the
          // OLDEST entry.
          const std::size_t pos = avail - lookback;
          const double old_ub = (pos < seed_n)
              ? m_seed_prior_history_[pos].upper_bound
              : results[pos - seed_n].upper_bound;
          ir.gap_change = std::abs(ir.upper_bound - old_ub)
              / std::max(1e-10, std::abs(old_ub));

          const auto stationary =
              evaluate_stationary_check(ir,
                                        next_converge_iteration_index,
                                        m_iteration_offset_,
                                        results.size(),
                                        m_options_);
          if (!ir.converged && stationary.converged()) {
            ir.converged = true;
            ir.stationary_converged = true;
          }
        }
      }

      // Async-only fall-through: when every scene has already been
      // marked converged by the per-scene tracker (scene_gap < tol
      // for ≥ min_iterations — see ``check_scene_convergence`` at
      // line ~1304), the aggregate IS converged regardless of how
      // many aggregate results we have collected.  Without this
      // propagation the async path could only set ``ir.converged``
      // via the stationary check, which needs ``!results.empty()``;
      // when all scenes converge on the very first aggregate snapshot
      // (e.g. the 2-scene 3-phase tests in test_sddp_async.cpp),
      // the stationary block is skipped and ``ir.converged`` would
      // never flip.  The per-scene tracker is the async equivalent
      // of the synchronous stationary check for scheduler-driven
      // early-exit; treating its terminal state as aggregate
      // convergence keeps the two dispatch paths semantically
      // aligned.
      if (!ir.converged && tracker.all_converged()) {
        ir.converged = true;
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

      // Log aggregate with pool stats.  Append the cumulative worst
      // kappa so the async-mode iteration headline carries the same
      // conditioning telemetry as the synchronous path above.
      //
      // Three-line block mirroring the synchronous path:
      //   ─── iter K (async) ───  elapsed=…  ETA=…
      //     iter K  obj   UB=…  LB=…  gap=…  Δgap=…  kappa=…
      //     iter K  scenes  spread=[a,b]  conv=K/N  pool(active=… pending=…)
      // Each line carries `iter K` for grep-friendliness; the second
      // line uses `(async)` so tail readers can tell a sync run from
      // an async run at a glance.
      {
        const auto pool_stats = pool.get_statistics();
        // Per-iter kappa — see comment at the sync iterate() site at
        // line ~678.  Same rationale: no stale propagation across
        // iters when CPLEX returns nullopt.  Async path uses the
        // loop-local ``next_converge_iteration_index`` (the iter id
        // about to be reported in this log block).
        const auto gk_async =
            current_iter_max_kappa(next_converge_iteration_index);
        const auto kappa_clause_async = (gk_async >= 0.0)
            ? std::format("  kappa={:.2e}", gk_async)
            : std::string {};
        const auto iter_id_async = next_converge_iteration_index;
        const auto elapsed_async_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now()
                                          - solve_start)
                .count();
        const auto avg_iter_async_s =
            elapsed_async_s / std::max(1, iter_id_async + 1);
        const auto max_iters_async = m_options_.max_iterations;
        const auto remaining_async =
            (max_iters_async > 0
             && iter_id_async + 1 < static_cast<int>(max_iters_async))
            ? static_cast<int>(max_iters_async) - 1 - iter_id_async
            : 0;
        const auto eta_async_s = (remaining_async > 0 && !ir.converged)
            ? remaining_async * avg_iter_async_s
            : 0.0;
        const auto eta_clause_async = (eta_async_s > 0.0)
            ? std::format("  ETA={}", gtopt::log::dur(eta_async_s))
            : std::string {};
        const auto conv_clause_async =
            ir.converged ? std::string {"  [CONVERGED]"} : std::string {};
        SPDLOG_INFO("─── iter {} (async) ───  elapsed={}{}{}",
                    iter_id_async,
                    gtopt::log::dur(elapsed_async_s),
                    eta_clause_async,
                    conv_clause_async);
        SPDLOG_INFO("    iter {}  obj     ub={}  lb={}  gap={}  Δgap={}{}",
                    iter_id_async,
                    gtopt::log::money(ir.upper_bound),
                    gtopt::log::money(ir.lower_bound),
                    gtopt::log::pct(ir.gap),
                    gtopt::log::pct(ir.gap_change),
                    kappa_clause_async);
        SPDLOG_INFO(
            "    iter {}  scenes  spread=[{},{}]  conv={}/{}  "
            "pool(active={} pending={} cpu={:.0f}%)",
            iter_id_async,
            tracker.min_completed_iteration(),
            tracker.max_completed_iteration(),
            tracker.num_converged(),
            num_scenes,
            pool_stats.tasks_active,
            pool_stats.tasks_pending,
            pool_stats.current_cpu_load);
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

    // Aggregate convergence (e.g. stationary / gap-based) fires on the
    // last finalized iter result.  When `ir.converged` is set there, the
    // training phase has nothing left to learn — every additional iter
    // adds redundant cuts that will not tighten LB or UB.  Trigger the
    // global stop so the orchestration loop transitions every active
    // scene to `simulation_running` (or `done` under
    // `skip_simulation_pass`) instead of grinding all scenes to
    // `max_iterations`.  Mirrors the synchronous `iterate()` path,
    // which breaks out of its iter loop on `ir.converged` at
    // `sddp_iteration.cpp:810-812`.  Without this signal-routing,
    // stationary convergence on the aggregate iter was invisible to
    // the async loop and the run silently burned its full iter budget
    // (observed on juan/iplp_plain: [CONVERGED] flagged at iter 15 of
    // 30, level continued to iter 30 anyway).
    //
    // Fire the INFO log + stop request EXACTLY ONCE — the
    // `m_stop_requested_` load guard skips this branch on every
    // subsequent orchestration-loop iteration (otherwise the log
    // line spammed thousands of times while the remaining scenes
    // drained).
    if (!all_done && !results.empty() && results.back().converged
        && !m_stop_requested_.load())
    {
      SPDLOG_INFO(
          "SDDP Async [i{}]: aggregate convergence reached — "
          "stopping training loop, transitioning remaining scenes",
          gtopt::uid_of(results.back().iteration_index));
      m_stop_requested_.store(true);
      // Snapshot per-scene cut counts AT the stop boundary so any
      // cuts added by in-flight tasks that complete after this
      // point during the drain pass can be truncated deterministically.
      // Without this, the inherited cut count at the next cascade
      // level depends on the pool's drain timing race.
      auto& scene_cuts_at_stop = m_cut_store_.scene_cuts();
      for (std::size_t si_sz = 0; si_sz < scene_cuts_at_stop.size()
           && si_sz < stop_before_scene_sizes.size();
           ++si_sz)
      {
        const auto si = SceneIndex {static_cast<Index>(si_sz)};
        stop_before_scene_sizes[si_sz] = scene_cuts_at_stop[si].size();
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

  // Discard simulation feasibility cuts (same as synchronous path).
  // `sim_before_scene_sizes[si]` carries the post-training cut count
  // captured when that scene transitioned into the sim pass; the
  // sentinel `size_t::max()` marks "this scene never reached the sim
  // pass" (e.g. `skip_simulation_pass=true`, or the run was cancelled
  // mid-orchestration), and we leave its cut store alone.
  if (!m_options_.save_simulation_cuts) {
    std::size_t dropped = 0;
    auto& scene_cuts = m_cut_store_.scene_cuts();
    for (std::size_t si_sz = 0;
         si_sz < scene_cuts.size() && si_sz < sim_before_scene_sizes.size();
         ++si_sz)
    {
      const auto si = SceneIndex {static_cast<Index>(si_sz)};
      const auto pre = sim_before_scene_sizes[si_sz];
      if (pre == std::numeric_limits<std::size_t>::max()) {
        continue;  // scene never reached the sim pass
      }
      if (scene_cuts[si].size() > pre) {
        dropped += scene_cuts[si].size() - pre;
        scene_cuts[si].resize(pre);
      }
    }
    if (dropped > 0) {
      SPDLOG_INFO("SDDP: discarding {} simulation feasibility cut(s)", dropped);
    }
  }

  // Discard RACE-CONDITION training cuts under `cut_drain_mode`:
  //   * `count`     — snapshot per-scene cut count at the stop boundary
  //                   (legacy; asymmetric — faster scenes keep their
  //                   iter-(N+1) head-start cuts; the asymmetry is
  //                   bounded by `max_async_spread`).
  //   * `iteration` — drop any cut whose `iteration_index` is greater
  //                   than the aggregate-certified iter (the last
  //                   iter for which `tracker.all_complete()` fired).
  //                   Symmetric across scenes; bit-for-bit
  //                   reproducible regardless of pool drain timing.
  //                   Default.
  //   * `all`       — no truncation; keep every cut currently in the
  //                   store.  Trades determinism for maximum
  //                   cut retention into the next cascade level.
  //
  // Same sentinel under `count`: `size_t::max()` marks "stop never
  // fired for this scene" (level ran to `max_iters` without aggregate
  // convergence) — no truncation in that case.
  {
    const auto mode = m_options_.cut_drain_mode;
    std::size_t dropped = 0;
    auto& scene_cuts = m_cut_store_.scene_cuts();

    if (mode == CutDrainMode::count) {
      for (std::size_t si_sz = 0;
           si_sz < scene_cuts.size() && si_sz < stop_before_scene_sizes.size();
           ++si_sz)
      {
        const auto si = SceneIndex {static_cast<Index>(si_sz)};
        const auto pre = stop_before_scene_sizes[si_sz];
        if (pre == std::numeric_limits<std::size_t>::max()) {
          continue;  // stop never fired — keep everything
        }
        if (scene_cuts[si].size() > pre) {
          dropped += scene_cuts[si].size() - pre;
          scene_cuts[si].resize(pre);
        }
      }
    } else if (mode == CutDrainMode::iteration) {
      // The aggregate-certified iter is the iter index of the last
      // entry pushed to `results` while `!m_stop_requested_` (every
      // `tracker.all_complete(...)`-finalized iter pushes one entry).
      // Use `m_iteration_offset_` as a floor when results is empty
      // (no training iters ran — e.g. the level was hot-started past
      // its own max_iterations).
      const auto certified_iter = results.empty()
          ? m_iteration_offset_
          : results.back().iteration_index;
      for (std::size_t si_sz = 0; si_sz < scene_cuts.size(); ++si_sz) {
        const auto si = SceneIndex {static_cast<Index>(si_sz)};
        // `SceneCutStore::cuts()` exposes the underlying
        // `std::vector<StoredCut>&` — use it directly so
        // `std::erase_if` matches the vector overload.
        auto& vec = scene_cuts[si].cuts();
        const auto orig = vec.size();
        std::erase_if(vec,
                      [certified_iter](const auto& c)
                      { return c.iteration_index > certified_iter; });
        dropped += orig - vec.size();
      }
    }
    // mode == all → no truncation, dropped stays at 0.

    if (dropped > 0) {
      SPDLOG_INFO(
          "SDDP: discarding {} race-condition training cut(s) "
          "(post-aggregate-convergence drain, mode={})",
          dropped,
          enum_name(mode));
    }
  }

  // Per-step timing trace for the iterate() tail.  Profile on
  // `cases/sddp_hydro_3phase` showed a ~296 ms gap between the last
  // SDDP iteration log line and the pool destructor on a 0.5-s wall
  // run — most of the wall, none of it inside the work-pool's
  // shutdown (verified at <2 ms via the per-pool shutdown trace
  // landed in d2e1a364).  These traces partition that gap into
  // cut-persistence / monitor.stop / lp_debug drain / pool reset
  // / planning_lp tail so a future profiler can localise where
  // the time actually goes.
  using TailClock = std::chrono::steady_clock;
  const auto t_tail_begin = TailClock::now();
  const auto tail_elapsed_ms =
      [](TailClock::time_point a, TailClock::time_point b)
  { return std::chrono::duration<double, std::milli>(b - a).count(); };

  // ── Cut persistence ──
  if (!m_options_.cuts_output_file.empty() && !results.empty()) {
    const auto num_scenes_final = planning_lp().simulation().scene_count();
    std::vector<uint8_t> final_feasible(
        static_cast<std::size_t>(num_scenes_final), 1U);
    save_cuts_for_iteration(results.back().iteration_index, final_feasible);

    const auto mode = m_options_.cut_recovery_mode;
    if (mode == HotStartMode::append) {
      const auto cuts_to_append = build_combined_cuts();
      auto result = save_cuts_parquet(cuts_to_append,
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
  const auto t_after_cuts = TailClock::now();

  monitor.stop();
  const auto t_after_monitor = TailClock::now();

  m_lp_debug_writer_.drain();
  const auto t_after_drain = TailClock::now();

  m_benders_cut_.set_pool(nullptr);
  m_pool_ = nullptr;
  m_solver_tier_.set_pool(nullptr);
  m_aux_pool_ = nullptr;
  m_lp_debug_writer_ = {};
  const auto t_after_reset = TailClock::now();

  spdlog::trace(
      "SDDP iterate() tail: cuts_persistence={:.1f}ms monitor.stop={:.1f}ms "
      "lp_debug.drain={:.1f}ms pool_reset+lp_debug_dtor={:.1f}ms",
      tail_elapsed_ms(t_tail_begin, t_after_cuts),
      tail_elapsed_ms(t_after_cuts, t_after_monitor),
      tail_elapsed_ms(t_after_monitor, t_after_drain),
      tail_elapsed_ms(t_after_drain, t_after_reset));

  // Surface α / varphi_s + the rebase constant c̄ on any FutureCost element so
  // they are written to the solution (async path); read-only w.r.t. the LP.
  populate_future_cost_output();

  return results;
}

}  // namespace gtopt
