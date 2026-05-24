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
 *
 * - `piecewise_direct` (6): PLP-faithful two-direction piecewise-linear
 *   model for lines **without capacity expansion**.  Per direction, K
 *   segment variables (bound `[0, tmax_dir/K]`) inject directly into
 *   the bus-balance rows with the per-segment loss factor
 *   `λ_k = (tmax_dir/K) · (2k−1) · R / V²` baked into the coefficients
 *   (PLP `genpdlin.f`).  There is no loss variable, no loss-tracking
 *   row, no aggregator column, and no flow-link row: each segment also
 *   stamps directly into the Kirchhoff (KVL) row with `±x_τ`, so the
 *   identity `Σ seg_k = |f|` is recovered without an explicit equality.
 *   This produces the most compact LP (2K cols, 0 extra rows per block
 *   per line) and matches the row count of PLP exactly.
 *   Trade-off: the `line.flowp` / `line.flown` solution columns are
 *   *not emitted* for piecewise_direct lines — the AMPL compound
 *   `line.flow` is unavailable for these lines (use `piecewise` if you
 *   need it).
 *   Activation: opt-in only — `adaptive` prefers `piecewise` (smaller
 *   LP per block, but more *rows*).  Select explicitly for PLP-diff
 *   parity or to halve the LP size on transmission-heavy cases.  On
 *   expandable lines this mode falls back to `piecewise` with a
 *   warning.
 *   Ref: PLP Fortran `genpdlin.f` (GenPDLinA).
 */
enum class LineLossesMode : uint8_t
{
  none = 0,  ///< No losses modeled
  linear = 1,  ///< Lumped linear loss factor
  piecewise = 2,  ///< Single-direction piecewise-linear (PLP-style)
  bidirectional = 3,  ///< Two-direction piecewise-linear (gtopt legacy)
  adaptive = 4,  ///< Piecewise if fixed capacity, bidirectional if expandable
  dynamic = 5,  ///< Dynamic piecewise-linear (future; falls back to piecewise)
  piecewise_direct = 6,  ///< PLP-style compact PWL (no loss vars/rows)
};

inline constexpr auto line_losses_mode_entries =
    std::to_array<EnumEntry<LineLossesMode>>({
        {.name = "none", .value = LineLossesMode::none},
        {.name = "linear", .value = LineLossesMode::linear},
        {.name = "piecewise", .value = LineLossesMode::piecewise},
        {.name = "bidirectional", .value = LineLossesMode::bidirectional},
        {.name = "adaptive", .value = LineLossesMode::adaptive},
        {.name = "dynamic", .value = LineLossesMode::dynamic},
        {.name = "piecewise_direct", .value = LineLossesMode::piecewise_direct},
    });

[[nodiscard]] constexpr auto enum_entries(LineLossesMode /*tag*/) noexcept
{
  return std::span {line_losses_mode_entries};
}

// ─── LinePwlLayout ──────────────────────────────────────────────────

/**
 * @brief Segment-layout strategy for the piecewise-linear loss model.
 *
 * Applies only when `line_losses_mode` is `piecewise`, `bidirectional`,
 * `adaptive`, `dynamic`, or `piecewise_direct`.  Controls **where the
 * breakpoints are placed** along `[0, envelope]` and **what kind of
 * lines** (secant chords vs. tangent supports) approximate the
 * quadratic loss curve `ℓ(f) = (R/V²)·f²`.
 *
 * - `uniform` (0, default): equal-width secant chords.  Breakpoints at
 *   `b_k = (k/K)·envelope`; each segment's chord slope is
 *   `(R/V²)·width·(2k−1)`.  Backwards-compatible with all bundles
 *   that don't carry `loss_pwl_layout`.  Worst-case chord error scales
 *   as `(envelope/K)²/4`, peaking on the OUTER segment.
 *
 * - `equal_error` (1): √-spaced secant chords (minimax).  Breakpoints
 *   at `b_k = √(k/K)·envelope`; each segment carries the SAME max
 *   chord error.  Drop-in replacement for `uniform`: same K, same LP
 *   row count, but max chord error drops by a factor of ~√K.
 *   Recommended when you want better accuracy at no LP-size cost.
 *
 * - `tangent` (2): outer-approximation via K tangent lines (future).
 *   Loss is bounded below by the maximum of K tangents at points
 *   `t_k = ((2k−1)/(2K))·envelope`.  Yields a LOWER bound on losses
 *   that's tight at the LP's chosen operating point.  Currently
 *   reserved; falls back to `uniform` with a one-time log warning
 *   until the alternative LP structure (1 col + K rows per (line,
 *   block) instead of K cols + 1 row) is wired up.
 *
 * Theory references:
 *   - Imamura et al., *On piecewise linear approximation of quadratic
 *     functions* (2003).
 *   - Aigner & Van Hentenryck, *Line Loss Outer Approximation*,
 *     arXiv:2112.10975 (2022) — for the `tangent` mode.
 *   - Fitiwi et al., *Dynamic PWL DC Transmission Losses*, IEEE 2010.
 */
