/**
 * @file      sddp_pool.hpp
 * @brief     SDDP-specialised work pool: SDDPTaskKey, SDDPWorkPool, and factory
 * @date      2026-03-14
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This header provides the work-pool specialisation used by the SDDP solver:
 *
 *  - `SDDPTaskKey` – a 3-tuple `(iteration, signed_phase, kind)` used as the
 *    secondary sort key so that LP solves in the earliest iteration, with
 *    correct per-direction phase ordering, are always scheduled first.
 *  - `SDDPPassDirection` – enum class with values `forward = +1` and
 *    `backward = -1` so the enum value itself acts as the phase-sign
 *    multiplier when computing the priority key.
 *  - `SDDPTaskKind` – enum class (lp / non_lp) for task kind.
 *  - `make_sddp_task_key()` – factory from strong types to SDDPTaskKey tuple.
 *    Computes `signed_phase = direction * phase`, encoding both direction
 *    and per-direction phase order in a single signed int.
 *  - `SDDPWorkPool` – a concrete `BasicWorkPool<SDDPTaskKey>` subclass that
 *    can be forward-declared as `class SDDPWorkPool;` in other headers.
 *  - `make_sddp_work_pool()` – a factory that creates, configures, and starts
 *    an `SDDPWorkPool` with sensible CPU-aware defaults.
 *
 * ## SDDPTaskKey semantics
 *
 * The tuple `(iteration_index, signed_phase, kind)`:
 *  - `iteration_index`: SDDP iteration number (0, 1, …)
 *  - `signed_phase`:    `direction * phase` (`forward(+1) * phase = +phase`,
 *                       `backward(-1) * phase = -phase`)
 *  - `kind`:            `lp` (0) = LP solve/resolve, `non_lp` (1) = other
 *
 * With the default `std::less<SDDPTaskKey>` lexicographic comparison
 * (smaller tuple → higher dequeue priority):
 *  - **Lower iteration** wins (iter 0 before iter 1, …).
 *  - **Within forward**: smaller `signed_phase` wins, so phase 0 dequeues
 *    before phase N — earlier phase first, matching how the forward pass
 *    walks 0 → N.
 *  - **Within backward**: `signed_phase = -phase`, so `-N < … < -1 < 0` —
 *    phase N dequeues before phase 0, matching how the backward pass walks
 *    N → 0.
 *  - **Cross-direction collisions** at the same iteration are degenerate
 *    and resolved by `kind` / submit time.  In practice the SDDP scheduler
 *    runs the forward and backward passes serially within one iteration
 *    (`sddp_method_iteration.cpp` blocks on `fut.wait()` between them),
 *    so the queue never contains both directions at once.
 *  - **`lp` (0) wins over `non_lp` (1)** so write_lp / dump tasks never
 *    block a solve worker.
 */

#pragma once

#include <cmath>
#include <memory>
#include <thread>
#include <tuple>

