/**
 * @file      reservoir.hpp
 * @brief     Header of
 * @date      Wed Jul 30 23:11:55 2025
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

struct Reservoir
{
  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};

  SingleId junction {};
  OptTRealFieldSched capacity {};

  OptTRealFieldSched annual_loss {};
  OptTRealFieldSched vmin {};
  OptTRealFieldSched vmax {};
  OptTRealFieldSched vcost {};
  OptReal vini {};
  OptReal vfin {};
};

}  // namespace gtopt
