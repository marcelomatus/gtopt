/**
 * @file      line_losses.hpp
 * @brief     Modular transmission line losses engine
 * @date      2026-04-03
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides a pluggable loss-model dispatch for transmission lines.
 * Each LineLossesMode has its own implementation function that adds
 * the appropriate variables and constraints to the LP.
 *
 * ### Supported modes
 *
 * Counts are PER LOSSY LINE, PER BLOCK — a mode's LP footprint is
 * `count × n_lossy_lines × n_blocks`.  (`K` = number of segments / tangents.)
 *
 * (`K` = segments/tangents, `L` = `loss_secant_segments`, default 1.)
 *
 *   - `none`                 — 0 rows, 1 col  (signed flow)
 *   - `linear`               — 0 rows, 2 cols (flow per dir)
 *   - `piecewise` (≠tangent) — wraps `bidirectional`: 4 rows, 2K+4 cols
 *   - `piecewise` (tangent layout, legacy shared) — K rows, 3 cols
 *   - `bidirectional`        — 4 rows, 2K+4 cols (per-dir segs)
 *   - `piecewise_direct`     — 0 rows, 2K cols (per-dir segs only)
 *   - `tangent_signed_flow`  — K+3 rows (K tangents, one dropped when K
 *     is odd — prefer EVEN K — + 2 abs rows + 1 chord), 2+L cols
 *     (signed flow, loss, L abs-flow `v` cols)
 *   - `tangent_signed_flow` + SOS2 λ-form — K+3 rows + 1 SOS2 set,
 *     2L+3 cols
 *   - `adaptive` / `dynamic` — resolved at config time (piecewise /
 *     bidirectional)
 *
 * `tangent_signed_flow` (Coffrin outer-approximation; `LinePwlLayout::tangent`)
 * is the plexos2gtopt DEFAULT (eps=0.1, K=6) and is BOTH the most compact
 * (2+L cols) AND the most accurate: its K tangent inequalities form a LOWER
 * outer envelope of `(R/V²)·f²`, exact at every tangent point.  CAVEAT — it
 * never over-states loss ONLY while the bus-dual pair-sum stays
 * non-negative: under a negative pair-sum the LP inflates the abs-flow
 * proxy `v` past `|f|` and rides the loss up the chord (see
 * `test_line_losses_negative_lmp_kvl.cpp` and the ε-threshold notes in
 * `line.hpp::loss_cost_eps`); the exact fix is the SOS2 λ-form
 * (`loss_use_sos2` + `loss_secant_segments ≥ 2`).  Example footprint
 * (10-05, 278 lossy lines × 168 blocks): `tangent K=6` → 93,408 cols /
 * 326,928 rows, vs the regressed `piecewise uniform/midpoint K=2–5` mix
 * → 284,088 cols / 93,408 rows.
 *
 * ### Mathematical background
 *
 * Quadratic loss: `P_loss = R · f² / V²`  [MW], with R [Ω], f [MW], V [kV].
 *
 * Piecewise-linear approximation with K segments over `[0, f_max]`:
 *   - Segment width: `w = f_max / K`
 *   - Segment k (1-based) loss coefficient: `loss_k = w · R · (2k−1) / V²`
 *   - Total: `loss = Σ_k loss_k · seg_k`, with `Σ_k seg_k = |f|`
 *
 * PWL layout (`LinePwlLayout`, line_enums.hpp) controls ACCURACY at fixed K:
 *   - `uniform`  — equal-width SECANT chords.  A chord is an UPPER bound of the
 *     convex quadratic: exact at the segment endpoints, over-stating between
 *     them, worst near `f = 0`.  On a lightly-loaded line (`f ≪ f_max`) with
 *     few segments the first chord (constant slope `R·w/V²`) sits far above the
 *     true `R·f²/V²`, over-stating loss by ≈ `w/f`.  Aggregated this is why the
 *     uniform mix booked 93.3 GWh of loss against only ~40 GWh that the flows
 *     physically support (10-05).
 *   - `midpoint` — each chord shifted to the segment midpoint → de-biased.
 *   - `tangent`  — Coffrin tangents: a LOWER outer envelope, exact at the
 *     tangent points, never over-states.  Preferred (and the intended default).
 *
 * References:
 * - [1] Macedo, Vallejos, Fernández, "A Dynamic Piecewise Linear Model
 *       for DC Transmission Losses in Optimal Scheduling Problems",
 *       IEEE Trans. Power Syst., vol. 26, no. 1, pp. 508–516, 2011.
 * - [2] Wood & Wollenberg, "Power Generation, Operation and Control",
 *       3rd ed., Wiley, Ch. 13 (incremental transmission losses).
 * - [3] FERC Staff Paper, "Optimal Power Flow Paper 2: Linearization",
 *       December 2012.
 */

