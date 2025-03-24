/**
 * @file      system.hpp<gtopt>
 * @brief     Header of System class
 * @date      Wed Mar 19 21:59:12 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the System class, which contains all the system elements.
 */

#pragma once

#include <gtopt/bus.hpp>

namespace gtopt
{

struct System
{
  Name name;
  String version;

  Array<Bus> bus_array;
};

}  // namespace gtopt
