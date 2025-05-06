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
  Uid uid {};  ///< Unique identifier
  Name name {};  ///< Line name
  OptActive active {};  ///< Line active status

  SingleId bus_a {};  ///< From-bus ID
  SingleId bus_b {};  ///< To-bus ID
  OptTRealFieldSched voltage {};  ///< Line voltage level
  OptTRealFieldSched resistance {};  ///< Line resistance
  OptTRealFieldSched reactance {};  ///< Line reactance
  OptTRealFieldSched lossfactor {};  ///< Line loss factor
  OptTBRealFieldSched tmin {};  ///< Minimum power flow limit
  OptTBRealFieldSched tmax {};  ///< Maximum power flow limit
  OptTRealFieldSched tcost {};  ///< Transmission cost

  OptTRealFieldSched capacity {};  ///< Installed capacity
  OptTRealFieldSched expcap {};  ///< Expansion capacity
  OptTRealFieldSched expmod {};  ///< Expansion module size
  OptTRealFieldSched capmax {};  ///< Maximum capacity limit
  OptTRealFieldSched annual_capcost {};  ///< Annual capacity cost
  OptTRealFieldSched annual_derating {};  ///< Annual derating factor
};

}  // namespace gtopt
