/**
 * @file      line_enums.hpp
 * @brief     Enumerations for transmission line configuration
 * @date      2026-03-30
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <cstdint>

#include <gtopt/enum_option.hpp>

namespace gtopt
{

// ─── LossAllocationMode ──────────────────────────────────────────────

/**
 * @brief How transmission losses are allocated between sender and
 *        receiver buses in the nodal power balance equation.
 *
 * The total loss is always `λ·f` (conserving energy).  The allocation
 * determines which bus "absorbs" the loss in the KCL equation via
 * emission (PerdEms) and reception (PerdRec) fractions where
 * PerdEms + PerdRec = 1:
 *
 * - `receiver` (0, default): sender = −f, receiver = +(1 − λ)f.
 *   PerdEms=0, PerdRec=1.
 * - `sender` (1): sender = −(1 + λ)f, receiver = +f.
 *   PerdEms=1, PerdRec=0.  (PLP `FPerdLin = 'E'`)
 * - `split` (2): sender = −(1 + λ/2)f, receiver = +(1 − λ/2)f.
 *   PerdEms=0.5, PerdRec=0.5.  (PLP `FPerdLin = 'M'`)
 */
enum class LossAllocationMode : uint8_t
{
  receiver = 0,  ///< 100% losses at receiving bus (default)
  sender = 1,  ///< 100% losses at sending bus (PLP 'E')
  split = 2,  ///< 50/50 between sender and receiver (PLP 'M')
};

inline constexpr auto loss_allocation_mode_entries =
    std::to_array<EnumEntry<LossAllocationMode>>({
        {.name = "receiver", .value = LossAllocationMode::receiver},
        {.name = "sender", .value = LossAllocationMode::sender},
        {.name = "split", .value = LossAllocationMode::split},
    });

constexpr auto enum_entries(LossAllocationMode /*tag*/) noexcept
{
  return std::span {loss_allocation_mode_entries};
}

}  // namespace gtopt
