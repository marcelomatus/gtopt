/**
 * @file      iteration_lp.hpp
 * @brief     LP representation of an Iteration control entry
 * @date      Mon Mar 24 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides the IterationLP class, which wraps an Iteration entry and
 * exposes per-iteration solver flags (e.g. whether to dispatch update_lp).
 */

#pragma once

#include <gtopt/iteration.hpp>

namespace gtopt
{

/**
 * @class IterationLP
 * @brief LP-side wrapper for an Iteration control entry
 *
 * Stores the Iteration definition and its position in the preallocated
 * iteration vector.  Default-constructed entries represent iterations with
 * no explicit user override; they return `should_update_lp() == true` and
 * `has_explicit_update_lp() == false`.
 *
 * The SDDP solver preallocates a vector of IterationLP objects sized to
 * `iteration_offset + max_iterations` and accesses them directly via
 * `IterationIndex`, giving O(1) lookup.
 */
class IterationLP
{
public:
  IterationLP() noexcept = default;

  explicit IterationLP(Iteration iteration,
                       IterationIndex iteration_index) noexcept
      : m_iteration_(iteration)
      , m_index_(iteration_index)
  {
  }

  /// @return The 0-based iteration index (the lookup key)
  [[nodiscard]] constexpr auto index() const noexcept { return m_index_; }

  /// @return The underlying Iteration data
  [[nodiscard]] constexpr const auto& iteration() const noexcept
  {
    return m_iteration_;
  }

  /// @return Whether update_lp should be dispatched for this iteration
  [[nodiscard]] constexpr auto should_update_lp() const noexcept
  {
    return m_iteration_.should_update_lp();
  }

  /// @return Whether the user explicitly set update_lp for this iteration
  [[nodiscard]] constexpr auto has_explicit_update_lp() const noexcept
  {
    return m_iteration_.update_lp.has_value();
  }

private:
  Iteration m_iteration_ {};
  IterationIndex m_index_ {unknown_index};
};

}  // namespace gtopt
