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

  SingleId bus_a {};  ///< From-bus ID
  SingleId bus_b {};  ///< To-bus ID
  [[no_unique_address]] OptTRealFieldSched voltage {};  ///< Line voltage level
  [[no_unique_address]] OptTRealFieldSched resistance {};  ///< Line resistance
  [[no_unique_address]] OptTRealFieldSched reactance {};  ///< Line reactance
  [[no_unique_address]] OptTRealFieldSched lossfactor {};  ///< Line loss factor
  [[no_unique_address]] OptTBRealFieldSched tmin {};  ///< Minimum power flow
  [[no_unique_address]] OptTBRealFieldSched tmax {};  ///< Maximum power flow
  [[no_unique_address]] OptTRealFieldSched tcost {};  ///< Transmission cost

  [[no_unique_address]] OptTRealFieldSched capacity {};  ///< Installed capacity
  [[no_unique_address]] OptTRealFieldSched expcap {};  ///< Expansion capacity
  [[no_unique_address]] OptTRealFieldSched expmod {};  ///< Expansion module
  [[no_unique_address]] OptTRealFieldSched capmax {};  ///< Maximum capacity
  [[no_unique_address]] OptTRealFieldSched
      annual_capcost {};  ///< Capacity cost
  [[no_unique_address]] OptTRealFieldSched
      annual_derating {};  /// Derating factor
};

}  // namespace gtopt
