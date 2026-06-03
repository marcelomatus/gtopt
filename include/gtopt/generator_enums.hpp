/**
 * @file      generator_enums.hpp
 * @brief     Generator-class enums (CogenMode)
 * @date      2026-06-03
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Mirrors the ``line_enums.hpp`` / ``planning_enums.hpp`` pattern: the JSON-
 * facing field stays as ``OptName`` (a string) and the C++ side reaches the
 * typed enum via ``Generator::cogen_mode_enum()`` which delegates to the
 * generic ``enum_from_name<CogenMode>`` lookup.
 */

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <gtopt/enum_option.hpp>

namespace gtopt
{

// ─── CogenMode ──────────────────────────────────────────────────────

/**
 * @brief Cogeneration-unit modelling mode.
 *
 * Stored on ``Generator.cogen_mode`` as an ``OptName`` (case-insensitive
 * JSON string).  The mere presence of a value tags the unit as a cogen
 * regardless of any name-pattern heuristic; the value selects the LP
 * bound-shape contract:
 *
 *   * ``dispatched`` — the LP dispatches the unit freely on cost,
 *     identically to a normal thermal (``pmin``/``pmax`` left at their
 *     rated values).  Downstream tools (gtopt_marginal_units' merit-walk-up,
 *     audit dashboards) still skip the unit as "not a real market
 *     competitor" because its physical role is industrial heat supply.
 *
 *   * ``must_run`` — the unit is pinned via ``pmin == pmax`` (the
 *     converter sets both bounds to the cogen's heat-bound dispatch
 *     trajectory, scalar or scheduled).  Pin hardness is controlled by
 *     the existing ``pmin_fcost`` field — unset = hard pin (LP
 *     infeasibility on conflict); set = soft pin (slack at the penalty
 *     cost via the standard unserved-slack column).
 *
 * Future cogen-LP work (heat-electricity coupling, multi-mode operation)
 * will land as additional enumerators here, never as new ``OptName``
 * values silently parsed.  Side-channel metadata that doesn't drive LP
 * behavior belongs in the description meta block, not as new cogen_mode
 * values.
 */
enum class CogenMode : uint8_t
{
  dispatched = 0,  ///< LP-free dispatch (pmin/pmax untouched)
  must_run = 1,  ///< pmin == pmax pin (hardness via pmin_fcost)
};

inline constexpr auto cogen_mode_entries = std::to_array<EnumEntry<CogenMode>>({
    {.name = "dispatched", .value = CogenMode::dispatched},
    {.name = "must_run", .value = CogenMode::must_run},
});

[[nodiscard]] constexpr auto enum_entries(CogenMode /*tag*/) noexcept
{
  return std::span {cogen_mode_entries};
}

}  // namespace gtopt
