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
 *  - `kSDDPKeyIsLP`, `kSDDPKeyIsNonLP`, `kSDDPKeyForward`, `kSDDPKeyBackward`
 *    – named constants for the individual tuple fields.
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

#include <gtopt/work_pool.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif
#include <spdlog/spdlog.h>

namespace gtopt
{

// ─── SDDPTaskKey and named constants ─────────────────────────────────────────

/// @brief SDDP solver task priority key.
///
/// A 4-tuple `(iteration_index, is_backward, phase_index, is_nonlp)`:
///  - `iteration_index`: SDDP iteration number (0, 1, …)
///  - `is_backward`:     0 = forward pass, 1 = backward pass
///  - `phase_index`:     phase within the iteration (0, 1, …)
///  - `is_nonlp`:        0 = LP solve/resolve, 1 = other (e.g. write_lp)
///
/// With the default `std::less<SDDPTaskKey>` comparator, the tuple is
/// compared lexicographically: lower iteration → lower `is_backward` →
/// lower phase → lower `is_nonlp` → higher execution priority.
using SDDPTaskKey = std::tuple<int, int, int, int>;

/// @brief Named constant for the `is_nonlp` field of `SDDPTaskKey`.
/// LP solve/resolve calls use 0; all other tasks (e.g. write_lp) use 1.
inline constexpr int kSDDPKeyIsLP = 0;
inline constexpr int kSDDPKeyIsNonLP = 1;

/// @brief Named constants for the `is_backward` field of `SDDPTaskKey`.
inline constexpr int kSDDPKeyForward = 0;
inline constexpr int kSDDPKeyBackward = 1;

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
 * @param cpu_factor  Over-commit factor applied to hardware_concurrency.
 *                    Default 1.25.
 * @return A started SDDPWorkPool (heap-allocated, non-movable).
 */
[[nodiscard]] inline std::unique_ptr<SDDPWorkPool> make_sddp_work_pool(
    double cpu_factor = 1.25)
{
  WorkPoolConfig pool_config {};
  pool_config.max_threads = static_cast<int>(
      std::lround(cpu_factor * std::thread::hardware_concurrency()));
  pool_config.max_cpu_threshold = static_cast<int>(
      100.0 - (50.0 / static_cast<double>(pool_config.max_threads)));

  auto pool = std::make_unique<SDDPWorkPool>(pool_config);
  pool->start();
  SPDLOG_TRACE("SDDP work pool started: max_threads={} cpu_threshold={:.0f}%",
               pool_config.max_threads,
               pool_config.max_cpu_threshold);
  return pool;
}

}  // namespace gtopt