enum class LinePwlLayout : uint8_t
{
  uniform = 0,  ///< Equal-width secant chords (default; current behavior)
  equal_error = 1,  ///< √-spaced secant chords (minimax error)
  tangent =
      2,  ///< Outer-approximation tangents (future; falls back to uniform)
};

inline constexpr auto line_pwl_layout_entries =
    std::to_array<EnumEntry<LinePwlLayout>>({
        {.name = "uniform", .value = LinePwlLayout::uniform},
        {.name = "equal_error", .value = LinePwlLayout::equal_error},
        {.name = "tangent", .value = LinePwlLayout::tangent},
    });

[[nodiscard]] constexpr auto enum_entries(LinePwlLayout /*tag*/) noexcept
{
  return std::span {line_pwl_layout_entries};
}

// ─── KirchhoffMode ──────────────────────────────────────────────────

/**
 * @brief Selects the Kirchhoff Voltage Law (KVL) formulation.
 *
 * - `node_angle` (0): the classical B–θ form.  One bus angle
 *   (theta) variable per active bus, plus one KVL equality per active
 *   line per block:
 *     `−θ_a + θ_b + x_τ · f_p − x_τ · f_n = −φ`
 *   where `x_τ = τ · X / V²`.  A reference bus per island has its
 *   theta pinned at 0 to fix the gauge.  Required for line expansion
 *   that changes topology per stage and for phase-shift transformers.
 *
 * - `cycle_basis` (1, default): the loop-flow form — eliminates theta
 *   entirely and replaces per-line KVL with one equality per
 *   fundamental cycle:
 *     `Σ_{l∈C} ε_l · x_l · (f_p − f_n)  =  Σ_{l∈C} ε_l · φ_l`
 *   with `ε_l ∈ {+1,−1}` the cycle direction sign.  No reference-bus
 *   pin, no theta column scale to tune, typically `(|L| − |B| + #islands)`
 *   rows per block instead of `|L|` — strictly smaller LP for meshed
 *   grids.  PyPSA-style; reference: Hörsch et al., "PyPSA: Python for
 *   Power System Analysis", 2018.
 *
 * Default: `cycle_basis`.  Selected via `model_options.kirchhoff_mode`
 * (string) or the `--kirchhoff-mode` CLI flag.  See
 * `kirchhoff_node_angle.hpp` for the row-assembly helper.
 */
enum class KirchhoffMode : uint8_t
{
  node_angle = 0,  ///< B–θ formulation (per-line KVL row, gauge-pinned)
  cycle_basis = 1,  ///< Loop-flow formulation (per-cycle KVL row, no theta)
};

inline constexpr auto kirchhoff_mode_entries =
    std::to_array<EnumEntry<KirchhoffMode>>({
        {.name = "node_angle", .value = KirchhoffMode::node_angle},
        {.name = "cycle_basis", .value = KirchhoffMode::cycle_basis},
    });

[[nodiscard]] constexpr auto enum_entries(KirchhoffMode /*tag*/) noexcept
{
  return std::span {kirchhoff_mode_entries};
}

}  // namespace gtopt
