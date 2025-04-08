/**
 * @file      period.hpp
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

struct Period
{
  Uid uid {};
  OptName name {};
  OptBool active {};

  Size first_stage {};
  Size count_stage {};

  static constexpr std::string_view class_name = "period";
};

using PeriodUid = StrongUidType<struct Period>;
using PeriodIndex = StrongIndexType<Period>;
using OptPeriodIndex = std::optional<PeriodIndex>;

}  // namespace gtopt
