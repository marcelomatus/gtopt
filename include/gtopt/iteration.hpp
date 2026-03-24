/**
 * @file      iteration.hpp
 * @brief     Defines the Iteration structure for per-iteration solver control
 * @date      Mon Mar 24 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * An Iteration allows per-iteration control of SDDP solver behaviour.
 * When an `iteration_array` is present in the Simulation JSON, the solver
 * looks up the current iteration index and honours per-iteration flags
 * such as `update_lp` (whether to dispatch LP coefficient updates).
 *
 * ### JSON Example
 * ```json
 * {"index": 0, "update_lp": false}
 * {"index": 5, "update_lp": true}
 * ```
 */

#pragma once

#include <gtopt/basic_types.hpp>

namespace gtopt
{

/**
 * @struct Iteration
 * @brief Per-iteration control flags for the SDDP solver
 *
 * Unlike Phase and Scenario, Iteration is keyed by `Index index` rather
 * than a Uid.  The solver matches the current 0-based iteration number
 * against this index to find per-iteration overrides.
 */
struct Iteration
{
  /// 0-based iteration index used as lookup key
  Index index {0};

  /// Whether to dispatch update_lp for this iteration.
  /// When absent (nullopt), update_lp is dispatched (default = true).
  OptBool update_lp {};

  /// Class name constant used for serialisation/deserialisation
  static constexpr std::string_view class_name = "iteration";

  /// @return Whether update_lp should be dispatched for this iteration
  [[nodiscard]] constexpr auto should_update_lp() const noexcept
  {
    return update_lp.value_or(true);
  }
};

/// Tag type for SDDP iteration numbering
struct IterationTag;

/// Strongly-typed index for SDDP iterations
using IterationIndex = StrongIndexType<IterationTag>;

}  // namespace gtopt
