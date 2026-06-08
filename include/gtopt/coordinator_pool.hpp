// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      coordinator_pool.hpp
 * @brief     Tier-1 scene/pass orchestration pool for the SDDP method.
 * @date      2026-06-08
 * @author    claude
 * @copyright BSD-3-Clause
 *
 * The coordinator tier runs one driver per scene per pass, each on its own
 * thread.  A driver executes that scene's forward (or backward) sweep and,
 * in the two-tier design, spends ~all its time blocked on Tier-2 solve
 * futures — so a blocked driver costs only a thread stack and no core.
 *
 * Crucially the coordinator tier carries **no** resource management: no
 * admission gate, no measured-memory model, no CPU/swap throttle, no
 * elastic sizing.  All of that lives in the solver tier, whose tasks are
 * the ones that actually allocate LP/solver memory.  This is why Tier 1 is
 * a trivial `std::async` fan-out and not a `BasicWorkPool`: it has no work
 * queue, no priority key and no gates.
 *
 * One driver per scene per pass means ~`num_scenes` thread spawns per pass —
 * negligible next to minutes of LP solves — so spawning per pass and joining
 * via the returned futures is the minimal correct implementation.
 */
#pragma once

#include <cstddef>
#include <future>
#include <type_traits>
#include <utility>

namespace gtopt
{

/// @brief Fixed-width scene/pass orchestration pool (Tier 1).
///
/// Stateless beyond its nominal driver count: each `run_driver` call fans a
/// callable onto its own thread and returns a future for the caller to
/// await.  Carries zero resource management by design (see file header).
class CoordinatorPool
{
public:
  explicit CoordinatorPool(std::size_t num_drivers) noexcept
      : m_num_drivers_(num_drivers)
  {
  }

  /// Nominal driver count (one per scene).  Informational — the pool spawns
  /// exactly one thread per `run_driver` call.
  [[nodiscard]] std::size_t num_drivers() const noexcept
  {
    return m_num_drivers_;
  }

  /// Run nullary `fn` on its own driver thread; returns a future for the
  /// result.  Called once per scene per pass; the caller awaits the futures
  /// (which joins the driver threads).
  template<class Fn>
  [[nodiscard]] auto run_driver(Fn&& fn)
      -> std::future<std::invoke_result_t<Fn>>
  {
    return std::async(std::launch::async, std::forward<Fn>(fn));
  }

private:
  std::size_t m_num_drivers_;
};

}  // namespace gtopt
