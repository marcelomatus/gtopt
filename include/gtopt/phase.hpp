/**
 * @file      phase.hpp
 * @brief     Defines the Phase structure for multi-phase optimization problems
 * @date      Wed Mar 26 12:11:10 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * A Phase groups a consecutive set of planning stages. Phases allow
 * modelling problems with distinct investment or operational windows,
 * e.g. a 5-year construction phase followed by a 20-year operational phase.
 *
 * ### JSON Example
 * ```json
 * {"uid": 1, "first_stage": 0, "count_stage": 5}
 * ```
 *
 * @see Stage for the stages contained within a phase
 */

#pragma once

#include <span>

#include <gtopt/basic_types.hpp>

namespace gtopt
{

/**
 * @struct Phase
 * @brief Groups consecutive planning stages into a higher-level period
 *
 * When only one phase is needed (the common case) it is created
 * automatically with default values that cover all stages.
 */
struct Phase
{
  /// Unique identifier for the phase
  [[no_unique_address]] Uid uid {unknown_uid};

  /// Optional human-readable label
  [[no_unique_address]] OptName name {};

  /// Activation status (default: active)
  [[no_unique_address]] OptBool active {};

  /// 0-based index of the first stage in this phase [dimensionless]
  Size first_stage {0};

  /**
   * @brief Number of stages in this phase [dimensionless]
   * @note `std::dynamic_extent` means "all remaining stages".
   */
  Size count_stage {std::dynamic_extent};

  /// Optional aperture UIDs to use in this phase's SDDP backward pass.
  /// When empty, all apertures from the global `aperture_array` are used.
  /// When non-empty, only the listed aperture UIDs participate in the
  /// backward-pass cut computation for this phase.
  Array<Uid> apertures {};

  /// Class name constant used for serialisation/deserialisation
  static constexpr std::string_view class_name = "phase";

  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return active.value_or(true);
  }
};

/// Strongly-typed unique identifier for Phase objects
using PhaseUid = StrongUidType<Phase>;

/// Strongly-typed index for Phase objects in collections
using PhaseIndex = StrongPositionIndexType<Phase>;

/// @brief First phase index — the root of the planning horizon.
[[nodiscard]] constexpr auto first_phase_index() noexcept -> PhaseIndex
{
  return PhaseIndex {0};
}

/// @brief Next phase index (phase_index + 1), preserving strong type.
[[nodiscard]] constexpr auto next(PhaseIndex phase_index) noexcept -> PhaseIndex
{
  return ++phase_index;
}

/// @brief Previous phase index (phase_index - 1), preserving strong type.
[[nodiscard]] constexpr auto previous(PhaseIndex phase_index) noexcept
    -> PhaseIndex
{
  return --phase_index;
}

}  // namespace gtopt
