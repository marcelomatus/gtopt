/**
 * @file      bus.hpp
 * @brief     Header of
 * @date      Tue Mar 18 13:31:45 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Bus class, which defines an electric Busbar.
 */

#pragma once

#include <gtopt/basic_types.hpp>

namespace gtopt
{

struct Bus
{
  Uid uid {};
  Name name {};

  OptReal voltage {};
  OptReal reference_theta {};
  OptBool skip_kirchhoff {};
};

}  // namespace gtopt
