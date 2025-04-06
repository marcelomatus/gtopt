/**
 * @file      reserve_zone.hpp
 * @brief     Header of
 * @date      Thu Apr  3 10:32:46 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/object.hpp>

namespace gtopt
{

struct ReserveZone
{
  GTOPT_OBJECT_ATTRS;

  OptTBRealFieldSched urreq {};  // up reserve requirement
  OptTBRealFieldSched drreq {};  // down reserve requirement
  OptTRealFieldSched urcost {};  // up reserve shortage cost
  OptTRealFieldSched drcost {};  // down reserve shortage cost
};

}  // namespace gtopt
