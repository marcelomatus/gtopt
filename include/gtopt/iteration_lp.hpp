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
 * Stores the Iteration definition and its position in the iteration_array.
 * The SDDP solver looks up IterationLP objects by their `index()` to find
 * per-iteration overrides.
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

private:
  Iteration m_iteration_ {};
  IterationIndex m_index_ {unknown_index};
};

}  // namespace gtopt
