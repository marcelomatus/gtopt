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
 *   PerdEms=1, PerdRec=0.  (PLP FPerdLin = 'E')
 * - `split` (2): sender = −(1 + λ/2)f, receiver = +(1 − λ/2)f.
 *   PerdEms=0.5, PerdRec=0.5.  (PLP FPerdLin = 'M')
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

[[nodiscard]] constexpr auto enum_entries(LossAllocationMode /*tag*/) noexcept
{
  return std::span {loss_allocation_mode_entries};
}

// ─── LineLossesMode ─────────────────────────────────────────────────

/**
 * @brief Selects the mathematical model for transmission line losses.
 *
 * Each mode trades off accuracy, LP size, and solver compatibility:
 *
 * - `none` (0): No losses.  Flow balance: `P_send = P_recv`.
 *   Simplest model; suitable when losses are negligible or handled
 *   externally.
 *
 * - `linear` (1): Lumped linear loss factor `λ`.
 *   `P_loss = λ · |f|`.  If the line has `resistance > 0` and
 *   `voltage > 0` but no explicit `lossfactor`, one is auto-computed
 *   as `λ = R · f_max / V²` (linearization at rated flow of the
 *   quadratic curve `P_loss = R · f² / V²`).
 *   Zero extra LP rows; loss enters as bus-balance coefficients.
 *   Ref: Wood & Wollenberg, "Power Generation, Operation and Control",
 *        Ch. 13, incremental transmission losses.
 *
 * - `piecewise` (2): Single-direction piecewise-linear approximation
 *   of `P_loss = R · f² / V²`.  One set of K segments covers the
 *   absolute flow range `[0, f_max]`, producing K segment variables,
 *   1 linking row, and 1 loss-tracking row per block.  Segment k
 *   (1-based) has loss coefficient `(f_max/K) · R · (2k−1) / V²`.
 *   This is the PLP/PSR approach and produces half the rows of the
 *   `bidirectional` mode.
 *   Ref: Macedo, Vallejos, Fernández, "A Dynamic Piecewise Linear
 *        Model for DC Transmission Losses in Optimal Scheduling
 *        Problems", IEEE Trans. Power Syst., vol. 26, no. 1, 2011.
 *
 * - `bidirectional` (3): Two independent piecewise-linear models,
 *   one per flow direction (A→B and B→A).  Each direction gets its
 *   own K segment variables + linking + loss-tracking rows.
 *   Produces 2× the rows and columns of `piecewise` but explicitly
 *   prevents simultaneous bidirectional flow through non-negative
 *   variable bounds.
 *   Ref: FERC Staff Paper, "Optimal Power Flow Paper 2: Linearization",
 *        December 2012.
 *
 * - `adaptive` (4): Automatically selects `piecewise` for lines with
 *   fixed capacity (no expansion modules) and `bidirectional` for
 *   expandable lines.  Fixed-capacity lines can encode `f ≤ tmax` in
 *   segment variable bounds, so a single-direction model suffices.
 *   Expandable lines need explicit capacity rows (`cap − f ≥ 0`)
 *   per direction, requiring the bidirectional decomposition.
 *   This is the recommended default for mixed networks.
 *
 * - `dynamic` (5): Iteratively adjusted piecewise-linear segments
 *   that track the current operating point.  Achieves higher accuracy
 *   with fewer segments than static PWL.  Currently a placeholder
 *   that falls back to `piecewise` with a log warning.
 *   Ref: Macedo et al. (2011), §III — dynamic cut adjustment.
 */
enum class LineLossesMode : uint8_t
{
  none = 0,  ///< No losses modeled
  linear = 1,  ///< Lumped linear loss factor
  piecewise = 2,  ///< Single-direction piecewise-linear (PLP-style)
  bidirectional = 3,  ///< Two-direction piecewise-linear (gtopt legacy)
  adaptive = 4,  ///< Piecewise if fixed capacity, bidirectional if expandable
  dynamic = 5,  ///< Dynamic piecewise-linear (future; falls back to piecewise)
};

inline constexpr auto line_losses_mode_entries =
    std::to_array<EnumEntry<LineLossesMode>>({
        {.name = "none", .value = LineLossesMode::none},
        {.name = "linear", .value = LineLossesMode::linear},
        {.name = "piecewise", .value = LineLossesMode::piecewise},
        {.name = "bidirectional", .value = LineLossesMode::bidirectional},
        {.name = "adaptive", .value = LineLossesMode::adaptive},
        {.name = "dynamic", .value = LineLossesMode::dynamic},
    });

[[nodiscard]] constexpr auto enum_entries(LineLossesMode /*tag*/) noexcept
{
  return std::span {line_losses_mode_entries};
}

}  // namespace gtopt
