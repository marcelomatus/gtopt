/**
 * @file      sddp_cut_store_enums.hpp
 * @brief     Enumerations for SDDP Benders cut storage
 * @date      2026-03-30
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <cstdint>

#include <gtopt/enum_option.hpp>

namespace gtopt
{

// ─── CutType ────────────────────────────────────────────────────────────────

/// Type of Benders cut: optimality (standard) or feasibility (elastic filter)
enum class CutType : uint8_t
{
  Optimality = 0,  ///< Standard Benders optimality cut
  Feasibility,  ///< Feasibility cut from elastic filter
};

inline constexpr auto cut_type_entries = std::to_array<EnumEntry<CutType>>({
    {.name = "optimality", .value = CutType::Optimality},
    {.name = "feasibility", .value = CutType::Feasibility},
});

constexpr auto enum_entries(CutType /*tag*/) noexcept
{
  return std::span {cut_type_entries};
}

}  // namespace gtopt
