/**
 * @file      lp_build_enums.hpp
 * @brief     Named enum types for LP build options
 * @date      2026-03-29
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines all enum types used by LpBuildOptions.  Extracted from
 * lp_build_options.hpp so that the struct definition stays focused on
 * the option fields.
 */

#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <gtopt/enum_option.hpp>

namespace gtopt
{

// --- LpNamesLevel -----------------------------------------------------------

/**
 * @brief LP variable/constraint naming level for matrix assembly.
 *
 * Controls how much naming information is generated during LP construction.
 * Higher levels provide better diagnostics but consume more memory.
 *
 * - `minimal`:      State-variable column names only (for internal use,
 *                   e.g. cascade solver state transfer).  Smallest footprint.
 * - `only_cols`:    All column names + name-to-index maps.
 * - `cols_and_rows`: Column + row names + maps.  Warns on duplicate names.
 */
enum class LpNamesLevel : uint8_t
{
  minimal = 0,  ///< State-variable column names only (default)
  only_cols = 1,  ///< All column names + name maps
  cols_and_rows = 2,  ///< Column + row names + maps + warn on duplicates
};

inline constexpr auto lp_names_level_entries =
    std::to_array<EnumEntry<LpNamesLevel>>({
        {.name = "minimal", .value = LpNamesLevel::minimal},
        {.name = "only_cols", .value = LpNamesLevel::only_cols},
        {.name = "cols_and_rows", .value = LpNamesLevel::cols_and_rows},
    });

constexpr auto enum_entries(LpNamesLevel /*tag*/) noexcept
{
  return std::span {lp_names_level_entries};
}

}  // namespace gtopt
