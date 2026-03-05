/**
 * @file      line.hpp
 * @brief     Header for transmission line components
 * @date      Sun Apr 20 02:12:30 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines data structures for representing AC transmission lines
 * in power system planning models. In DC power-flow (Kirchhoff) mode the
 * reactance determines the flow split between parallel paths. The line also
 * supports capacity-expansion planning.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "l1_4",
 *   "bus_a": "b1",
 *   "bus_b": "b4",
 *   "reactance": 0.0576,
 *   "tmax_ab": 250,
 *   "tmax_ba": 250
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 2-D inline array indexed by `[stage][block]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Line/`
 */

#pragma once

#include <gtopt/capacity.hpp>

namespace gtopt
{

/**
 * @struct Line
 * @brief Represents a transmission line (branch) connecting two buses
 *
 * A line carries active power flow `f ∈ [-tmax_ba, tmax_ab]` between
 * `bus_a` and `bus_b`. In Kirchhoff mode the flow is constrained by
 * `f = (θ_a − θ_b) / reactance`. Optional expansion variables allow
 * the solver to invest in additional transfer capacity.
 *
 * @see Bus for connected bus definitions
 * @see LineLP for the LP formulation
 */
struct Line
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Line name
  OptActive active {};  ///< Activation status (default: active)

  SingleId bus_a {unknown_uid};  ///< Sending-end (from) bus ID
  SingleId bus_b {unknown_uid};  ///< Receiving-end (to) bus ID

  OptTRealFieldSched voltage {};  ///< Nominal voltage level [kV]
  OptTRealFieldSched resistance {};  ///< Series resistance [p.u.]
  OptTRealFieldSched reactance {};  ///< Series reactance used in DC power flow [p.u.]
  OptTRealFieldSched lossfactor {};  ///< Lumped loss factor [p.u.]
  OptTBRealFieldSched tmax_ba {};  ///< Maximum power flow in B→A direction [MW]
  OptTBRealFieldSched tmax_ab {};  ///< Maximum power flow in A→B direction [MW]
  OptTRealFieldSched tcost {};  ///< Variable transmission cost [$/MWh]

  OptTRealFieldSched capacity {};  ///< Installed transfer capacity [MW]
  OptTRealFieldSched expcap {};  ///< Capacity added per expansion module [MW]
  OptTRealFieldSched expmod {};  ///< Maximum number of expansion modules [dimensionless]
  OptTRealFieldSched capmax {};  ///< Absolute maximum capacity after expansion [MW]
  OptTRealFieldSched annual_capcost {};  ///< Annualized investment cost [$/MW-year]
  OptTRealFieldSched annual_derating {};  ///< Annual capacity derating factor [p.u./year]
};

}  // namespace gtopt
