/**
 * @file      stage.hpp
 * @brief     Time period representation for power system optimization models
 * @date      Wed Mar 26 12:11:10 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * @struct Stage
 * @brief Represents a distinct time period in optimization planning
 *
 * Each Stage:
 * - Defines a contiguous set of time blocks
 * - Applies optional discount factors for cost calculations
 * - Can be marked active/inactive for scenario filtering
 *
 * Key attributes:
 * - uid: Unique identifier for the stage
 * - first_block: Index of first block in the stage
 * - count_block: Number of blocks in the stage
 * - discount_factor: Optional stage-specific cost multiplier
 * - active: Controls whether stage is included in optimizations
 *
 * @see StageLP for the linear programming representation
 * @see Phase for higher-level time groupings
 */

#pragma once

#include <span>  // For std::dynamic_extent

#include <gtopt/basic_types.hpp>

namespace gtopt
{

struct Stage
{
  Uid uid {unknown_uid};
  OptName name {};
  OptBool active {};

  Size first_block {0};
  Size count_block {std::dynamic_extent};
  OptReal discount_factor {1};

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
