/**
 * @file      generator.hpp
 * @brief     Header for generator components in power system planning
 * @date      Sat Mar 29 11:52:29 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines data structures for representing generators in power
 * system planning models
 */

#pragma once

#include <gtopt/capacity.hpp>

namespace gtopt
{
/**
 * @struct GeneratorAttrs
 * @brief Contains the core attributes for a generator
 */
struct GeneratorAttrs
{
  SingleId bus {unknown_uid};  ///< Bus ID where the generator is connected

  OptTBRealFieldSched pmin {};  ///< Minimum active power output schedule
  OptTBRealFieldSched pmax {};  ///< Maximum active power output schedule
  OptTRealFieldSched lossfactor {};  ///< Loss factor for the generator
  OptTRealFieldSched gcost {};  ///< Generation cost

  OptTRealFieldSched capacity {};  ///< Installed capacity
  OptTRealFieldSched expcap {};  ///< Expansion capacity
  OptTRealFieldSched expmod {};  ///< Expansion module size
  OptTRealFieldSched capmax {};  ///< Maximum capacity limit
  OptTRealFieldSched annual_capcost {};  ///< Annual capacity cost
  OptTRealFieldSched annual_derating {};  ///< Annual derating factor
};

/**
 * @struct Generator
 * @brief Represents a generator in a power system planning model
 */
struct Generator
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Generator name
  OptActive active {};  ///< Generator active status

  SingleId bus {unknown_uid};  ///< Bus ID where the generator is connected

  [[no_unique_address]] OptTBRealFieldSched
      pmin {};  ///< Minimum active power output schedule
  [[no_unique_address]] OptTBRealFieldSched
      pmax {};  ///< Maximum active power output schedule
  [[no_unique_address]] OptTRealFieldSched
      lossfactor {};  ///< Loss factor for the generator
  [[no_unique_address]] OptTRealFieldSched gcost {};  ///< Generation cost

  [[no_unique_address]] OptTRealFieldSched capacity {};  ///< Installed capacity
  [[no_unique_address]] OptTRealFieldSched expcap {};  ///< Expansion capacity
  [[no_unique_address]] OptTRealFieldSched
      expmod {};  ///< Expansion module size
  [[no_unique_address]] OptTRealFieldSched
      capmax {};  ///< Maximum capacity limit
  [[no_unique_address]] OptTRealFieldSched
      annual_capcost {};  ///< Annual capacity cost
  [[no_unique_address]] OptTRealFieldSched
      annual_derating {};  ///< Annual derating factor

  /**
   * @brief Sets generator attributes from a GeneratorAttrs object
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
