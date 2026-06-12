/**
 * @file      hydrogen_node.hpp
 * @brief     Hydrogen-carrier balance node (electrolyser / fuel cell)
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Concrete instantiation of the generic ``Node<>`` template for the
 * hydrogen carrier (MWh_LHV; 1 kg-H₂ ≈ 33.3 kWh-LHV).  Used to anchor
 * the hydrogen-economy topology:
 *
 *   ElectricBus → Electrolyser (Converter, η ≈ 0.7) → HydrogenNode
 *   HydrogenNode → HydrogenStorage (steel-cylinder / salt cavern)
 *   HydrogenNode → FuelCell (Converter, η ≈ 0.5) → ElectricBus
 *   HydrogenNode → Haber-Bosch (Converter, η ≈ 0.5) → AmmoniaNode
 *
 * Type-safe: a ``SingleId<HydrogenNode>`` cannot accidentally resolve
 * against an electric ``Bus`` or an ``AmmoniaNode``.
 *
 * @see node.hpp                generic template
 * @see hydrogen_storage.hpp    first user of HydrogenNode
 * @see ammonia_node.hpp        downstream NH₃ carrier
 * @see carrier.hpp             carrier enum
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/node.hpp>

namespace gtopt
{

/**
 * @struct HydrogenNode
 * @brief Carrier-tagged balance node for hydrogen (MWh_LHV) flows.
 */
struct HydrogenNode : Node<Carrier::Hydrogen>
{
  /// Canonical class-name used in LP row labels and PAMPL element
  /// resolution.  Matches the JSON ``hydrogen_node_array`` entry name.
  static constexpr LPClassName class_name {"HydrogenNode"};
};

}  // namespace gtopt
