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

#include <gtopt/basic_types.hpp>
#include <span>  // For std::dynamic_extent

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

  // Use static constexpr string_view for class identifier
  static constexpr std::string_view class_name = "stage";

  // Use deducing this to get const and non-const versions
  template<typename Self>
  [[nodiscard]] constexpr decltype(auto) is_active(this Self&& self) noexcept
  {
    return std::forward<Self>(self).active.value_or(true);
  }

  // Use attribute for explicit member initialization
  [[no_unique_address]] std::byte padding_{};
};

using StageUid = StrongUidType<Stage>;
using StageIndex = StrongIndexType<Stage>;
using OptStageIndex = std::optional<StageIndex>;

}  // namespace gtopt

// Deduction guide for structured bindings (C++23)
namespace gtopt
{
}  // namespace gtopt

// Use standard concepts for incrementable traits
namespace std
{
template<>
struct incrementable_traits<gtopt::StageIndex>
{
  using difference_type = ptrdiff_t;  // More standard difference type
};
}  // namespace std
