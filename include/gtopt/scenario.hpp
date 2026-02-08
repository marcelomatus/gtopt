/**
 * @file      scenario.hpp
 * @brief     Header of
 * @date      Wed Mar 26 12:12:32 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/basic_types.hpp>

namespace gtopt
{

struct Scenario
{
  Uid uid {unknown_uid};
  OptName name {};
  OptBool active {};

  OptReal probability_factor {1};

  static constexpr std::string_view class_name = "scenario";

  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return active.value_or(true);
  }
};

using ScenarioUid = StrongUidType<struct Scenario>;
using ScenarioIndex = StrongIndexType<struct Scenario>;

}  // namespace gtopt
