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

inline constexpr auto month_type_entries = std::to_array<EnumEntry<MonthType>>({
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
});

constexpr auto enum_entries(MonthType /*tag*/) noexcept
{
  return std::span {month_type_entries};
}

}  // namespace gtopt