#pragma once

#include <optional>
#include <span>
#include <vector>

#include <gtopt/line.hpp>
#include <gtopt/linear_problem.hpp>

namespace gtopt
{

class PlanningOptionsLP;
class SystemContext;
class ScenarioLP;
class StageLP;
class BlockLP;

namespace line_losses
{

// ─── Configuration ──────────────────────────────────────────────────

/**
 * @brief Resolved loss parameters for LP construction.
 *
 * Built once per (line, stage) and used for every block in that stage.
 */
struct LossConfig
{
  LineLossesMode mode {};  ///< Fully resolved (no `adaptive`)
  LossAllocationMode allocation {};
  double lossfactor {};  ///< Effective linear loss [p.u.]
  double resistance {};  ///< R [Ω]
  double V2 {};  ///< V² [kV²]
  int nseg {1};  ///< Segment count for PWL modes
  /// Row-scale multiplier for the loss-link constraint
  /// (`s · loss − Σ s · loss_k · seg_k = 0`).  Default `1.0` (no
  /// scaling).  Lifts the smallest segment coefficient
  /// `seg_width · R / V²` (typically ~1e-6 for HV lines) toward O(1)
  /// so simplex pivoting doesn't see microscopic nonzeros.  Set
  /// globally via `model_options.scale_loss_link`; `PlanningLP`
  /// auto-computes from `median(R/V²)` when unset.
  double loss_row_scale {1.0};
  /// Segment-layout strategy for the PWL loss approximation.  See
  /// `LinePwlLayout` (line_enums.hpp): `uniform` (default; equal-
  /// width secant chords — preserves pre-2026-05 behaviour),
  /// `equal_error` (√-spaced minimax: same K and LP row count, ~√K
  /// better worst-case chord error), `tangent` (outer approximation,
  /// reserved for future).  Per-line override via
  /// `Line.loss_pwl_layout`; otherwise inherits from
  /// `ModelOptions.loss_pwl_layout` (TBD) — default `uniform`.
  LinePwlLayout pwl_layout {LinePwlLayout::uniform};
  /// Upper envelope [MW] over which the K PWL segments are spread.
  /// DECOUPLED from the flow cap: when the caller supplies a positive
  /// value (typically the ORIGINAL line rating for a soft-cap /
  /// `enforce_level`-lifted line whose `fmax` flow cap is inflated),
  /// the loss segments concentrate over THIS range rather than `fmax`.
  /// `0.0` (default) means "use the per-direction `fmax`/`block_tmax`"
  /// — i.e. the legacy flow-cap-anchored envelope, fully backward
  /// compatible.  See `add_piecewise`/`add_direction` for how flow past
  /// the envelope extrapolates on the last segment's slope.
  double loss_envelope {0.0};
  /// Per-MWh cost stamped on the per-direction loss columns
  /// (``loss_p`` / ``loss_n``) in ``piecewise``/``bidirectional`` modes.
  /// Strictly breaks LP-relax bidirectional-flow degeneracy: among all
  /// primal-feasible solutions sharing the same net dispatch the LP
  /// picks the one with single-direction flow.  ``0.0`` (default)
  /// preserves legacy behaviour.  Recommended ε ≈ 1e-6 $/MWh — well
  /// below LP optimality tolerance, so the objective is essentially
  /// unchanged.  Inert for ``none``, ``linear``, ``piecewise_direct``,
  /// and ``tangent`` layouts.
  double loss_cost_eps {0.0};
  /// Number of L-secant segment columns ``v_l`` emitted per (line,
  /// block) by ``add_tangent_signed_flow`` when the L-secant chord
  /// upper bound is active (issue #504).  ``1`` (default) preserves
  /// the pre-#504 single-secant chord ``ℓ ≤ (R·envelope/V²)·v``.
  /// ``L > 1`` replaces the single ``v`` column with ``L`` columns
  /// bounded ``v_l ∈ [0, envelope/L]`` and the chord upper bound
  /// becomes the piecewise ``ℓ ≤ Σ chord_slope_l · v_l`` with
  /// ``chord_slope_l = (R/V²)·(envelope/L)·(2l − 1)``.
  /// Inert outside ``tangent_signed_flow`` mode.
  int nseg_secant {1};
  /// Toggle SOS2 enforcement on the ``L`` secant-segment columns
  /// emitted when ``nseg_secant > 1`` (issue #504).  Without SOS2 the
  /// LP exploits the segment freedom to maximise the chord ceiling;
  /// with SOS2 at most two consecutive ``v_l`` are non-zero, forcing
  /// fill order ``v_1`` → … → ``v_L``.  Requires a MIP-capable LP
  /// backend with native SOS2 (CPLEX / Gurobi / HiGHS ≥ 1.6); see
  /// ``SolverBackend::add_sos2`` for the support matrix.  Inert
  /// outside ``tangent_signed_flow`` mode or when ``nseg_secant ≤ 1``.
  bool use_sos2 {false};
};

// ─── PWL geometry (exposed for unit testing) ────────────────────────

/// Geometry of one piecewise-linear segment built by ``add_segments``
/// for the static PWL approximation of the convex loss curve
/// ``ℓ(f) = (R/V²)·f²`` on ``[0, envelope]``.
///
///   * ``width``  — Δf covered by this segment (becomes the seg col's
///                  upper bound when caps are enforced; the LP picks
///                  ``Σ seg_k = |f|``).
///   * ``slope``  — chord slope of ``ℓ/(R/V²)`` on this segment, i.e.
///                  the geometric pre-factor.  The actual loss-row
///                  coefficient is ``slope × R / V²``.
struct SegGeom
{
  double width;
  double slope;
};

/// Compute the per-segment ``(width, slope)`` for the static PWL
/// approximation of the line-loss curve.  ``layout = uniform`` (the
/// default and currently the only mode with a meaningful per-segment
/// distribution — ``equal_error`` aliases to it for convex quadratic;
/// ``tangent`` uses a structurally different LP, not this function)
/// gives equal-width chords:
///
///   width_k  = envelope / K
///   slope_k  = (envelope / K) × (2k − 1)              ← chord (a + b)
///
/// so the loss-row coefficient on ``seg_k`` is
/// ``loss_k = slope_k × R / V²``.  Caller (``add_segments``) must
/// pass the FULL envelope (``effective_fmax`` = ``tmax`` when
/// ``enforce_level ≥ 1``, ``2·tmax`` when ``enforce_level = 0``);
/// passing ``seg_width = envelope/K`` by mistake makes every slope
/// shrink by ``1/K`` and the LP underestimate loss accordingly.
/// Exposed via this public header so unit tests can pin the formula.
[[nodiscard]] SegGeom loss_segment_geometry(
    double envelope,
    int nseg,
    int k,
    LinePwlLayout layout = LinePwlLayout::uniform) noexcept;

/// Geometry of one tangent line in the outer-approximation PWL
/// formulation (``layout = tangent``).  Tangent ``k`` touches the
/// convex quadratic ``ℓ(f) = (R/V²)·f²`` at ``touch_point = t_k``
/// and forms the LP inequality
///
///     loss ≥ slope_coef · |f| + intercept_coef       (pre-`R/V²`)
///
/// i.e. ``loss ≥ R/V² · (2·t_k · |f| − t_k²)``.
///
/// Fields are the GEOMETRIC pre-``R/V²`` values, so unit tests can
/// validate the curve math independently of any specific line's
/// resistance / voltage.  ``add_tangents`` multiplies both by
/// ``R/V²`` (and by ``loss_row_scale``) when stamping rows.
struct TangentGeom
{
  double touch_point;  ///< t_k where the tangent meets the curve
  double slope_coef;  ///< 2 · t_k (pre-R/V²); coef on |f| in `loss ≥ …`
  double intercept_coef;  ///< -t_k² (pre-R/V²); intercept in `loss ≥ …`
};

/// Compute the ``k``-th tangent's geometry for the outer-approximation
/// PWL loss model.  Tangent touch points are uniform partition
/// midpoints on ``[0, envelope]``:
///
///     t_k = envelope · (2k − 1) / (2K)
///
/// giving a max chord-error of ``(envelope/(2K))²`` at partition
/// boundaries — same magnitude as the secant overestimate (uniform
/// mode) but BELOW the curve (LP underestimates loss).  ``k`` is
/// 1-based; ``layout`` accepted only as ``tangent`` (other layouts
/// return ``{0, 0, 0}`` since this geometry is meaningless for them).
[[nodiscard]] TangentGeom loss_tangent_geometry(
    double envelope,
    int nseg,
    int k,
    LinePwlLayout layout = LinePwlLayout::tangent) noexcept;

// ─── Adaptive per-line K allocation (cube-root rule) ───────────────

/// Options for ``compute_adaptive_loss_segments``.
///
/// Defaults track the Python plexos2gtopt converter (see
/// ``scripts/plexos2gtopt/parsers.py::_apply_adaptive_loss_segments``):
/// 1 % worst-case error budget, floor of 2 segments (one segment is a
/// degenerate single secant), ceiling of 6 (hard LP-cost cap so the
/// rule never explodes K on extreme lines).
struct AdaptiveSegmentsOpts
{
  /// Worst-case PWL secant-error budget, as a fraction of the sum of
  /// per-line analytical peak losses ``Σ_i L_max,i`` with
  /// ``L_max,i = R_i · f_max,i²``.  When the raw KKT solution lands
  /// inside ``[floor, ceiling]`` for every line, the realised total
  /// error satisfies ``Σ_i L_max,i / (4 K_i²) ≤ err_pct · Σ_i L_max,i``.
  /// Set ≤ 0 to disable adaptive mode (every lossy line gets ``ceiling``).
  double err_pct {0.01};
  /// Minimum K per lossy line.  Floor=2 because a single secant
  /// reduces to a linear approximation, which collapses to ``linear``
  /// loss mode and is handled elsewhere.
  int floor {2};
  /// Maximum K per lossy line.  Acts as a hard LP-cost cap so the
  /// rule never spends >ceiling segments on an outlier line whose
  /// ``L_max`` would otherwise demand K≫6 under a tight budget.
  int ceiling {6};
};

/// Allocate per-line PWL segment counts using the KKT cube-root rule.
///
/// Given parallel ``resistances`` and ``peak_flows`` for ``N`` lines
/// and a worst-case error budget ``err_pct``, returns a length-``N``
/// vector of segment counts ``K_i`` minimising the total LP cost
/// ``Σ K_i`` subject to
///
///     Σ_i L_max,i / (4 K_i²)  ≤  err_pct · Σ_i L_max,i,
///         L_max,i  =  R_i · f_max,i²,        floor ≤ K_i ≤ ceiling.
///
/// The KKT-optimal allocation is ``K_i ∝ L_max,i^(1/3)``; concretely
///
///     S  = Σ_i L_max,i^(1/3)
///     B  = err_pct · Σ_i L_max,i
///     c  = √(S / (4·B))
///     K_i = clamp(⌈c · L_max,i^(1/3)⌉, floor, ceiling)
///
/// Lossless lines (``R_i ≤ 0`` OR ``f_max,i ≤ 0``) get ``K_i = 0`` so
/// the caller's PWL builder can omit them entirely.
///
/// Empirically on a CEN-PCP-sized system (281 lossy lines, L spans 5
/// orders of magnitude), this Pareto-dominates uniform-K=4 on every
/// axis at ``err_pct ≥ 0.02``: 39 % fewer LP variables, $1.73 M lower
/// LP cost (tighter PWL upper bound), and comparable CPLEX time —
/// because uniform K wastes segments on small lines whose true loss is
/// already negligible.
///
/// When ``opts.err_pct ≤ 0`` returns ``opts.ceiling`` for every lossy
/// line (uniform-K fallback).
///
/// @param resistances  R_i for each line (any unit, must match peak_flows)
/// @param peak_flows   f_max,i for each line (any unit, must match)
/// @param opts         budget + floor + ceiling
/// @return             K_i for each line (length == resistances.size())
/// @pre                ``resistances.size() == peak_flows.size()``
/// @pre                ``opts.floor ≥ 1`` and ``opts.ceiling ≥ opts.floor``
[[nodiscard]] std::vector<int> compute_adaptive_loss_segments(
    std::span<const double> resistances,
    std::span<const double> peak_flows,
    const AdaptiveSegmentsOpts& opts = {});

// ─── Dynamic per-line PWL layout selection ─────────────────────────

/// Result of ``compute_dynamic_loss_layout`` per line.
///
/// Encodes the two outputs of the dynamic rule:
///   * ``K``         segment count assigned by the cube-root rule
///                   (Phase 1 — identical to
///                   ``compute_adaptive_loss_segments``)
///   * ``layout``    PWL layout chosen by the mean-error allocator
///                   (Phase 2 — ``uniform`` for most lines, ``midpoint``
///                   for the heaviest mean-error contributors).
///
/// ``K = 0`` and ``layout = LinePwlLayout::uniform`` signals a lossless
/// line (R ≤ 0 or fmax ≤ 0); the caller's PWL builder should omit it.
struct DynamicAssignment
{
  int K {0};
  LinePwlLayout layout {LinePwlLayout::uniform};
};

/// Options for ``compute_dynamic_loss_layout``.  Reuses the same
/// ``err_pct`` budget that drives the adaptive K rule — the single
/// user-facing knob controls both per-line K (worst-case bound) AND
/// per-line layout (mean-error cancellation).
struct DynamicLayoutOpts
{
  double err_pct {0.01};  ///< same single budget as AdaptiveSegmentsOpts
  int floor {2};
  int ceiling {6};
};

/// Allocate per-line ``(K, layout)`` jointly under the same
/// ``err_pct`` budget that drives the adaptive K rule.
///
/// **Phase 1 — K allocation** (identical to
/// ``compute_adaptive_loss_segments``): cube-root rule
/// ``K_i ∝ L_max,i^(1/3)`` clamped to ``[floor, ceiling]``, bounding
/// the worst-case PWL secant error
/// ``Σ L_max,i / (4 K_i²) ≤ err_pct · Σ L_max,i``.
///
/// **Phase 2 — layout selection**: start every line at ``uniform``
/// (presolve eliminates the loss column → fastest LP).  Compute the
/// system-wide signed mean error
///
///     E_sys  =  Σ_uniform L_max,i / (6 K_i²)
///               − Σ_midpoint L_max,i / (12 K_i²)
///
/// (uniform overstates by +L/(6K²); midpoint understates by −L/(12K²)).
/// If ``E_sys ≤ err_pct · Σ L_max,i`` ⇒ keep all uniform; done.
/// Otherwise greedily flip the line with the largest mean-error
/// contribution (``L_i / K_i²``, highest first) to midpoint.  Each
/// flip reduces E_sys by ``L_i / (4 K_i²)`` (= the worst-case error
/// of that line).  Stop when either:
///   1. ``|E_sys| ≤ err_pct · Σ L_max,i`` (budget met), OR
///   2. the next flip would move ``E_sys`` further from zero than its
///      current value (greedy local optimum reached — typically when a
///      single heavy line's contribution exceeds the budget on its own).
///
/// The latter stop condition prevents overshoot into the negative
/// budget zone: without it, the greedy would happily keep flipping
/// past E_sys = 0 and land at -|E_sys| of the same magnitude.
///
/// When ``opts.err_pct ≤ 0`` returns ``{K = ceiling, layout = uniform}``
/// for every lossy line (uniform-K fallback; no layout decision needed).
///
/// @param resistances  R_i for each line (any unit, must match peak_flows)
/// @param peak_flows   f_max,i for each line (any unit, must match)
/// @param opts         budget + floor + ceiling
/// @return             per-line ``(K_i, layout_i)`` (length == size())
/// @pre                ``resistances.size() == peak_flows.size()``
/// @pre                ``opts.floor ≥ 1`` and ``opts.ceiling ≥ opts.floor``
[[nodiscard]] std::vector<DynamicAssignment> compute_dynamic_loss_layout(
    std::span<const double> resistances,
    std::span<const double> peak_flows,
    const DynamicLayoutOpts& opts = {});

// ─── Results ────────────────────────────────────────────────────────

/**
 * @brief LP indices produced by add_block() for one block.
 */
struct BlockResult
{
  std::optional<ColIndex> fp_col;  ///< A→B flow column (aggregator)
  std::optional<ColIndex> fn_col;  ///< B→A flow column (aggregator)
  /// Per-direction LINEAR loss factor stamped on `fp_col` / `fn_col`
  /// (`config.lossfactor`).  Non-zero ONLY under `LineLossesMode::
  /// linear`, where the line's loss is `fp_loss · primal(fp_col) +
  /// fn_loss · primal(fn_col)` — there is no explicit loss column or
  /// per-segment col in that mode.  Default `0` for every other mode
  /// (their loss is carried by `lossp_col` / `lossn_col` or the
  /// `seg_*_loss` vectors instead).
  double fp_loss {};
  double fn_loss {};
  std::optional<ColIndex> lossp_col;  ///< A→B loss column (PWL modes)
  std::optional<ColIndex> lossn_col;  ///< B→A loss column (PWL modes)
  std::optional<RowIndex> capp_row;  ///< A→B capacity constraint
  std::optional<RowIndex> capn_row;  ///< B→A capacity constraint
  /// Per-segment columns for the A→B direction.  Populated only by
  /// `piecewise_direct` mode, which has no aggregator (`fp_col` is
  /// empty).  Each segment carries its own bus-balance stamp with the
  /// per-segment loss factor; the Kirchhoff (KVL) row sums them with
  /// `+x_τ` per segment to recover `x_τ · f_p`.
  std::vector<ColIndex> seg_p_cols;
  /// Per-segment columns for the B→A direction.  Same semantics as
  /// `seg_p_cols`; KVL stamps each with `−x_τ`.
  std::vector<ColIndex> seg_n_cols;
  /// Per-segment physical loss factor `lf_k` for the A→B direction,
  /// parallel to `seg_p_cols` (same length / order).  Populated only by
  /// `piecewise_direct` mode.  `lf_k` is the exact coefficient stamped
  /// into the bus-balance rows for segment `k` (`seg_width · R ·
  /// (2k−1) / V²`), so the LP-consistent A→B loss is
  /// `Σ_k seg_p_loss[k] · primal(seg_p_cols[k])` in physical MW — there
  /// is NO extra column scale on this coefficient in direct mode.
  std::vector<double> seg_p_loss;
  /// Per-segment physical loss factor `lf_k` for the B→A direction,
  /// parallel to `seg_n_cols`.  Same semantics as `seg_p_loss`.
  std::vector<double> seg_n_loss;
  /// Single SIGNED flow column used by `tangent_signed_flow` mode
  /// (Coffrin outer approximation; no `fp`/`fn` decomposition).  When
  /// populated, `fp_col` / `fn_col` are empty and KVL stamps `+x_τ` on
  /// `flow_col` directly (sign comes from `f` itself).  Combined with
  /// `lossp_col` (a single quadratic-upper-bounded loss column).
  std::optional<ColIndex> flow_col;
  /// Auxiliary `|f|`-envelope column used by `tangent_signed_flow` to
  /// tighten the loss upper bound from the loose constant `R·fmax²/V²`
  /// to the linear-in-`|f|` chord `(R·fmax/V²) · v` (with `v ≥ |f|`).
  /// Internal to the loss model — KVL / bus balance do NOT stamp it.
  std::optional<ColIndex> f_abs_col;
};

// ─── Mode resolution ────────────────────────────────────────────────

/**
 * @brief Resolve the effective LineLossesMode for a line.
 *
 * Fallback chain:
 *   1. Per-line `line_losses_mode` (string → enum)
 *   2. Per-line `use_line_losses` (deprecated bool: false → none)
 *   3. Global `line_losses_mode()` from PlanningOptionsLP
 *
 * If the resolved mode is `adaptive`, it is mapped to:
 *   - `bidirectional` if the line has expansion modules (`has_expansion`)
 *   - `piecewise` otherwise (smallest-LP shared-segment model)
 *
 * If the resolved mode is `dynamic`, it falls back to `piecewise`
 * (with a log warning on first call).
 *
 * If the resolved mode is `piecewise_direct` **and** the line has
 * expansion (`has_expansion`), it falls back to `piecewise` with a
 * one-shot warning — the direct model bakes the per-segment bound
 * `tmax/K` into variable bounds and cannot be linked to a capacity
 * column.
 *
 * @param line            The line data (per-element overrides)
 * @param options         Global planning options
 * @param has_expansion   Whether the line has capacity expansion (expcap)
 */
[[nodiscard]] LineLossesMode resolve_mode(const Line& line,
                                          const PlanningOptionsLP& options,
                                          bool has_expansion);

/**
 * @brief Build a LossConfig for the given line and stage parameters.
 *
 * Combines mode resolution with physical parameter extraction.
 * For `linear` mode, auto-computes lossfactor from R/V if needed:
 *   `λ = R · f_max / V²`  (linearization at rated flow [2]).
 *
 * For PWL modes, validates that R > 0 and V > 0 and nseg > 1;
 * falls back to `linear` or `none` if insufficient.
 */
[[nodiscard]] LossConfig make_config(LineLossesMode mode,
                                     const Line& line,
                                     LossAllocationMode allocation,
                                     double lossfactor,
                                     double resistance,
                                     double voltage,
                                     int loss_segments,
                                     double fmax,
                                     double loss_row_scale = 1.0,
                                     double loss_envelope = 0.0,
                                     double loss_cost_eps = 0.0,
                                     int nseg_secant = 1,
                                     bool use_sos2 = false);

// ─── LP construction ────────────────────────────────────────────────

/**
 * @brief Add loss model variables and constraints for one block.
 *
 * Dispatches to the appropriate mode implementation:
 *   - `none`:             single bidirectional flow, no loss
 *   - `linear`:           directional flows with loss coefficients
 *   - `piecewise`:        shared segments for |f| = fp + fn [1]
 *   - `bidirectional`:    independent segments per direction [3]
 *   - `piecewise_direct`: PLP-faithful, per-segment bus stamps, no
 *                         loss var/row (requires no capacity column)
 *
 * @return LP indices for the created variables and constraints.
 */
[[nodiscard]] BlockResult add_block(const LossConfig& config,
                                    const ScenarioLP& scenario,
                                    const StageLP& stage,
                                    const BlockLP& block,
                                    LinearProblem& lp,
                                    SparseRow& brow_a,
                                    SparseRow& brow_b,
                                    double block_tmax_ab,
                                    double block_tmax_ba,
                                    double block_tcost,
                                    std::optional<ColIndex> capacity_col,
                                    Uid uid,
                                    bool enforce_capacity = true);

}  // namespace line_losses
}  // namespace gtopt
