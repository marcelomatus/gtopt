/**
 * @file      carrier.hpp
 * @brief     Energy-carrier tag for ``Node<>`` templated balance nodes
 * @date      Fri May 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the ``Carrier`` enum used as the template parameter for the
 * generic ``Node<C>`` balance-node template (see ``node.hpp``).  The
 * enum is the single source of truth for which energy carrier a node
 * (and any storage / generator / converter attached to it) operates
 * on.  Cross-carrier wiring is rejected at compile time by the C++
 * type system — there are no runtime ``carrier`` string compares.
 *
 * Reserved values:
 *   * ``Electric`` — the legacy ``Bus`` class will be refactored to
 *     ``Node<Carrier::Electric>`` in a follow-up pass.
 *   * ``Water``    — the legacy ``Junction`` class will be refactored
 *     to ``Node<Carrier::Water>`` in a follow-up pass.
 *
 * New carriers (``Hydrogen``, ``Thermal``) get their own typed
 * ``HydrogenNode`` / ``ThermalNode`` concrete classes that inherit
 * from the generic template — see ``hydrogen_node.hpp`` and
 * ``thermal_node.hpp``.
 */

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include <gtopt/enum_option.hpp>

namespace gtopt
{

/// Energy carrier tag.  Each value identifies a distinct balance
/// network: an electric grid (MW_e), a water cascade (m³/s), a
/// thermal stream (MW_th), or a hydrogen network (kg_H₂ ≡ MWh_LHV).
/// Carriers do not mix — a ``Generator`` on a ``Carrier::Thermal``
/// node cannot supply a ``Demand`` on a ``Carrier::Electric`` node
/// except through an explicit ``Converter``.
enum class Carrier : std::uint8_t
{
  Electric = 0,  ///< Electricity (MW_e).  Reserved for future Bus refactor.
  Water = 1,  ///< Water flow (m³/s).  Reserved for future Junction refactor.
  Hydrogen = 2,  ///< Hydrogen energy (MWh_LHV); 1 kg-H₂ ≈ 33.3 kWh-LHV.
  Thermal = 3,  ///< Thermal energy (MW_th).  CSP / district heat.
  Ammonia = 4,  ///< Ammonia energy (MWh_LHV); 1 kg-NH₃ ≈ 5.17 kWh-LHV.
                ///< Long-term H₂ carrier (Haber-Bosch synthesis,
                ///< NH₃ cracking back to H₂ for end-use).  Easier to
                ///< liquefy (-33 °C @ 1 atm) and ship than H₂.
};

/// Human-readable name of a carrier (for diagnostics / error
/// messages).  Returned as a ``std::string_view`` so it can be used
/// in ``std::format`` calls without allocation.
[[nodiscard]] constexpr std::string_view to_string(Carrier c) noexcept
{
  switch (c) {
    case Carrier::Electric:
      return "electric";
    case Carrier::Water:
      return "water";
    case Carrier::Hydrogen:
      return "hydrogen";
    case Carrier::Thermal:
      return "thermal";
    case Carrier::Ammonia:
      return "ammonia";
  }
  return "unknown";
}

/// NamedEnum table — lets ``Carrier`` round-trip through JSON via
/// ``json_string_enum<>``.  See ``enum_option.hpp`` for the framework.
inline constexpr auto carrier_entries = std::to_array<EnumEntry<Carrier>>({
    {.name = "electric", .value = Carrier::Electric},
    {.name = "water", .value = Carrier::Water},
    {.name = "hydrogen", .value = Carrier::Hydrogen},
    {.name = "thermal", .value = Carrier::Thermal},
    {.name = "ammonia", .value = Carrier::Ammonia},
});

[[nodiscard]] constexpr auto enum_entries(Carrier) noexcept
{
  return std::span {carrier_entries};
}

}  // namespace gtopt
