/**
 * @file      reservoir.hpp
 * @brief     Defines the Reservoir structure representing a water reservoir
 * @date      Wed Jul 30 23:11:55 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This file defines the `gtopt::Reservoir` structure, which models a water reservoir
 * within a hydro-thermal power system. A reservoir is a key component for
 * storing and managing water resources, influencing power generation schedules
 * and water flow dynamics. It is characterized by its storage capacity,
 * operational limits, and associated costs.
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @brief Models a water reservoir with its physical and economic attributes.
 *
 * This structure holds all the data defining a water reservoir, including its
 * physical properties (like capacity, minimum/maximum volumes) and economic
 * factors (like storage costs). It also links to a junction in the water
 * network. The data can be static or time-varying to model different
 * conditions over an optimization horizon.
 */
struct Reservoir
{
  /// @brief Unique identifier for the reservoir.
  Uid uid {unknown_uid};
  /// @brief Name of the reservoir.
  Name name {};
  /// @brief Optional flag indicating if the reservoir is active.
  OptActive active {};

  /// @brief ID of the junction associated with this reservoir.
  SingleId junction {unknown_uid};
  /// @brief Optional time-varying storage capacity of the reservoir.
  OptTRealFieldSched capacity {};

  /// @brief Optional time-varying annual water loss as a fraction of the stored volume (e.g., due to evaporation).
  OptTRealFieldSched annual_loss {};
  /// @brief Optional time-varying minimum allowed volume.
  OptTRealFieldSched vmin {};
  /// @brief Optional time-varying maximum allowed volume.
  OptTRealFieldSched vmax {};
  /// @brief Optional time-varying cost associated with stored water volume.
  OptTRealFieldSched vcost {};
  /// @brief Optional initial volume of water at the beginning of the optimization horizon.
  OptReal vini {};
  /// @brief Optional final (target) volume of water at the end of the optimization horizon.
  OptReal vfin {};

  /// @brief Optional scaling factor for volume units. Defaults to 1.0.
  OptReal vol_scale {1.0};
};

}  // namespace gtopt
