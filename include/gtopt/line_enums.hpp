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
 * - `piecewise` (2): Piecewise-linear approximation of
 *   `P_loss = R · f² / V²`.  The historical single-direction
 *   implementation used one shared K-segment set covering `[0, f_max]`
 *   with link row `fp + fn − Σ seg_k = 0` and a single shared `loss`
 *   column charged once to one bus.  As of 2026-05-31 `piecewise` is
 *   implemented as a thin wrapper around `bidirectional` for every
 *   non-`tangent` PWL layout: the single-direction shared-loss
 *   formulation is structurally vulnerable to a phantom-flow
 *   arbitrage (`fp · fn > 0` while only one bus pays loss) that
 *   inflates total system loss in meshed networks with negative-LMP
 *   receiving buses.  Selecting `piecewise` therefore now produces
 *   the same LP shape as `bidirectional` (2 × K segment columns,
 *   per-direction link + loss-tracking rows, per-direction loss
 *   column).  The legacy single-direction code path is kept only for
 *   `LinePwlLayout::tangent`, which has no per-direction counterpart.
 *   Segment k (1-based) has loss coefficient
 *   `(f_max/K) · R · (2k−1) / V²` (`uniform` layout).
 *   Ref: Macedo, Vallejos, Fernández, "A Dynamic Piecewise Linear
 *        Model for DC Transmission Losses in Optimal Scheduling
 *        Problems", IEEE Trans. Power Syst., vol. 26, no. 1, 2011;
 *        FERC Staff Paper, "Optimal Power Flow Paper 2:
 *        Linearization", December 2012 (bidirectional shape).
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
 * - `adaptive` (4): Automatically selects an arbitrage-free PWL mode
 *   based on whether the line carries capacity expansion:
 *     - has expansion              → `bidirectional` (capacity rows
 *                                     need the per-direction decomposition)
 *     - no expansion + any KVL     → `piecewise` (which itself wraps
 *                                     `bidirectional` for non-tangent
 *                                     layouts; both produce identical LP).
 *   The previous shortcut that routed `cycle_basis + no-expansion` to
 *   `piecewise_direct` has been retired because `piecewise_direct` is
 *   not phantom-flow safe on meshed networks with negative-LMP
 *   receivers (see `piecewise_direct` docstring below).  Recommended
 *   default for any GTEP case that may produce negative receiver LMPs
 *   (curtailment-priced demand, must-dispatch surplus, congestion).
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
 *   stamps directly into the Kirchhoff (KVL) row with `±x_τ`.
 *   This produces the most compact LP (2K cols, 0 extra rows per block
 *   per line) and matches the row count of PLP exactly.
 *
 *   ⚠ Phantom-flow caveat: because there is NO link row and NO
 *   `fp/fn` aggregator, the LP has no structural barrier preventing
 *   simultaneous non-zero positive- AND negative-direction segments.
 *   Empirically on CEN PCP v0407, `piecewise_direct` is the WORST
 *   mode for phantom bidirectional flow.  In meshed networks with
 *   blocks where the receiving bus has negative LMP, the LP can
 *   inflate both directions to dump quadratic loss "for free" at the
 *   negative-LMP bus.  Use `bidirectional` (or `piecewise`, which now
 *   wraps `bidirectional`) instead when phantom-flow purity matters
 *   more than LP row count.  `piecewise_direct` is only safe in
 *   networks that never see negative LMPs at receiving buses (PLP's
 *   historical operating regime).
 *
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
 *
 * - `tangent_signed_flow` (7): Coffrin-style outer approximation on a
 *   SINGLE signed flow variable.  Per (line, block) the LP carries:
 *     * one signed flow column `f ∈ [−tmax_ba, +tmax_ab]` (no
 *       `flowp`/`flown` decomposition; KVL uses `f` directly);
 *     * one loss column `ℓ ∈ [0, (R/V²)·fmax²]` (the quadratic upper
 *       envelope on `[−fmax, +fmax]`, exact at the rated flow);
 *     * `K` tangent inequalities of the form
 *         `ℓ ≥ (2·R/V²)·f_k·f − (R/V²)·f_k²`,    k = 1..K
 *       where `f_k` are uniformly spread (midpoint placement)
 *       across `[−fmax, +fmax]`:
 *         `f_k = fmax · (2k − K − 1) / K`.
 *
 *   Each tangent is the gradient of the quadratic loss curve at
 *   `f = f_k`; the K-fold maximum is a piecewise-affine LOWER
 *   approximation of `ℓ(f) = (R/V²)·f²`, exact at every tangent point
 *   and within `(fmax/K)²·R/V²` of the curve at the partition
 *   boundaries.  Combined with the upper bound on `ℓ`, the LP's
 *   feasible loss region tightly brackets the true quadratic.
 *
 *   ✔ **Phantom-flow IMPOSSIBLE by construction**: there is no
 *   `fp`/`fn` decomposition, so the LP has no way to express
 *   simultaneous bidirectional flow on a line.  KVL stamps the single
 *   signed `f` column with its natural `±x_τ` coefficient.
 *   ⚠ Loss-side arbitrage is still OPEN under a negative bus-dual
 *   pair-sum: the abs-flow proxy `v` is only lower-bounded by `±f`,
 *   so the LP can inflate `v` past `|f|` and ride the loss up the
 *   chord row (idle line → sink of `c·env²` MW).  `loss_cost_eps`
 *   (on the loss column, ≥ ½·|worst pair-sum|) closes it at
 *   loss-scaled cost (see `line.hpp`); the exact fix is the SOS2
 *   λ-form.  Demonstrated in
 *   `test_line_losses_negative_lmp_kvl.cpp`.
 *
 *   LP size (per line per block): **(2+L) cols + (K+3) rows**
 *   (signed flow, loss column, L abs-flow `v` cols; K tangent rows —
 *   the zero-slope middle tangent is DROPPED when K is odd, so prefer
 *   EVEN K — plus 2 abs rows and 1 chord row).  At K=5, L=1 that's
 *   `3 + 7 = 10` elements vs `bidirectional` at K=5 (`14 + 4 = 18`)
 *   — roughly 44 % fewer while preserving the convex-quadratic shape.
 *
 *   Works with BOTH `node_angle` and `cycle_basis` Kirchhoff modes
 *   (the signed `f` column drops directly into the KVL row with
 *   `+x_τ` instead of the `+x_τ·fp − x_τ·fn` pair).
 *
 *   Activation: opt-in only via explicit `line_losses_mode =
 *   "tangent_signed_flow"`.  The `adaptive` resolver does NOT route
 *   here (its decision tree is unchanged); a future PR can lift the
 *   constraint after wider-system validation on negative-LMP and
 *   expansion-line workloads.
 *
 *   Refs:
 *   - Coffrin & Van Hentenryck, *A Linear-Programming Approximation of
 *     AC Power Flows*, INFORMS J. Computing, 2014 (arXiv:1206.3614).
 *   - Aigner & Van Hentenryck, *Line Loss Outer Approximation*,
 *     arXiv:2112.10975 (2022).
 */
