/**
 * @file      reservoir.hpp
 * @brief     Defines the Reservoir structure representing a water reservoir
 * @date      Wed Jul 30 23:11:55 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Reservoir structure which represents a water storage
 * component in the system. A reservoir stores water and controls its release
 * through connected waterways and turbines.
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @brief Represents a water reservoir in the system
 *
 * A reservoir stores water and controls its release through connected
 * waterways and turbines. It has capacity constraints and may have
 * associated costs for water storage.
 */
struct Reservoir
{
  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};

  SingleId junction {unknown_uid};
  OptTRealFieldSched capacity {};

  OptTRealFieldSched annual_loss {};
  OptTRealFieldSched vmin {};
  OptTRealFieldSched vmax {};
  OptTRealFieldSched vcost {};
  OptReal vini {};
  OptReal vfin {};

  OptReal vol_scale {1.0};
};

}  // namespace gtopt
