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
  Uid uid {};
  OptName name {};
  OptBool active {};

  Size first_block {};
  Size count_block {};
  OptReal discount_factor {1};

  static constexpr std::string_view class_name = "stage";
};

using StageUid = StrongUidType<struct tuid_>;
using StageIndex = StrongIndexType<Stage>;
using OptStageIndex = std::optional<StageIndex>;

}  // namespace gtopt
