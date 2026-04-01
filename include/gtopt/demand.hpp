/**
 * @file      demand.hpp
 * @brief     Header file defining demand-related structures for power system
 * planning
 * @date      Thu Mar 27 09:12:21 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides structures and types to represent and manipulate
 * electrical demand (load) entities within the planning framework. A demand
 * absorbs active power at a bus and can be expanded via capacity-planning
 * variables to model flexible or expandable load.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "d1",
 *   "bus": "b1",
 *   "lmax": 125.0,
 *   "capacity": 0,
 *   "expcap": 20,
 *   "expmod": 10,
 *   "annual_capcost": 8760
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 2-D inline array indexed by `[stage][block]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Demand/`
 */

#pragma once

#include <gtopt/capacity.hpp>

namespace gtopt
{

/**
 * @struct DemandAttrs
 * @brief Technical and economic attributes shared by demand objects
 *
 * Used as a lightweight value type when parsing demand-attribute updates
 * separately from the demand identity fields.
 */
struct DemandAttrs
{
  SingleId bus {unknown_uid};  ///< Bus ID where the demand is connected
  OptTBRealFieldSched lmax {};  ///< Maximum served load [MW]
  OptTRealFieldSched lossfactor {};  ///< Network loss factor [p.u.]
  OptTRealFieldSched fcost {};  ///< Demand curtailment cost [$/MWh]
  OptTRealFieldSched
      emin {};  ///< Minimum energy that must be served per stage [MWh]
  OptTRealFieldSched ecost {};  ///< Energy-shortage cost [$/MWh]

  OptTRealFieldSched capacity {};  ///< Installed demand capacity [MW]
  OptTRealFieldSched expcap {};  ///< Capacity added per expansion module [MW]
  OptTRealFieldSched
      expmod {};  ///< Maximum number of expansion modules [dimensionless]
  OptTRealFieldSched
      capmax {};  ///< Absolute maximum capacity after expansion [MW]
  OptTRealFieldSched
      annual_capcost {};  ///< Annualized investment cost [$/MW-year]
  OptTRealFieldSched
      annual_derating {};  ///< Annual capacity derating factor [p.u./year]
};

/**
 * @struct Demand
 * @brief Represents an electrical demand (load) at a bus
 *
 * A demand consumes active power `load ∈ [0, lmax]` at its connected bus.
 * Any deficit between `lmax` and the served load is treated as unserved
 * demand and penalized at the global `demand_fail_cost`. When `expcap` and
 * `expmod` are non-null the solver may invest in additional load capacity.
 *
 * @see DemandProfile for time-varying demand profiles
 * @see DemandLP for the LP formulation
 */
struct Demand
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Descriptive name
  OptActive active {};  ///< Activation status (default: active)
  OptName
      type {};  ///< Optional demand type tag (e.g. "residential", "industrial")

  SingleId bus {unknown_uid};  ///< Bus ID where the demand is connected
  OptTBRealFieldSched lmax {};  ///< Maximum served load [MW]
  OptTRealFieldSched lossfactor {};  ///< Network loss factor [p.u.]
  OptTRealFieldSched fcost {};  ///< Demand curtailment cost [$/MWh]
  OptTRealFieldSched
      emin {};  ///< Minimum energy that must be served per stage [MWh]
  OptTRealFieldSched ecost {};  ///< Energy-shortage cost [$/MWh]

  OptTRealFieldSched capacity {};  ///< Installed demand capacity [MW]
  OptTRealFieldSched expcap {};  ///< Capacity added per expansion module [MW]
  OptTRealFieldSched
      expmod {};  ///< Maximum number of expansion modules [dimensionless]
  OptTRealFieldSched
      capmax {};  ///< Absolute maximum capacity after expansion [MW]
  OptTRealFieldSched
      annual_capcost {};  ///< Annualized investment cost [$/MW-year]
  OptTRealFieldSched
      annual_derating {};  ///< Annual capacity derating factor [p.u./year]

  /**
   * @brief Sets the demand attributes from a DemandAttrs object
   *
   * @tparam T Type of attributes (deduced automatically)
   * @param self  The demand object to update (deduced; supports
   * const/non-const).
   * @param attrs The attribute object to transfer values from
   * @return Reference to the updated demand object
   */
  auto& set_attrs(this auto&& self, auto&& attrs)
  {
    self.bus = std::exchange(attrs.bus, {});
    self.lmax = std::exchange(attrs.lmax, {});
    self.lossfactor = std::exchange(attrs.lossfactor, {});
    self.fcost = std::exchange(attrs.fcost, {});
    self.emin = std::exchange(attrs.emin, {});
    self.ecost = std::exchange(attrs.ecost, {});

    self.capacity = std::exchange(attrs.capacity, {});
    self.expcap = std::exchange(attrs.expcap, {});
    self.expmod = std::exchange(attrs.expmod, {});
    self.capmax = std::exchange(attrs.capmax, {});
    self.annual_capcost = std::exchange(attrs.annual_capcost, {});
    self.annual_derating = std::exchange(attrs.annual_derating, {});

    return self;
  }
};
}  // namespace gtopt
