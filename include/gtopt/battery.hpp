/**
 * @file      battery.hpp
 * @brief     Header of
 * @date      Wed Apr  2 01:40:47 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/object.hpp>

namespace gtopt
{

struct Battery
{
  GTOPT_OBJECT_ATTRS;

  OptTRealFieldSched annual_loss {};
  OptTRealFieldSched vmin {};
  OptTRealFieldSched vmax {};
  OptTRealFieldSched vcost {};
  OptReal vini {};
  OptReal vfin {};

  OptTRealFieldSched capacity {};
  OptTRealFieldSched expcap {};
  OptTRealFieldSched expmod {};
  OptTRealFieldSched capmax {};
  OptTRealFieldSched annual_capcost {};
  OptTRealFieldSched annual_derating {};
};

}  // namespace gtopt
