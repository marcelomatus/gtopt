/**
 * @file      reservoir_enums.hpp
 * @brief     Enumerations for reservoir / storage configuration
 * @date      2026-03-30
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <cstdint>

#include <gtopt/enum_option.hpp>

namespace gtopt
{

// ─── EnergyScaleMode ────────────────────────────────────────────────────────

/**
 * @brief How the LP energy-variable scaling factor is determined for storage
 *        elements (reservoirs, batteries).
 *
 * - `manual` (0): Use the explicit `energy_scale` field (default 1.0 if
 *   unset).  This is the legacy behaviour.
 * - `auto_scale` (1, default): Compute energy_scale by rounding emax/1000
 *   up to the next power of 10, so LP variables stay in a well-conditioned
 *   range regardless of physical reservoir size.
 *   Formula: `energy_scale = 10^ceil(log10(emax / 1000))` when emax > 1000,
 *   else 1.0.  Matches PLP practice where ScaleVol values (Escala/1e6,
 *   10^(FEscala-6)) are powers of 10.
 *   An explicit `energy_scale` field on the element overrides auto.
 */
enum class EnergyScaleMode : uint8_t
{
  manual = 0,  ///< Use explicit energy_scale field (legacy, 1.0 default)
  auto_scale = 1,  ///< Compute from emax: max(1.0, emax / 1000) (default)
};

inline constexpr auto energy_scale_mode_entries =
    std::to_array<EnumEntry<EnergyScaleMode>>({
        {.name = "manual", .value = EnergyScaleMode::manual},
        {.name = "auto", .value = EnergyScaleMode::auto_scale},
    });

constexpr auto enum_entries(EnergyScaleMode /*tag*/) noexcept
{
  return std::span {energy_scale_mode_entries};
}

}  // namespace gtopt
