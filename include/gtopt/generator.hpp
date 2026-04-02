/**
 * @file      generator.hpp
 * @brief     Header for generator components in power system planning
 * @date      Sat Mar 29 11:52:29 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines data structures for representing generators (thermal,
 * renewable, hydro) in power system planning models. A generator injects
 * active power at a bus and may be expanded via capacity-planning variables.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "g1",
 *   "bus": "b1",
 *   "pmin": 10,
 *   "pmax": 250,
 *   "gcost": 20,
 *   "capacity": 250
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant (e.g. `100`)
 * - A 2-D inline array indexed by `[stage][block]` (e.g. `[[100, 90], [95]]`)
 * - A filename string referencing a Parquet/CSV schedule in the
 *   `input_directory/Generator/` directory (e.g. `"pmax"`)
 */

#pragma once

#include <gtopt/capacity.hpp>

namespace gtopt
{
/**
 * @struct GeneratorAttrs
 * @brief Core technical and economic attributes shared by generator objects
 *
 * Used as a lightweight value type when parsing generator-attribute updates
 * separately from the generator identity fields.
 */
struct GeneratorAttrs
{
  SingleId bus {unknown_uid};  ///< Bus ID where the generator is connected

  OptTBRealFieldSched pmin {};  ///< Minimum active power output [MW]
  OptTBRealFieldSched pmax {};  ///< Maximum active power output [MW]
  OptTRealFieldSched lossfactor {};  ///< Network loss factor [p.u.]
  OptTRealFieldSched gcost {};  ///< Variable generation cost [$/MWh]

  OptTRealFieldSched capacity {};  ///< Installed generation capacity [MW]
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
 * @struct Generator
 * @brief Represents a generation unit (thermal, renewable, hydro) at a bus
 *
 * A generator injects active power `p ∈ [pmin, pmax]` at its connected bus.
 * The LP objective includes `gcost × power × duration` for operational cost.
 * When `expcap` and `expmod` are non-null the solver may invest in additional
 * capacity modules at cost `annual_capcost` per module per year.
 *
 * @see GeneratorProfile for time-varying capacity-factor profiles
 * @see GeneratorLP for the LP formulation
 */
struct Generator
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Generator name
  OptActive active {};  ///< Activation status (default: active)
  OptName type {};  ///< Optional generator type tag (e.g. "thermal", "hydro",
                    ///< "solar")

  SingleId bus {unknown_uid};  ///< Bus ID where the generator is connected

  OptTBRealFieldSched pmin {};  ///< Minimum active power output [MW]
  OptTBRealFieldSched pmax {};  ///< Maximum active power output [MW]
  OptTRealFieldSched lossfactor {};  ///< Network loss factor [p.u.]
  OptTRealFieldSched gcost {};  ///< Variable generation cost [$/MWh]

  OptTRealFieldSched capacity {};  ///< Installed generation capacity [MW]
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
   * @brief Sets generator attributes from a GeneratorAttrs object
   * @param self  The generator object to update (deduced; supports
   * const/non-const).
   * @param attrs Generator attributes to be set
   * @return Reference to this Generator object
   *
   * @code{.cpp}
   * // Example usage:
   * Generator gen;
   * GeneratorAttrs attrs;
   * attrs.bus = 1;
   * attrs.pmax = 100.0;
   * gen.set_attrs(std::move(attrs));
   * // gen.bus should now be 1, and attrs.bus should be empty
   * @endcode
   */

  auto& set_attrs(this auto&& self, auto&& attrs)
  {
    self.bus = std::exchange(attrs.bus, {});
    self.pmin = std::exchange(attrs.pmin, {});
    self.pmax = std::exchange(attrs.pmax, {});
    self.lossfactor = std::exchange(attrs.lossfactor, {});
    self.gcost = std::exchange(attrs.gcost, {});

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
