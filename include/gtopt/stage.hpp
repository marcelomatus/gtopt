/**
 * @file      stage.hpp
 * @brief     Header of
 * @date      Wed Mar 26 12:11:10 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

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

namespace std
{

template<>
struct incrementable_traits<gtopt::StageIndex>
{
  using difference_type = int;
};

}  // namespace std
