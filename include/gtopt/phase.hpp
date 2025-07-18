/**
 * @file      phase.hpp
 * @brief     Defines the Phase structure and related types for optimization
 * phases
 * @date      Wed Mar 26 12:11:10 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Phase structure which represents a distinct phase in
 * a multi-phase optimization problem. Each phase contains configuration for a
 * set of stages in the optimization.
 */

#pragma once

#include <span>

#include <gtopt/basic_types.hpp>

namespace gtopt
{

/**
 * @struct Phase
 * @brief Represents a phase in a multi-phase optimization problem
 *
 * A phase groups together a set of stages in the optimization process with
 * common configuration. This allows modeling problems with distinct phases like
 * planning, operation, etc.
 */
struct Phase
{
  /// Unique identifier for the phase
  [[no_unique_address]] Uid uid {unknown_uid};

  /// Optional name for the phase (human-readable)
  [[no_unique_address]] OptName name {};

  /// Flag indicating if this phase is active in the optimization
  [[no_unique_address]] OptBool active {};

  /// Index of the first stage belonging to this phase
  Size first_stage {0};

  /**
   * @brief Number of stages in this phase
   * @note Uses std::dynamic_extent to indicate all remaining stages when
   * unspecified
   */
  Size count_stage {std::dynamic_extent};

  /// Class name constant used for serialization/deserialization
  static constexpr std::string_view class_name = "phase";

  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return active.value_or(true);
  }
};

/// Strongly-typed unique identifier for Phase objects
using PhaseUid = StrongUidType<Phase>;

/// Strongly-typed index for Phase objects in collections
using PhaseIndex = StrongIndexType<Phase>;

}  // namespace gtopt