#include <gtopt/hardware_info.hpp>
#include <gtopt/phase.hpp>
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
/// The enum value doubles as the phase-sign multiplier in
/// `make_sddp_task_key`: `forward(+1) * phase = +phase` (so phase 0 dequeues
/// first within forward), `backward(-1) * phase = -phase` (so phase N
/// dequeues first within backward, matching the N → 0 walk).
// NOLINTBEGIN(performance-enum-size) — int matches SDDPTaskKey tuple element
// type
enum class SDDPPassDirection : int
{
  forward = 1,
  backward = -1,
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
/// A 3-tuple `(iteration_index, signed_phase, kind)`:
///  - `IterationIndex`: SDDP iteration number (0, 1, …)
///  - `int signed_phase`: `direction * phase` (positive for forward,
///    non-positive for backward) — encodes both pass direction and the
///    correct per-direction phase ordering in a single signed value.
///  - `SDDPTaskKind`: `lp` (0) or `non_lp` (1)
///
/// Tuple comparison with `std::less<SDDPTaskKey>` (lexicographic, smaller
/// → higher dequeue priority):
///   - lower iteration  → higher priority
///   - within forward   (signed_phase ≥ 0): phase 0 < phase 1 < …
///   - within backward  (signed_phase ≤ 0): -N < … < -1 < 0
///   - lp (0) < non_lp (1)
using SDDPTaskKey =
    std::tuple<IterationIndex, int /*signed_phase*/, SDDPTaskKind>;

/// @brief Build an SDDPTaskKey from strongly-typed SDDP parameters.
///
/// Computes `signed_phase = direction * phase`.  The
/// `SDDPPassDirection::{forward = +1, backward = -1}` enum values are
/// chosen so they multiply directly with `phase_index` to encode the
/// correct per-direction priority ordering.
///
/// @param iteration_index SDDP iteration index
/// @param direction       Forward or backward pass
/// @param phase_index     Phase index within the iteration
/// @param kind            LP solve or non-LP task
[[nodiscard]] constexpr auto make_sddp_task_key(IterationIndex iteration_index,
                                                SDDPPassDirection direction,
                                                PhaseIndex phase_index,
                                                SDDPTaskKind kind) noexcept
    -> SDDPTaskKey
{
  const int signed_phase =
      static_cast<int>(direction) * static_cast<int>(phase_index.value_of());
  return {iteration_index, signed_phase, kind};
}

// ─── Task-requirement builders (LP solves) ───────────────────────────────────

/// Build a `BasicTaskRequirements<SDDPTaskKey>` for a forward-pass LP solve.
///
/// Centralises the (priority, priority_key) tuple so the SDDP solver never
/// spells out the three-element key inline, and so the lexicographic
/// ordering invariant stays in one place:
///
///   iter N LP        < iter N+1 LP                (same direction, phase)
///   forward phase 0  < forward phase 1 < …        (within forward)
///   backward phase N < … < backward phase 0       (within backward)
///   any LP           < any non-LP                 (same iter, signed_phase)
///
/// Both forward and backward solves share `TaskPriority::Medium`; the
/// tuple key alone provides full SDDP ordering.  Exposed for unit
/// testing — the priority-key invariant is the contract every SDDP
/// scheduler caller relies on, and a regression here silently
/// reorders the solve graph.
[[nodiscard]] constexpr auto make_forward_lp_task_req(
    IterationIndex iteration_index, PhaseIndex phase_index) noexcept
    -> BasicTaskRequirements<SDDPTaskKey>
{
  return BasicTaskRequirements<SDDPTaskKey> {
      .priority = TaskPriority::Medium,
      .priority_key = make_sddp_task_key(iteration_index,
                                         SDDPPassDirection::forward,
                                         phase_index,
                                         SDDPTaskKind::lp),
      .name = {},
  };
}

/// Build a `BasicTaskRequirements<SDDPTaskKey>` for a backward-pass LP solve.
/// See `make_forward_lp_task_req` for the ordering invariant.
[[nodiscard]] constexpr auto make_backward_lp_task_req(
    IterationIndex iteration_index, PhaseIndex phase_index) noexcept
    -> BasicTaskRequirements<SDDPTaskKey>
{
  return BasicTaskRequirements<SDDPTaskKey> {
      .priority = TaskPriority::Medium,
      .priority_key = make_sddp_task_key(iteration_index,
                                         SDDPPassDirection::backward,
                                         phase_index,
                                         SDDPTaskKind::lp),
      .name = {},
  };
}

// ─── SDDPWorkPool
// ─────────────────────────────────────────────────────────────

/// @brief Work pool specialised for the SDDP solver with tuple priority key.
///
/// Uses `SDDPTaskKey = std::tuple<IterationIndex, int, SDDPTaskKind>` as the
/// secondary sort key with the default `std::less<SDDPTaskKey>` comparator
/// (lexicographic).  A concrete derived class so that `class SDDPWorkPool;`
/// can be forward-declared in headers that only need the pointer type.
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
 * (iteration, signed_phase, kind) with the default
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
