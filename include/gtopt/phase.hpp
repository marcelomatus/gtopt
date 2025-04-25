/**
 * @file      phase.hpp
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

struct Phase
{
  Uid uid {};
  OptName name {};
  OptBool active {};

  Size first_stage {0};
  Size count_stage {std::numeric_limits<Size>::max()};

  static constexpr std::string_view class_name = "phase";
};

using PhaseUid = StrongUidType<Phase>;
using PhaseIndex = StrongIndexType<Phase>;
using OptPhaseIndex = std::optional<PhaseIndex>;

}  // namespace gtopt
