/**
 * @file      thermal_node.hpp
 * @brief     Thermal-carrier balance node (CSP / district heat)
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Concrete instantiation of the generic ``Node<>`` template for the
 * thermal carrier (MW_th).  Used to anchor:
 *   * ``ThermalStorage`` (molten-salt TES, sensible / latent stores)
 *   * Future ``ThermalGenerator`` (CSP solar field, geothermal,
 *     concentrating heat source)
 *   * Future ``ThermalDemand`` (district heat off-take, defocus
 *     spillage sink)
 *
 * Type-safe: a ``SingleId<ThermalNode>`` cannot accidentally be
 * resolved against an electric ``Bus`` because the two are distinct
 * C++ types.
 *
 * @see node.hpp                      generic template
 * @see thermal_storage.hpp           first user of ThermalNode
 * @see carrier.hpp                   carrier enum
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/node.hpp>

namespace gtopt
{

/**
 * @struct ThermalNode
 * @brief Carrier-tagged balance node for MW_th flows.
 *
 * Inherits all data members from ``Node<Carrier::Thermal>``.
 * Adds only the LP class-name constant used to label thermal balance
 * rows in dumped LP files (e.g.
 * ``thermal_node_balance_<uid>_<scn>_<stg>_<blk>``).
 */
struct ThermalNode : Node<Carrier::Thermal>
{
  /// Canonical class-name used in LP row labels and PAMPL element
  /// resolution.  Matches the JSON ``thermal_node_array`` entry name.
  static constexpr LPClassName class_name {"ThermalNode"};
};

}  // namespace gtopt
