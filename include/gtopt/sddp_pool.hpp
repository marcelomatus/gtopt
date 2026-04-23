/**
 * @file      sddp_pool.hpp
 * @brief     SDDP-specialised work pool: SDDPTaskKey, SDDPWorkPool, and factory
 * @date      2026-03-14
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This header provides the work-pool specialisation used by the SDDP solver:
 *
 *  - `SDDPTaskKey` – a 4-tuple `(iteration, is_backward, phase, is_nonlp)`
 *    used as the secondary sort key so that forward LP solves in the earliest
 *    iteration are always scheduled before backward passes, later iterations,
 *    or non-LP tasks.
 *  - `SDDPPassDirection` – enum class (forward / backward) for pass direction.
 *  - `SDDPTaskKind` – enum class (lp / non_lp) for task kind.
 *  - `make_sddp_task_key()` – factory from strong types to SDDPTaskKey tuple.
 *  - `SDDPWorkPool` – a concrete `BasicWorkPool<SDDPTaskKey>` subclass that
 *    can be forward-declared as `class SDDPWorkPool;` in other headers.
 *  - `make_sddp_work_pool()` – a factory that creates, configures, and starts
 *    an `SDDPWorkPool` with sensible CPU-aware defaults.
 *
 * ## SDDPTaskKey semantics
 *
 * The tuple `(iteration_index, is_backward, phase_index, is_nonlp)`:
 *  - `iteration_index`: SDDP iteration number (0, 1, …)
 *  - `is_backward`:     0 = forward pass, 1 = backward pass
 *  - `phase_index`:     phase within the iteration (0, 1, …)
 *  - `is_nonlp`:        0 = LP solve/resolve, 1 = other (e.g. write_lp)
 *
 * With the default `std::less<SDDPTaskKey>` lexicographic comparison:
 *  - Lower iteration  → higher priority
 *  - Forward (0)      → higher priority than backward (1)
 *  - Lower phase      → higher priority
 *  - LP solve (0)     → higher priority than non-LP (1)
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

/// @brief Pass direction for the `is_backward` field of `SDDPTaskKey`.
/// Forward (0) has higher priority than backward (1) in lexicographic order.
// NOLINTBEGIN(performance-enum-size) — int matches SDDPTaskKey tuple element
// type
enum class SDDPPassDirection : int
{
  forward = 0,
  backward = 1,
};

/// @brief Task kind for the `is_nonlp` field of `SDDPTaskKey`.
/// LP solves (0) have higher priority than non-LP tasks (1).
enum class SDDPTaskKind : int
{
  lp = 0,
  non_lp = 1,
};
// NOLINTEND(performance-enum-size)

/// @brief SDDP solver task priority key.
///
/// A 4-tuple `(iteration_index, direction, phase_index, kind)` using the
/// project's strong types so that construction and inspection stay
/// type-safe end-to-end:
///  - `IterationIndex`:    SDDP iteration number (0, 1, …)
///  - `SDDPPassDirection`: `forward` (0) or `backward` (1)
///  - `PhaseIndex`:        phase within the iteration (0, 1, …)
///  - `SDDPTaskKind`:      `lp` (0) or `non_lp` (1)
///
/// Tuple comparison with `std::less<SDDPTaskKey>` remains lexicographic —
/// both strong indices and enum classes expose `operator<` that matches
/// their integer ordering, so lower iteration → forward → lower phase →
/// lp → higher execution priority.
using SDDPTaskKey =
    std::tuple<IterationIndex, SDDPPassDirection, PhaseIndex, SDDPTaskKind>;

/// @brief Build an SDDPTaskKey from strongly-typed SDDP parameters.
///
/// Preserves strong types through the tuple — no unwrapping needed.
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
  return {iteration_index, direction, phase_index, kind};
}

// ─── SDDPWorkPool
// ─────────────────────────────────────────────────────────────

/// @brief Work pool specialised for the SDDP solver with tuple priority key.
///
/// Uses `SDDPTaskKey = std::tuple<int,int,int,int>` as the secondary sort
/// key with the default `std::less<SDDPTaskKey>` comparator (lexicographic).
/// A concrete derived class so that `class SDDPWorkPool;` can be forward-
/// declared in headers that only need the pointer type.
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
 * (iteration, is_backward, phase, is_nonlp) with the default
 * `std::less<SDDPTaskKey>` comparator (smaller tuple → higher priority).
 *
 * @param cpu_factor        Over-commit factor applied to hardware_concurrency.
 *                          Default 4.0 — aperture tasks block on clone mutex,
 *                          so extra threads keep CPUs busy while others wait.
 * @param memory_limit_mb   Process RSS limit in MB (0 = no limit).
 * @return A started SDDPWorkPool (heap-allocated, non-movable).
 */
[[nodiscard]] inline std::unique_ptr<SDDPWorkPool> make_sddp_work_pool(
    double cpu_factor = 4.0, double memory_limit_mb = 0.0)
{
  WorkPoolConfig pool_config {};
  pool_config.name = "SDDPWorkPool";
  pool_config.max_threads =
      static_cast<int>(std::lround(cpu_factor * physical_concurrency()));
  pool_config.max_cpu_threshold = static_cast<int>(
      100.0 - (50.0 / static_cast<double>(pool_config.max_threads)));
  pool_config.max_process_rss_mb = memory_limit_mb;

  auto pool = std::make_unique<SDDPWorkPool>(pool_config);
  pool->start();
  SPDLOG_INFO(
      "SDDP work pool started: max_threads={} cpu_threshold={:.0f}%{} "
      "(physical_cores={} logical_cores={})",
      pool_config.max_threads,
      static_cast<double>(pool_config.max_cpu_threshold),
      memory_limit_mb > 0
          ? std::format(" memory_limit={:.0f}MB", memory_limit_mb)
          : "",
      physical_concurrency(),
      std::thread::hardware_concurrency());
  return pool;
}

}  // namespace gtopt
