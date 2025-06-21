/**
 * @file      stage.hpp
 * @brief     Stage representation for optimization problems
 * @date      Wed Mar 26 12:11:10 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the Stage struct representing time periods in optimization models
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
    return self.active.value_or(true);
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
Stage()->Stage;
}

// Use standard concepts for incrementable traits
namespace std
{
template<>
struct incrementable_traits<gtopt::StageIndex>
{
  using difference_type = ptrdiff_t;  // More standard difference type
};
}  // namespace std
