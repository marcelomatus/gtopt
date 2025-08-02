/**
 * @file      line.hpp
 * @brief     Header for transmission line components
 * @date      Sun Apr 20 02:12:30 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines data structures for representing transmission lines
 * in power system planning models.
 */

#pragma once

#include <gtopt/capacity.hpp>

namespace gtopt
{

/**
 * @struct Line
 * @brief Represents a transmission line in a power system planning model
 */
struct Line
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Line name
  OptActive active {};  ///< Line active status

  SingleId bus_a {unknown_uid};  ///< From-bus ID
  SingleId bus_b {unknown_uid};  ///< To-bus ID

  OptTRealFieldSched voltage {};  ///< Line voltage level
  OptTRealFieldSched resistance {};  ///< Line resistance
  OptTRealFieldSched reactance {};  ///< Line reactance
  OptTRealFieldSched lossfactor {};  ///< Line loss factor
  OptTBRealFieldSched tmax_ba {};  ///< Minimum power flow
  OptTBRealFieldSched tmax_ab {};  ///< Maximum power flow
  OptTRealFieldSched tcost {};  ///< Transmission cost

  OptTRealFieldSched capacity {};  ///< Installed capacity
  OptTRealFieldSched expcap {};  ///< Expansion capacity
  OptTRealFieldSched expmod {};  ///< Expansion module
  OptTRealFieldSched capmax {};  ///< Maximum capacity
  OptTRealFieldSched annual_capcost {};  ///< Capacity cost
  OptTRealFieldSched annual_derating {};  /// Derating factor
};

}  // namespace gtopt