enum class LineLossesMode : uint8_t
{
  none = 0,  ///< No losses modeled
  linear = 1,  ///< Lumped linear loss factor
  piecewise = 2,  ///< Per-direction PWL (wraps bidirectional; PLP-named)
  bidirectional = 3,  ///< Two-direction piecewise-linear
  adaptive = 4,  ///< Piecewise if fixed capacity, bidirectional if expandable
  dynamic = 5,  ///< Dynamic piecewise-linear (future; falls back to piecewise)
  piecewise_direct =
      6,  ///< PLP-style compact PWL (NOT arbitrage-proof, see docstring)
  tangent_signed_flow =
      7,  ///< Coffrin outer-approximation; single signed flow column
          ///< (NO fp/fn) + K tangent rows; phantom-flow impossible.
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
        {.name = "tangent_signed_flow",
         .value = LineLossesMode::tangent_signed_flow},
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
 * - `midpoint` (3): de-biased secant chords.  Same K segment columns,
 *   same chord SLOPES `(R/V²)·width·(2k−1)`, and the same single
 *   loss-tracking row as `uniform` — but the row is an INEQUALITY
 *   `loss ≥ Σ loss_k·seg_k − (R/V²)·(width/2)²` so the whole PWL is
 *   shifted DOWN by the constant `(R/V²)·width²/4`.  This turns each
 *   secant chord into the tangent of `ℓ(f)=(R/V²)·f²` at the segment
 *   MIDPOINT `m_k=(2k−1)·width/2`.  Because adjacent midpoint tangents
 *   intersect EXACTLY at the breakpoints `b_k=k·width`, the offset is a
 *   FLAT constant `(R/V²)·width²/4` for every flow `f>0` (it does NOT
 *   accumulate per segment).  Result: an UNBIASED estimator — exact at
 *   the segment midpoints, at most `(R/V²)·width²/4` UNDER at the
 *   breakpoints — versus `uniform`'s strict (all-positive) overstatement
 *   of up to `(R/V²)·width²/4` at the midpoints.  Removes the systematic
 *   loss-overstatement of the secant layout while keeping the exact LP
 *   structure (K cols + 1 row).  `uniform` is preserved as the default
 *   for callers that need a guaranteed upper bound on losses.
 *
 *   ⚠ Negative-LMP caveat: the inequality `loss ≥ …` makes `loss` a
 *   free variable above its lower bound, so in blocks where the
 *   receiver bus has a NEGATIVE LMP the LP can pick `loss` arbitrarily
 *   high (up to whatever upstream generation can supply) to gain on
 *   the dual.  Empirically on CEN PCP v0407 this inflates `loss_sol`
 *   reporting by ~6× system-wide on lines whose receivers see
 *   periodic negative-LMP excursions (must-dispatch wind, fixed-load
 *   exports), but the LP-objective impact is bounded by the gen
 *   merit-order at the sender so the SOLVE stays well-conditioned.
 *   Use `uniform` (strict equality) when the reported `loss_sol`
 *   values matter for downstream accounting and your network has
 *   recurring negative-LMP receivers.
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
  midpoint = 3,  ///< De-biased secant chords (midpoint-tangent shift)
};

inline constexpr auto line_pwl_layout_entries =
    std::to_array<EnumEntry<LinePwlLayout>>({
        {.name = "uniform", .value = LinePwlLayout::uniform},
        {.name = "equal_error", .value = LinePwlLayout::equal_error},
        {.name = "tangent", .value = LinePwlLayout::tangent},
        {.name = "midpoint", .value = LinePwlLayout::midpoint},
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
