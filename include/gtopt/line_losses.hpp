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
 * | Mode                | Extra rows/block | Extra cols/block          |
 * |---------------------|------------------|---------------------------|
 * | `none`              | 0                | 1 (bidirectional flow)    |
 * | `linear`            | 0                | 1–2 (flow per dir)        |
 * | `piecewise`         | 2                | K+3 (segs + loss + fp+fn) |
 * | `bidirectional`     | 4                | 2(K+2) (per-dir segs)     |
 * | `adaptive`          | resolved at config time (piecewise/bidirectional) |
 * | `dynamic`           | placeholder → piecewise                           |
 * | `piecewise_direct`  | 0                | 2K  (per-dir segs only)       |
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
#include <string_view>
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

// ─── Results ────────────────────────────────────────────────────────

/**
 * @brief LP indices produced by add_block() for one block.
 */
struct BlockResult
{
  std::optional<ColIndex> fp_col;  ///< A→B flow column (aggregator)
  std::optional<ColIndex> fn_col;  ///< B→A flow column (aggregator)
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
                                     double loss_envelope = 0.0);

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
