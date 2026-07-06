/**
 * @file      sddp_pool.hpp
 * @brief     SDDP-specialised work pool: SDDPTaskKey, SDDPWorkPool, and factory
 * @date      2026-03-14
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This header provides the work-pool specialisation used by the SDDP solver:
 *
 *  - `SDDPTaskKey` – a 4-tuple `(iteration, is_backward, phase_rank, kind)`
 *    used as the sort key so LP solves in the earliest iteration, with
 *    forward strictly ahead of backward at the same iteration, are always
 *    scheduled first; `phase_rank` is a non-negative laggard-first tie-break
 *    for per-phase backward aperture chunks (0 for phase-agnostic drivers).
 *  - `SDDPPassDirection` – enum class (`forward` / `backward`) used by
 *    callers to identify the pass; converted to `is_backward` (0 / 1)
 *    by the factory.
 *  - `SDDPTaskKind` – enum class (lp / non_lp) for task kind.
 *  - `make_sddp_task_key()` – factory from strong types to SDDPTaskKey tuple.
 *  - `SDDPWorkPool` – a concrete `BasicWorkPool<SDDPTaskKey>` subclass that
 *    can be forward-declared as `class SDDPWorkPool;` in other headers.
 *  - `make_sddp_work_pool()` – a factory that creates, configures, and starts
 *    an `SDDPWorkPool` with sensible CPU-aware defaults.
 *
 * ## SDDPTaskKey semantics
 *
 * The tuple `(iteration_index, is_backward, phase_rank, kind)`:
 *  - `iteration_index`: SDDP iteration number (0, 1, …)
 *  - `is_backward`:     0 = forward pass, 1 = backward pass
 *  - `phase_rank`:      non-negative laggard-first tie-break (0 = driver)
 *  - `kind`:            `lp` (0) = LP solve/resolve, `non_lp` (1) = other
 *
 * With the default `std::less<SDDPTaskKey>` lexicographic comparison
 * (smaller tuple → higher dequeue priority):
 *  - **Lower iteration** wins (iter 0 before iter 1, …) — so backward
 *    iter N still outranks forward iter N+1.
 *  - **Forward (0) beats backward (1)** at the same iteration.  Within
 *    a single iter, forward always runs *first* by definition (it
 *    produces the trial points the backward pass needs), so the only
 *    way the queue contains both at once is that one scene's forward
 *    is *late* while another scene's same-iter backward has already
 *    been submitted.  Draining the late forward first lets the slow
 *    scene catch up; flipping the order would widen the spread
 *    instead.
 *  - **Smaller `phase_rank`** wins among per-phase backward aperture
 *    chunks tied on `(iteration, is_backward)`.  The backward sweep
 *    visits phases N-1…1, so a scene at a smaller phase index has MORE
 *    phases left this iteration (the laggard); the chunk submitter sets
 *    `phase_rank = (n_phases-1) - phase` so that larger-remaining scene
 *    gets the smaller rank and drains first.  Scene-driver tasks run all
 *    their phases as one task and leave `phase_rank = 0`.  The field is
 *    kept NON-NEGATIVE so it can never reorder forward ahead of backward
 *    (which `-phase` would, by sorting before any positive rank).
 *  - **`lp` (0) wins over `non_lp` (1)** so write_lp / dump tasks never
 *    block a solve worker.
 *
 * `phase_rank` matters only in the async path: there two scenes can be in
 * the same iteration's backward sweep at different phases at once.  In the
 * synchronous coordinator path (the default) apertures run inline on each
 * scene's own driver thread, so chunks never funnel through this pool and
 * `phase_rank` is moot — but the key stays correct for the opt-in async /
 * cascade path and any future engine that schedules chunks here.
 */

#pragma once

#include <cmath>
#include <memory>
#include <thread>
#include <tuple>

#include <gtopt/hardware_info.hpp>
#include <gtopt/memory_monitor.hpp>
#include <gtopt/sddp_common.hpp>
#include <gtopt/work_pool.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif
#include <spdlog/spdlog.h>

