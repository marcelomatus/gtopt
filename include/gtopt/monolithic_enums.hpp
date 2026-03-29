/**
 * @file      monolithic_enums.hpp
 * @brief     Named enum types for monolithic solver options
 * @date      2026-03-29
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines all enum types used by MonolithicOptions.  Extracted from
 * monolithic_options.hpp so that the struct definition stays focused on
 * the option fields.
 */

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <gtopt/enum_option.hpp>

namespace gtopt
{

// --- SolveMode --------------------------------------------------------------

/**
 * @brief Monolithic solver execution mode.
 */
enum class SolveMode : uint8_t
{
  monolithic = 0,  ///< Solve all phases in a single LP (default)
  sequential = 1,  ///< Solve phases sequentially
};

inline constexpr auto solve_mode_entries = std::to_array<EnumEntry<SolveMode>>({
    {.name = "monolithic", .value = SolveMode::monolithic},
    {.name = "sequential", .value = SolveMode::sequential},
});

constexpr auto enum_entries(SolveMode /*tag*/) noexcept
{
  return std::span {solve_mode_entries};
}

}  // namespace gtopt
