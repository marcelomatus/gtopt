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

  OptTRealFieldSched voltage {};  ///< Line voltage level [KW]
  OptTRealFieldSched resistance {};  ///< Line resistance [Ohm]
  OptTRealFieldSched reactance {};  ///< Line reactance  [Ohm]
  OptTRealFieldSched lossfactor {};  ///< Line loss factor [p.u.]
  OptTBRealFieldSched tmax_ba {};  ///< Minimum power flow [MW]
  OptTBRealFieldSched tmax_ab {};  ///< Maximum power flow [MW]
  OptTRealFieldSched tcost {};  ///< Transmission cost [$/MWh]

  OptTRealFieldSched capacity {};  ///< Installed capacity [MW]
  OptTRealFieldSched expcap {};  ///< Expansion capacity [MW]
  OptTRealFieldSched expmod {};  ///< Expansion module [MW]
  OptTRealFieldSched capmax {};  ///< Maximum capacity [MW]
  OptTRealFieldSched annual_capcost {};  ///< Capacity cost [$/MW-year]
  OptTRealFieldSched annual_derating {};  /// Derating factor [p.u./year]
};

}  // namespace gtopt
