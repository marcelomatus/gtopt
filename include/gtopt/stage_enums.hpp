/**
 * @file      stage_enums.hpp
 * @brief     Named enum types for stage configuration
 * @date      2026-04-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines enum types used by Stage: calendar month, etc.
 */

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <gtopt/enum_option.hpp>

namespace gtopt
{

// --- MonthType ---------------------------------------------------------------

/**
 * @brief Calendar month (1-based: january=1, december=12).
 *
 * Used in Stage to tag each planning period with its calendar month,
 * enabling seasonal parameter lookups (e.g., irrigation schedules).
 */
enum class MonthType : uint8_t
{
  january = 1,
  february = 2,
  march = 3,
  april = 4,
  may = 5,
  june = 6,
  july = 7,
  august = 8,
  september = 9,
  october = 10,
  november = 11,
  december = 12,
};

/**
 * @brief Name-value table for MonthType.
 *
 * Accepts:
 *
 *   - English full names (``"january"``, ``"february"``, ...).
 *     These come first so reverse lookup (``enum_name``) returns the
 *     canonical English name.
 *   - Spanish full names (``"enero"``, ``"febrero"``, ...), plus the
 *     alternate spelling ``"setiembre"`` for September.
 *   - English 3-letter abbreviations (``"jan"``, ``"feb"``, ...).
 *   - Spanish 3-letter abbreviations (``"ene"``, ``"feb"``, ...);
 *     ``"feb"`` maps to the same value as its English twin, so only one
 *     entry is needed for the shared abbreviations.
 *
 * Comparison is ASCII case-insensitive (see ``enum_option.hpp``), so
 * ``"January"``, ``"JANUARY"``, ``"Enero"`` and ``"ENE"`` all resolve to
 * ``MonthType::january``.
 */
inline constexpr auto month_type_entries = std::to_array<EnumEntry<MonthType>>({
    // Canonical English full names (kept first for reverse lookup).
    {.name = "january", .value = MonthType::january},
    {.name = "february", .value = MonthType::february},
    {.name = "march", .value = MonthType::march},
    {.name = "april", .value = MonthType::april},
    {.name = "may", .value = MonthType::may},
    {.name = "june", .value = MonthType::june},
    {.name = "july", .value = MonthType::july},
    {.name = "august", .value = MonthType::august},
    {.name = "september", .value = MonthType::september},
    {.name = "october", .value = MonthType::october},
    {.name = "november", .value = MonthType::november},
    {.name = "december", .value = MonthType::december},
    // Spanish full names.
    {.name = "enero", .value = MonthType::january},
    {.name = "febrero", .value = MonthType::february},
    {.name = "marzo", .value = MonthType::march},
    {.name = "abril", .value = MonthType::april},
    {.name = "mayo", .value = MonthType::may},
    {.name = "junio", .value = MonthType::june},
    {.name = "julio", .value = MonthType::july},
    {.name = "agosto", .value = MonthType::august},
    {.name = "septiembre", .value = MonthType::september},
    {.name = "setiembre", .value = MonthType::september},
    {.name = "octubre", .value = MonthType::october},
    {.name = "noviembre", .value = MonthType::november},
    {.name = "diciembre", .value = MonthType::december},
    // English 3-letter abbreviations.
    {.name = "jan", .value = MonthType::january},
    {.name = "feb", .value = MonthType::february},
    {.name = "mar", .value = MonthType::march},
    {.name = "apr", .value = MonthType::april},
    {.name = "jun", .value = MonthType::june},
    {.name = "jul", .value = MonthType::july},
    {.name = "aug", .value = MonthType::august},
    {.name = "sep", .value = MonthType::september},
    {.name = "oct", .value = MonthType::october},
    {.name = "nov", .value = MonthType::november},
    {.name = "dec", .value = MonthType::december},
    // Spanish 3-letter abbreviations (only the ones that differ from the
    // English set — "feb", "mar", "jun", "jul", "oct", "nov" are shared).
    {.name = "ene", .value = MonthType::january},
    {.name = "abr", .value = MonthType::april},
    {.name = "ago", .value = MonthType::august},
    {.name = "set", .value = MonthType::september},
    {.name = "dic", .value = MonthType::december},
});

[[nodiscard]] constexpr auto enum_entries(MonthType /*tag*/) noexcept
{
  return std::span {month_type_entries};
}

}  // namespace gtopt
