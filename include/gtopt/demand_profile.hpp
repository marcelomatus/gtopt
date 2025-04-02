/**
 * @file      demand_profile.hpp
 * @brief     Header of
 * @date      Wed Apr  2 01:23:45 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/demand.hpp>

namespace gtopt
{

struct DemandProfile
{
  GTOPT_OBJECT_ATTRS;

  DemandVar demand {};
  STBRealFieldSched profile {};
  OptTRealFieldSched scost {};
};

}  // namespace gtopt
