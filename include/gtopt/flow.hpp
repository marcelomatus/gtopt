/**
 * @file      flow.hpp
 * @brief     Header of
 * @date      Wed Jul 30 15:52:34 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

struct Flow
{
  Uid uid {};
  Name name {};
  OptActive active {};

  /// Flow direction: 1 for input, -1 for output
  Int direction {1};

  SingleId junction {};
  STBRealFieldSched discharge {};

  [[nodiscard]] constexpr bool is_input() const noexcept { return direction > 0; }
  [[nodiscard]] constexpr bool is_output() const noexcept { return direction < 0; }
};

}  // namespace gtopt
