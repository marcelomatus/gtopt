/**
 * @file      reserve.hpp
 * @brief     Header of
 * @date      Thu Apr  3 10:30:07 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/object.hpp>

namespace gtopt
{

struct ReserveProvision
{
  Uid uid {};
  Name name {};
  OptActive active {};

  SingleId generator {};
  String reserve_zones {};
  OptTBRealFieldSched urmax {};
  OptTBRealFieldSched drmax {};

  OptTRealFieldSched ur_capacity_factor {};
  OptTRealFieldSched dr_capacity_factor {};

  OptTRealFieldSched ur_provision_factor {};
  OptTRealFieldSched dr_provision_factor {};

  OptTRealFieldSched urcost {};
  OptTRealFieldSched drcost {};
};

}  // namespace gtopt
