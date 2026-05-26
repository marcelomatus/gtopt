/**
 * @file      ammonia_node.hpp
 * @brief     Ammonia-carrier balance node (long-term H₂ storage)
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Concrete instantiation of the generic ``Node<>`` template for the
 * ammonia carrier (MWh_LHV; 1 kg-NH₃ ≈ 5.17 kWh-LHV).  Ammonia is the
 * dominant long-term hydrogen carrier in the green-hydrogen
 * literature: easier to liquefy (-33 °C @ 1 atm vs -253 °C for LH₂),
 * higher volumetric energy density, and an existing global shipping /
 * pipeline / port infrastructure.
 *
 *   HydrogenNode → Haber-Bosch synthesis  → AmmoniaNode
 *   AmmoniaNode  → AmmoniaStorage (refrigerated / pressurised tanks)
 *   AmmoniaNode  → NH₃ cracker → HydrogenNode (round-trip η ≈ 0.6)
 *   AmmoniaNode  → direct NH₃ combustion (CCGT, marine fuel)
 *
 * Energy is bookkept in MWh_LHV so a single Carrier::Ammonia node
 * sums consistently in either direction (synthesis adds, cracking +
 * combustion remove).  Mass-balance and process-side losses are
 * captured by Converter efficiencies, not on the node itself.
 *
 * Sources:
 *   * Acar & Dincer (2018) — *Comparative review of NH₃ for energy
 *     storage and transport*.
 *   * NREL (2023) — *Techno-Economic Analysis of Off-Grid Green
 *     Ammonia Production*, NREL/TP-5400-89569.
 *   * Hayer et al. (2024) — *Modelling of green ammonia production
 *     based on solid oxide cells for Haber-Bosch decarbonisation*.
 *
 * @see node.hpp                generic template
 * @see ammonia_storage.hpp     first user of AmmoniaNode
 * @see hydrogen_node.hpp       upstream H₂ carrier
 * @see carrier.hpp             carrier enum
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/node.hpp>

namespace gtopt
{

/**
 * @struct AmmoniaNode
 * @brief Carrier-tagged balance node for ammonia (MWh_LHV) flows.
 */
struct AmmoniaNode : Node<Carrier::Ammonia>
{
  /// Canonical class-name used in LP row labels and PAMPL element
  /// resolution.  Matches the JSON ``ammonia_node_array`` entry name.
  static constexpr LPClassName class_name {"AmmoniaNode"};
};

}  // namespace gtopt
