/**
 * @file      stage.hpp
 * @brief     Planning-period (stage) definition for power system optimization
 * @date      Wed Mar 26 12:11:10 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * A Stage represents an investment period in the multi-stage planning
 * horizon.  Capacity decisions made in a stage persist into all subsequent
 * stages.  Stage-level discount factors allow modelling the time value of
 * money.
 *
 * ### JSON Example
 * ```json
 * {"uid": 1, "first_block": 0, "count_block": 24, "discount_factor": 0.9}
 * ```
 *
 * @see Block for the time blocks contained within a stage
 * @see Phase for higher-level groupings of stages
 * @see StageLP for the LP representation
 */

#pragma once

#include <span>  // For std::dynamic_extent

#include <gtopt/basic_types.hpp>
#include <gtopt/stage_enums.hpp>

namespace gtopt
{

/**
 * @struct Stage
 * @brief Investment period grouping consecutive time blocks
 *
 * Each stage:
 * - References `count_block` consecutive blocks starting at `first_block`
 * - Applies an optional `discount_factor` to all costs incurred in this
 *   period (present-value discounting)
 * - Can be individually deactivated for scenario analysis
 *
 * @see Block for the time blocks contained within a stage
 * @see StageLP for the linear programming representation
 */
struct Stage
{
  Uid uid {unknown_uid};  ///< Unique identifier
  OptName name {};  ///< Optional human-readable label
  OptBool active {};  ///< Activation status (default: active)

  Size first_block {
      0,
  };  ///< 0-based index of the first block in this stage [dimensionless]
  Size count_block {std::dynamic_extent};  ///< Number of consecutive blocks in
                                           ///< this stage [dimensionless]
  OptReal discount_factor {
      1};  ///< Present-value cost multiplier for this stage [p.u.]

  /// Calendar month for this stage.
  /// Used for seasonal parameter lookups (e.g., irrigation schedules).
  std::optional<MonthType> month {};

  /// Whether blocks in this stage are chronologically ordered.
  /// When true, unit commitment constraints (startup/shutdown transitions)
  /// are enforced.  When false or absent, commitment is silently skipped.
  OptBool chronological {};

  static constexpr std::string_view class_name = "stage";

  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return active.value_or(true);
  }
};

using StageUid = StrongUidType<Stage>;
using StageIndex = StrongIndexType<Stage>;
using OptStageIndex = std::optional<StageIndex>;

}  // namespace gtopt