namespace gtopt
{

// ─── SDDPTaskKey and named constants ─────────────────────────────────────────

/// @brief Pass direction for the SDDP scheduler.
///
/// Converted to the `is_backward` field of `SDDPTaskKey` by the factory
/// (`forward → 0`, `backward → 1`) so the lexicographic key tuple keeps
/// forward strictly ahead of backward at the same iteration.
// NOLINTNEXTLINE(performance-enum-size) — int matches SDDPTaskKey tuple element
enum class SDDPPassDirection : int
{
  forward = 0,
  backward = 1,
};

/// @brief Task kind for the `kind` field of `SDDPTaskKey`.
/// LP solves (0) have higher priority than non-LP tasks (1).
// NOLINTNEXTLINE(performance-enum-size) — int matches SDDPTaskKey tuple element
enum class SDDPTaskKind : int
{
  lp = 0,
  non_lp = 1,
};

/// @brief SDDP solver task priority key.
///
/// A 4-tuple `(iteration_index, is_backward, phase_rank, kind)`:
///  - `IterationIndex`: SDDP iteration number (0, 1, …)
///  - `int is_backward`: `0` for forward, `1` for backward.  Lexicographic
///    comparison keeps the forward pass strictly ahead of the backward
///    pass within the same iteration.
///  - `int phase_rank`: NON-NEGATIVE laggard-first tie-break for per-phase
///    backward aperture chunks (`(n_phases-1) - phase`); `0` for the
///    phase-agnostic scene-driver tasks (forward / backward / sim).
///  - `SDDPTaskKind`: `lp` (0) or `non_lp` (1)
///
/// Tuple comparison with `std::less<SDDPTaskKey>` (lexicographic, smaller
/// → higher dequeue priority):
///   - lower iteration   → higher priority
///   - forward (0)       → higher priority than backward (1)
///   - smaller phase_rank → higher priority (laggard scene's chunks first)
///   - lp (0) < non_lp (1)
///
/// `phase_rank` differentiates live tasks only in the async path, where
/// two scenes can be in the same iteration's backward sweep at different
/// phases at once.  Scene-driver submissions run all their phases as one
/// task (rank 0); only the backward aperture-chunk tasks set a positive
/// rank.  See the header comment block for the full rationale.
using SDDPTaskKey = std::tuple<IterationIndex,
                               int /*is_backward*/,
                               int /*phase_rank*/,
                               SDDPTaskKind>;

/// @brief Build an SDDPTaskKey from strongly-typed SDDP parameters.
///
/// Produces `(iteration_index, is_backward, phase_rank, kind)` where
/// `is_backward = (direction == backward) ? 1 : 0`.
///
/// `phase_rank` is a NON-NEGATIVE laggard-first tie-break used only by the
/// per-phase backward aperture-chunk tasks (see `make_aperture_submit_fn`):
/// in the async path two scenes can be in the same iteration's backward
/// sweep at different phases at once, and the scene with MORE phases still
/// to process (the laggard) should drain first.  Scene-driver tasks
/// (forward / backward / sim), which run all their phases internally as a
/// single pool task, leave it at the default `0` (highest within their
/// `(iteration, is_backward)` class).  Kept non-negative so it never inverts
/// the `is_backward` ordering above it in the tuple.
///
/// @param iteration_index SDDP iteration index
/// @param direction       Forward or backward pass
/// @param kind            LP solve or non-LP task
/// @param phase_rank      Laggard-first tie-break (0 = phase-agnostic driver)
[[nodiscard]] constexpr auto make_sddp_task_key(IterationIndex iteration_index,
                                                SDDPPassDirection direction,
                                                SDDPTaskKind kind,
                                                int phase_rank = 0) noexcept
    -> SDDPTaskKey
{
  const int is_backward = (direction == SDDPPassDirection::backward) ? 1 : 0;
  return {iteration_index, is_backward, phase_rank, kind};
}

// ─── Task-requirement builders (LP solves) ───────────────────────────────────

/// Build a `BasicTaskRequirements<SDDPTaskKey>` for a forward-pass LP solve.
///
/// Centralises the (priority, priority_key) tuple so the SDDP solver never
/// spells out the key inline, and so the lexicographic ordering invariant
/// stays in one place:
///
///   iter N LP   < iter N+1 LP   (same direction)
///   forward LP  < backward LP   (same iter)
///   any LP      < any non-LP    (same iter, direction)
///
/// Both forward and backward solves share `TaskPriority::Medium`; the
/// tuple key alone provides full SDDP ordering.  Exposed for unit
/// testing — the priority-key invariant is the contract every SDDP
/// scheduler caller relies on, and a regression here silently
/// reorders the solve graph.
[[nodiscard]] constexpr auto make_forward_lp_task_req(
    IterationIndex iteration_index) noexcept
    -> BasicTaskRequirements<SDDPTaskKey>
{
  return BasicTaskRequirements<SDDPTaskKey> {
      .priority = TaskPriority::Medium,
      .priority_key = make_sddp_task_key(
          iteration_index, SDDPPassDirection::forward, SDDPTaskKind::lp),
      .name = {},
  };
}

/// Build a `BasicTaskRequirements<SDDPTaskKey>` for a backward-pass LP solve.
/// See `make_forward_lp_task_req` for the ordering invariant.
[[nodiscard]] constexpr auto make_backward_lp_task_req(
    IterationIndex iteration_index) noexcept
    -> BasicTaskRequirements<SDDPTaskKey>
{
  return BasicTaskRequirements<SDDPTaskKey> {
      .priority = TaskPriority::Medium,
      .priority_key = make_sddp_task_key(
          iteration_index, SDDPPassDirection::backward, SDDPTaskKind::lp),
      .name = {},
  };
}

// ─── SDDPWorkPool
// ─────────────────────────────────────────────────────────────

/// @brief Work pool specialised for the SDDP solver with tuple priority key.
///
/// Uses `SDDPTaskKey = std::tuple<IterationIndex, int, SDDPTaskKind>`
/// (iteration, is_backward, kind) as the secondary sort key with the
/// default `std::less<SDDPTaskKey>` comparator (lexicographic).  A
/// concrete derived class so that `class SDDPWorkPool;` can be
/// forward-declared in headers that only need the pointer type.
class SDDPWorkPool final : public BasicWorkPool<SDDPTaskKey>
{
public:
  using BasicWorkPool<SDDPTaskKey>::BasicWorkPool;
};

// ─── Factory function
// ─────────────────────────────────────────────────────────

/**
 * @brief Create and start an SDDPWorkPool configured for the SDDP solver.
 *
 * Uses `SDDPTaskKey` (tuple) as the secondary priority key so that the
 * SDDP forward/backward LP solves are ordered by
 * (iteration, is_backward, kind) with the default
 * `std::less<SDDPTaskKey>` comparator (smaller tuple → higher priority).
 *
 * The pool sizes itself as
 * `cpu_factor × physical_concurrency() + cell_task_headroom`.
 * The headroom term compensates for a structural property of the
 * synchronised backward pass: per-scene cell tasks block on their
 * aperture sub-task futures while holding a worker slot.  With
 * `cell_task_headroom = num_feasible_scenes` the pool always has
 * `cpu_factor × physical_concurrency` slots free for aperture solves
 * regardless of how many cell tasks are mid-wait.  Without the
 * headroom, the cell-task block silently caps aperture parallelism
 * at `(cpu_factor × physical_concurrency) − num_scenes` (typically
 * ~80 % of the nominal cap on a 16-scene problem — observed on
 * juan/gtopt_iplp as `Active 36-48/80, Pending 0`, where the gap
 * between 80 and the active count is the cell-task block).
 *
 * The headroom workers are lazily spawned, so on iterations where the
 * cell-task block is short (e.g. backward pass without apertures)
 * they stay parked in `cv_.wait` and add no measurable overhead.
 *
 * @param cpu_factor          Over-commit factor applied to
 *                            `physical_concurrency()`.  Default 2.0.
 *                            Was 4.0 under the previous per-task
 *                            `std::async` design; with persistent
 *                            lazy-spawned workers the pool self-
 *                            regulates, so 2× is a better ceiling.
 * @param memory_limit_mb     Process RSS limit in MB (0 = no limit).
 * @param cell_task_headroom  Extra slots reserved for the synchronised
 *                            backward pass's per-scene cell tasks
 *                            (which block on aperture futures and would
 *                            otherwise reduce the aperture-parallelism
 *                            ceiling).  Pass the number of feasible
 *                            scenes for the run.  Default 0 preserves
 *                            prior behaviour for callers that have not
 *                            migrated.
 * @return A started SDDPWorkPool (heap-allocated, non-movable).
 */
[[nodiscard]] inline std::unique_ptr<SDDPWorkPool> make_sddp_work_pool(
    double cpu_factor = 2.0,
    double memory_limit_mb = 0.0,
    int cell_task_headroom = 0)
{
  WorkPoolConfig pool_config {};
  pool_config.name = "SDDPWorkPool";
  // The CPU-budget ceiling includes the cell-task headroom (extra slots
  // reserved for the synchronised backward pass's blocking cell tasks).
  const auto base_threads =
      static_cast<int>(std::lround(cpu_factor * physical_concurrency()));
  const int ceiling =
      std::max(1, base_threads + std::max(0, cell_task_headroom));

  // With a memory limit the pool starts at ONE thread and grows toward
  // `ceiling` under the live measured-memory controller (no fixed per-task
  // estimate); each forward/backward task reconstructs ~one cell's flat LP,
  // and the controller measures that marginal cost directly.  With no limit
  // it runs at the ceiling.  We feed the helper the *effective* cpu_factor
  // that reproduces `ceiling` (headroom folded in) so the ceiling is exact.
  const double effective_cpu_factor = static_cast<double>(ceiling)
      / std::max(1.0, static_cast<double>(physical_concurrency()));
  const auto clamp = memory_clamp_threads(
      effective_cpu_factor, memory_limit_mb, "SDDPWorkPool");
  pool_config.max_threads = clamp.initial_threads;
  pool_config.max_threads_ceiling = ceiling;
  pool_config.max_cpu_threshold =
      static_cast<int>(100.0 - (50.0 / static_cast<double>(ceiling)));

  // Memory governance: throttle on gtopt's OWN projected RSS, not on
  // system-wide usage.  The marginal-RSS dispatch gate
  // (`can_dispatch_task` gate #4 + `update_memory_model_`) measures this
  // pool's per-task footprint and admits up to a process-RSS budget — a
  // graduated, self-scoped throttle.  It only activates when
  // `max_process_rss_mb > 0`, so default the budget to a fraction of
  // physical RAM when the caller leaves it unset (was 0 = OFF, which left
  // ONLY the blunt system-wide gates below active).
  //
  // Why this matters: the previous default gated routine dispatch on
  // system `mem_pct >= 90%` / `free < 4 GB`.  Those count EVERY process
  // (an unrelated 17 GB clangd pushed the box over 90% and collapsed the
  // SDDP pool to a single worker) and, under `low_memory_mode=off`, cannot
  // be relieved by throttling at all — the 800 cell LPs stay resident
  // regardless of how many solve concurrently, so draining frees only
  // solver scratch.  Serializing to one core bought ~0 memory at ~20x the
  // latency.  Keying on gtopt's own RSS ignores external pressure and lets
  // the pool run at full concurrency whenever its own footprint (resident
  // LPs + marginal scratch) fits the budget.
  //
  // The system-wide gates are kept ONLY as near-OOM safety backstops
  // (97% / 1 GB free) so they fire just before the kernel OOM-killer, not
  // as a routine throttle.
  if (memory_limit_mb > 0.0) {
    pool_config.max_process_rss_mb = memory_limit_mb;
  } else {
    const auto total_mb = MemoryMonitor::get_system_memory_snapshot().total_mb;
    pool_config.max_process_rss_mb = total_mb > 0.0 ? 0.90 * total_mb : 0.0;
  }
  pool_config.max_memory_percent = 97.0;
  pool_config.min_free_memory_mb = 1024.0;

  auto pool = std::make_unique<SDDPWorkPool>(pool_config);
  pool->start();
  SPDLOG_INFO(
      "SDDP work pool started: max_threads={} (ceiling={} = base {} + "
      "cell_headroom {}) cpu_threshold={:.0f}%{} (physical_cores={} "
      "logical_cores={})",
      pool_config.max_threads,
      ceiling,
      base_threads,
      cell_task_headroom,
      static_cast<double>(pool_config.max_cpu_threshold),
      pool_config.max_process_rss_mb > 0
          ? std::format(" rss_budget={:.0f}MB{}",
                        pool_config.max_process_rss_mb,
                        memory_limit_mb > 0 ? "" : " (auto 0.90xRAM)")
          : "",
      physical_concurrency(),
      std::thread::hardware_concurrency());
  return pool;
}

}  // namespace gtopt
