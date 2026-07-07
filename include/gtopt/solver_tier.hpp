// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      solver_tier.hpp
 * @brief     Tier-2 solver executor seam for the SDDP method.
 * @date      2026-06-08
 * @author    claude
 * @copyright BSD-3-Clause
 *
 * `SolverTier` is the single point through which SDDP LP solves reach an
 * execution backend.  In this first step it is a transparent wrapper around
 * the existing `SDDPWorkPool` — the bodies of `SDDPMethod::resolve_via_pool`
 * and `resolve_clone_via_pool` move here verbatim, so behaviour is
 * unchanged.  Later steps swap the engine behind this interface (a
 * resource-aware governor in front of a battle-tested thread pool) without
 * touching any call site, and the per-scene orchestration moves onto a
 * separate coordinator tier that owns no memory management.
 */
#pragma once

#include <expected>

#include <gtopt/error.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/sddp_pool.hpp>
#include <gtopt/solver_options.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

/// @brief Tier-2 LP-solve executor.
///
/// Owns a non-owning pointer to the solver work pool.  A null pool means
/// "no pool available" — solves run directly on the caller thread, matching
/// the legacy fallback behaviour.
class SolverTier
{
public:
  SolverTier() = default;
  explicit SolverTier(SDDPWorkPool* pool) noexcept
      : m_pool_(pool)
  {
  }

  /// Point the tier at a work pool (or `nullptr` to detach).  Called
  /// wherever the SDDP method (re)binds its pool.
  void set_pool(SDDPWorkPool* pool) noexcept { m_pool_ = pool; }

  [[nodiscard]] SDDPWorkPool* pool() const noexcept { return m_pool_; }

  /// Resolve an LP via the work pool.  Falls back to a direct resolve if the
  /// pool is unavailable or submission fails.
  [[nodiscard]] auto resolve_via_pool(
      LinearInterface& li,
      const SolverOptions& opts,
      const BasicTaskRequirements<SDDPTaskKey>& task_req = {})
      -> std::expected<int, Error>
  {
    if (m_pool_ == nullptr) {
      return li.resolve(opts);
    }

    // Stamp the backend's declared resource class so GPU-backed solves
    // (cuOpt) are gated on the process-global GPU token bucket instead of
    // fanning out to cpu_factor x cores (see BasicTaskRequirements).
    auto req = task_req;
    req.resource_class = li.resource_class();
    auto fut = m_pool_->submit([&li, &opts] { return li.resolve(opts); }, req);
    if (fut.has_value()) {
      // Release this task's worker slot (exact no-op when the caller is
      // not one of the pool's workers, e.g. a coordinator driver) while
      // blocking on the inner solve.  Without it, an async-path scene
      // driver parked here still counts as "active"; when every worker
      // is such a parked driver and a resource gate (RSS/CPU) is closed,
      // the pool wedges — the universal progress guarantee only fires at
      // `active == 0`.
      auto slot_guard = m_pool_->release_slot_while_blocking();
      return fut->get();
    }
    SPDLOG_WARN("resolve_via_pool: pool submit failed, falling back to direct");
    return li.resolve(opts);
  }

  /// Resolve a cloned LP via the work pool.  The clone reference is safe
  /// because the future is awaited synchronously before this scope exits.
  [[nodiscard]] auto resolve_clone_via_pool(
      LinearInterface& clone,
      const SolverOptions& opts,
      const BasicTaskRequirements<SDDPTaskKey>& task_req = {})
      -> std::expected<int, Error>
  {
    if (m_pool_ == nullptr) {
      return clone.resolve(opts);
    }

    auto req = task_req;
    req.resource_class = clone.resource_class();
    auto fut =
        m_pool_->submit([&clone, &opts] { return clone.resolve(opts); }, req);
    if (fut.has_value()) {
      // See resolve_via_pool: yield the caller's slot during the wait.
      auto slot_guard = m_pool_->release_slot_while_blocking();
      return fut->get();
    }
    SPDLOG_WARN(
        "resolve_clone_via_pool: pool submit failed, falling back to direct");
    return clone.resolve(opts);
  }

private:
  /// Non-owning pointer to the SDDP work pool; `nullptr` ⇒ direct solve.
  SDDPWorkPool* m_pool_ {nullptr};
};

}  // namespace gtopt
