/**
 * @file      sddp_pool.hpp
 * @brief     SDDP-specialised work pool: SDDPTaskKey, SDDPWorkPool, and factory
 * @date      2026-03-14
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This header provides the work-pool specialisation used by the SDDP solver:
 *
 *  - `SDDPTaskKey` – a 3-tuple `(iteration, is_backward, kind)` used as
 *    the secondary sort key so LP solves in the earliest iteration,
 *    with forward strictly ahead of backward at the same iteration,
 *    are always scheduled first.
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
 * The tuple `(iteration_index, is_backward, kind)`:
 *  - `iteration_index`: SDDP iteration number (0, 1, …)
 *  - `is_backward`:     0 = forward pass, 1 = backward pass
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
 *  - **`lp` (0) wins over `non_lp` (1)** so write_lp / dump tasks never
 *    block a solve worker.
 *
 * **No per-phase field**: every async forward/backward submission runs all
 * phases internally as one pool task, so per-phase priority was unused
 * dead code at the four top-level submission sites.  The sync
 * phase-by-phase backward path submits one task per phase but waits
 * between phases (only one phase's tasks ever in flight), so per-phase
 * priority would not change scheduling there either.  Aperture chunks
 * use the same key but compete only across scenes at the same phase
 * level (which ties on the key — FIFO).  Removed 2026-05.
 */

#pragma once

#include <cmath>
#include <memory>
#include <thread>
#include <tuple>

#include <gtopt/hardware_info.hpp>
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
// NOLINTBEGIN(performance-enum-size) — int matches SDDPTaskKey tuple element
// type
enum class SDDPPassDirection : int
{
  forward = 0,
  backward = 1,
};

/// @brief Task kind for the `kind` field of `SDDPTaskKey`.
/// LP solves (0) have higher priority than non-LP tasks (1).
enum class SDDPTaskKind : int
{
  lp = 0,
  non_lp = 1,
};
// NOLINTEND(performance-enum-size)

/// @brief SDDP solver task priority key.
///
/// A 3-tuple `(iteration_index, is_backward, kind)`:
///  - `IterationIndex`: SDDP iteration number (0, 1, …)
///  - `int is_backward`: `0` for forward, `1` for backward.  Lexicographic
///    comparison keeps the forward pass strictly ahead of the backward
///    pass within the same iteration.
///  - `SDDPTaskKind`: `lp` (0) or `non_lp` (1)
///
/// Tuple comparison with `std::less<SDDPTaskKey>` (lexicographic, smaller
/// → higher dequeue priority):
///   - lower iteration  → higher priority
///   - forward (0)      → higher priority than backward (1)
///   - lp (0) < non_lp (1)
///
/// **Per-phase ordering is not encoded** — every async forward/backward
/// submission runs all phases internally as one pool task, so a
/// per-phase priority field would never differentiate live tasks (the
/// sync phase-by-phase backward path also serialises phase steps with
/// futures, so only one phase's tasks are ever in flight there).  See
/// the header comment block for the full rationale.
using SDDPTaskKey =
    std::tuple<IterationIndex, int /*is_backward*/, SDDPTaskKind>;

/// @brief Build an SDDPTaskKey from strongly-typed SDDP parameters.
///
/// Produces `(iteration_index, is_backward, kind)` where
/// `is_backward = (direction == backward) ? 1 : 0`.
///
/// @param iteration_index SDDP iteration index
/// @param direction       Forward or backward pass
/// @param kind            LP solve or non-LP task
[[nodiscard]] constexpr auto make_sddp_task_key(IterationIndex iteration_index,
                                                SDDPPassDirection direction,
                                                SDDPTaskKind kind) noexcept
    -> SDDPTaskKey
{
  const int is_backward = (direction == SDDPPassDirection::backward) ? 1 : 0;
  return {iteration_index, is_backward, kind};
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
  const auto base_threads =
      static_cast<int>(std::lround(cpu_factor * physical_concurrency()));
  pool_config.max_threads = base_threads + std::max(0, cell_task_headroom);
  pool_config.max_cpu_threshold = static_cast<int>(
      100.0 - (50.0 / static_cast<double>(pool_config.max_threads)));
  pool_config.max_process_rss_mb = memory_limit_mb;

  auto pool = std::make_unique<SDDPWorkPool>(pool_config);
  pool->start();
  SPDLOG_INFO(
      "SDDP work pool started: max_threads={} (base={} + cell_headroom={}) "
      "cpu_threshold={:.0f}%{} (physical_cores={} logical_cores={})",
      pool_config.max_threads,
      base_threads,
      cell_task_headroom,
      static_cast<double>(pool_config.max_cpu_threshold),
      memory_limit_mb > 0
          ? std::format(" memory_limit={:.0f}MB", memory_limit_mb)
          : "",
      physical_concurrency(),
      std::thread::hardware_concurrency());
  return pool;
}

}  // namespace gtopt
