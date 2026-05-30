// SPDX-License-Identifier: BSD-3-Clause

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <string>

#include <gtopt/constraint_names.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/line.hpp>
#include <gtopt/line_losses.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt::line_losses
{

// ─── Mode resolution ────────────────────────────────────────────────

namespace
{

/// Map `adaptive` → piecewise/piecewise_direct/bidirectional;
/// `dynamic` → piecewise; demote `piecewise_direct` → `piecewise` if
/// expansion is active.
///
/// `adaptive` picks the smallest-LP piecewise-linear option for the
/// active KVL formulation:
///   - has expansion           → `bidirectional` (2K+4 cols, 4 rows)
///   - no expansion + cycle_basis → `piecewise_direct` (2K cols, 0 rows)
///   - no expansion + node_angle  → `piecewise`        (K+3 cols, 2 rows)
///
/// Under cycle_basis the per-cycle KVL row already supports stamping
/// segments directly (see ``kirchhoff_cycle_basis.cpp:379-390``), so
/// the aggregator + link + loss rows of `piecewise` add no information
/// — picking `piecewise_direct` saves 2 rows per (line, block, scenario,
/// stage) at the cost of skipping the per-line `flowp`/`flown` solution
/// columns.  AMPL access to `line.flow` is preserved via the multi-col
/// segment-sum registration in ``line_lp.cpp``.
///
/// `piecewise_direct` is selectable explicitly in either KVL mode.
constexpr LineLossesMode resolve_adaptive_dynamic(LineLossesMode mode,
                                                  bool has_expansion,
                                                  KirchhoffMode kirchhoff_mode)
{
  switch (mode) {
    case LineLossesMode::adaptive:
      if (has_expansion) {
        return LineLossesMode::bidirectional;
      }
      return (kirchhoff_mode == KirchhoffMode::cycle_basis)
          ? LineLossesMode::piecewise_direct
          : LineLossesMode::piecewise;
    case LineLossesMode::dynamic:
      return LineLossesMode::piecewise;
    case LineLossesMode::piecewise_direct:
      return has_expansion ? LineLossesMode::piecewise
                           : LineLossesMode::piecewise_direct;
    default:
      return mode;
  }
}

}  // namespace

LineLossesMode resolve_mode(const Line& line,
                            const PlanningOptionsLP& options,
                            bool has_expansion)
{
  LineLossesMode mode {};

  if (auto m = line.line_losses_mode_enum()) {
    mode = *m;
  } else if (line.use_line_losses.has_value()) {
    if (!*line.use_line_losses) {
      mode = LineLossesMode::none;
    } else {
      // Per-line enables losses: use global mode, but if global is also
      // none fall back to the compiled default (adaptive).
      const auto global = options.line_losses_mode();
      mode = (global != LineLossesMode::none)
          ? global
          : PlanningOptionsLP::default_line_losses_mode;
    }
  } else {
    mode = options.line_losses_mode();
  }

  if (mode == LineLossesMode::dynamic) {
    static bool warned = false;
    if (!warned) {
      spdlog::warn(
          "line_losses_mode 'dynamic' is not yet implemented; "
          "falling back to 'piecewise'");
      warned = true;
    }
  }

  if (mode == LineLossesMode::piecewise_direct && has_expansion) {
    static bool warned = false;
    if (!warned) {
      spdlog::warn(
          "line_losses_mode 'piecewise_direct' requires no capacity "
          "column; falling back to 'piecewise' on expandable lines");
      warned = true;
    }
  }

  return resolve_adaptive_dynamic(
      mode, has_expansion, options.kirchhoff_mode());
}

// ─── Config builder ─────────────────────────────────────────────────

LossConfig make_config(LineLossesMode mode,
                       const Line& line,
                       LossAllocationMode allocation,
                       double lossfactor,
                       double resistance,
                       double voltage,
                       int loss_segments,
                       double fmax,
                       double loss_row_scale,
                       double loss_envelope)
{
  const double V2 = voltage * voltage;
  // Honour the caller's ``loss_segments`` verbatim (no ``max(1, …)``
  // clamp).  ``nseg = 0`` is a legitimate "no PWL segments" input
  // that the dispatcher (``add_block``) routes to the lossless
  // ``none`` formulation for PWL-required modes; ``nseg = 1`` still
  // falls back to ``linear`` below (one segment is degenerate as a
  // quadratic approximation).
  const int nseg = loss_segments;
  // Effective PWL envelope: explicit `loss_envelope` (decoupled from
  // the flow cap) when positive, else the flow cap `fmax`.  Used both
  // for the per-line `loss_row_scale` recipe below (so the scale
  // matches the actually-stamped coefficients) and stored on the
  // config for `add_piecewise`/`add_direction` to spread the segments.
  const double envelope = (loss_envelope > 0.0) ? loss_envelope : fmax;

  // Validate PWL prerequisites; fall back gracefully.
  if (mode == LineLossesMode::piecewise || mode == LineLossesMode::bidirectional
      || mode == LineLossesMode::piecewise_direct)
  {
    if (nseg <= 0) {
      // "No PWL segments" → no loss approximation at all.  The
      // dispatcher would also fall back to ``none``; setting it
      // here keeps the per-mode add_* implementations from ever
      // seeing ``nseg = 0``.
      mode = LineLossesMode::none;
    } else if (resistance <= 0.0 || V2 <= 0.0 || nseg < 2) {
      if (lossfactor > 0.0) {
        mode = LineLossesMode::linear;
      } else if (resistance > 0.0 && V2 > 0.0 && fmax > 0.0) {
        mode = LineLossesMode::linear;
        lossfactor = resistance * fmax / V2;
      } else {
        mode = LineLossesMode::none;
      }
    }
  }

  // For linear mode, auto-compute lossfactor from R/V if missing.
  // Linearization at rated flow: λ = R · f_max / V²  [2].
  if (mode == LineLossesMode::linear && lossfactor <= 0.0) {
    if (resistance > 0.0 && V2 > 0.0 && fmax > 0.0) {
      lossfactor = resistance * fmax / V2;
    } else {
      mode = LineLossesMode::none;
    }
  }

  // Per-line override.  Defaults to `uniform` when the field is unset
  // (current behaviour, no regression).
  //
  // Layout semantics:
  //   * ``uniform`` (default): equal-width secant chords.  Minimax for
  //     chord error on a convex quadratic (chord error scales as
  //     `width²/4`, minimized by equal widths).  ALWAYS overestimates
  //     loss; LP underdispatches lossy lines slightly.
  //   * ``equal_error``: documented alias for ``uniform`` — for a
  //     convex quadratic, equalising max chord error IS the uniform
  //     partition.  Kept as a named option so future schedulers can
  //     express the intent (e.g. weighted-by-flow-distribution
  //     adaptive partition) without a JSON-schema change once that
  //     variant lands.  See ``seg_geom`` for the rationale.
  //   * ``tangent``: K outer-approximation tangent inequalities on
  //     the existing flow + loss columns (no per-segment vars).
  //     UNDER-estimates loss; LP picks the binding tangent at its
  //     operating point and gets it exact there.  Structurally
  //     different LP — see ``add_tangents`` and the early-return in
  //     ``add_piecewise``.
  const auto requested = line.loss_pwl_layout_enum();

  // Per-line `loss_row_scale` override.
  //
  // A single global `loss_row_scale` (from
  // `ModelOptions.scale_loss_link` or auto-derived from the median
  // R/V² across all lines) is a compromise: it lifts the typical
  // line's seg coefficients to ~O(1) but lines whose R/V² is far
  // from the median end up with row coefs spread by many orders of
  // magnitude, driving κ up via the global min/max coefficient
  // ratio.
  //
  // For PWL modes, when all the per-line geometry is available
  // (R > 0, V² > 0, fmax > 0, nseg ≥ 2), pick a per-line scale so
  // the LARGEST per-segment loss coefficient lands at ~1.0 in the
  // LP matrix.  Concretely, the largest uniform-secant slope is
  //   max_k(loss_k) = (fmax/K)·(2K−1)·R/V²
  // so we use
  //   s_line = 1 / [ (fmax/K)·(2K−1)·R/V² ]
  //         = K·V² / [ fmax · R · (2K−1) ].
  //
  // The pre-scale `kLossCoeffTolerance` and post-scale
  // `kLossLpRowTolerance` checks in `add_segments` / `add_tangents`
  // continue to drop the tail-end seg/tangent stamps if the
  // per-line scale still leaves them below the LP-numerical-noise
  // floor.
  //
  // Falls back to the caller-supplied global `loss_row_scale` when
  // the per-line recipe isn't applicable (linear mode, no R/V, no
  // fmax, single-segment PWL).  This preserves prior behaviour for
  // the linear-loss path and for any line that can't supply enough
  // geometry to be self-scaled.
  double effective_scale = (loss_row_scale > 0.0) ? loss_row_scale : 1.0;
  const bool is_pwl = mode == LineLossesMode::piecewise
      || mode == LineLossesMode::bidirectional
      || mode == LineLossesMode::piecewise_direct;
  if (is_pwl && resistance > 0.0 && V2 > 0.0 && envelope > 0.0 && nseg >= 2) {
    // The PWL envelope is ``loss_envelope`` when decoupled (else the
    // flow cap ``fmax``) and is INDEPENDENT of EL: ``add_piecewise`` /
    // ``add_direction`` / ``add_piecewise_direct`` build the segments on
    // the same envelope whether or not the cap is enforced (EL=0
    // releases only the *bounds*, not the loss coefficients).  So the
    // per-line scale here matches the actual stamped coefs and a line
    // keeps the same loss approximation across EL transitions.
    const double max_slope = (envelope / static_cast<double>(nseg))
        * static_cast<double>((2 * nseg) - 1) * resistance / V2;
    if (max_slope > 0.0) {
      effective_scale = 1.0 / max_slope;
    }
  }

  return {
      .mode = mode,
      .allocation = allocation,
      .lossfactor = lossfactor,
      .resistance = resistance,
      .V2 = V2,
      .nseg = nseg,
      .loss_row_scale = effective_scale,
      .pwl_layout = requested,
      .loss_envelope = (loss_envelope > 0.0) ? loss_envelope : 0.0,
  };
}

// ─── Shared helpers (C++26 style) ───────────────────────────────────

namespace
{

/// Apply linear loss allocation to bus-balance rows for a single direction.
/// `sending` is the bus where power originates; `receiving` is where it
/// arrives.  The loss factor `lf` is allocated per [2].
void apply_linear_allocation(SparseRow& sending,
                             SparseRow& receiving,
                             ColIndex col,
                             double lf,
                             LossAllocationMode allocation)
{
  switch (allocation) {
    case LossAllocationMode::sender:
      sending[col] = -(1.0 + lf);
      receiving[col] = +1.0;
      break;
    case LossAllocationMode::split:
      sending[col] = -(1.0 + (lf / 2.0));
      receiving[col] = +(1.0 - (lf / 2.0));
      break;
    case LossAllocationMode::receiver:
    default:
      sending[col] = -1.0;
      receiving[col] = +(1.0 - lf);
      break;
  }
}

/// Apply loss-variable allocation to bus-balance rows.
/// Used by piecewise (on shared loss) and bidirectional (per-direction loss).
void apply_loss_allocation(SparseRow& sending,
                           SparseRow& receiving,
                           ColIndex loss_col,
                           LossAllocationMode allocation)
{
  switch (allocation) {
    case LossAllocationMode::sender:
      sending[loss_col] = -1.0;
      break;
    case LossAllocationMode::split:
      sending[loss_col] = -0.5;
      receiving[loss_col] = -0.5;
      break;
    case LossAllocationMode::receiver:
    default:
      receiving[loss_col] = -1.0;
      break;
  }
}

/// Add a capacity constraint: capacity_col − flow_col ≥ 0.
auto add_capacity_row(LinearProblem& lp,
                      const ScenarioLP& scenario,
                      const StageLP& stage,
                      const BlockLP& block,
                      std::string_view label,
                      Uid uid,
                      ColIndex capacity_col,
                      ColIndex flow_col) -> RowIndex
{
  auto row =
      SparseRow {
          .class_name = Line::class_name.full_name(),
          .constraint_name = label,
          .variable_uid = uid,
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      }
          .greater_equal(0);
  row[capacity_col] = 1;
  row[flow_col] = -1;
  return lp.add_row(std::move(row));
}

/// Tolerance below which a segment's loss coefficient is treated as
/// numerically negligible and the segment is skipped from the
/// lossrow.  Mirrors the
/// `PlanningOptionsLP::default_dc_line_reactance_threshold` pattern (1e-6 p.u.)
/// used by `validate_line_reactance` — when the LP coefficient is this small,
/// it adds no measurable physical effect and just pollutes the matrix
/// coefficient-range statistics.
///
/// The minimum representative segment-1 loss coefficient on
/// realistic transmission inputs is several orders of magnitude
/// above this threshold (a 500 kV line with R=0.1 Ω and tmax≈1000 MW
/// gives `loss_k_1 = (tmax/nseg)·R/V² ≈ 1.3e-6 · k_factor` — well
/// above 1e-6 for any non-degenerate input).  Lines that fall below
/// it are virtually-lossless transformers / busbar segments / data
/// errors where the loss model contributes nothing.
constexpr double kLossCoeffTolerance = 1e-6;

/// Post-scale (LP-coefficient) noise floor: drop the seg/tangent stamp
/// when its stamped coefficient — `loss_k * loss_row_scale` (segments)
/// or `2·k_loss·t_k * loss_row_scale` (tangents) — would land below
/// this threshold in the assembled LP matrix.
///
/// Rationale: a coefficient below this magnitude in the constraint
/// matrix sits in LP solver presolve noise and adds nothing the dual
/// can distinguish, but it does bloat the matrix coefficient range
/// (min/max ratio drives κ).  Started conservative at 1e-6 (same
/// order as the pre-scale physical-noise floor `kLossCoeffTolerance`
/// — only catches truly-degenerate seg/tangent stamps after scaling);
/// can be tightened upward (1e-5, 1e-4, …) if κ-driven measurements
/// show measurable headroom against the LP residual tolerance.
///
/// The pre-scale `kLossCoeffTolerance` check already catches the
/// truly-zero physical case; this is a separate LP-numerical-noise
/// floor that fires AFTER `loss_row_scale` is applied (so it scales
/// with the chosen row scaling and doesn't require retuning per
/// case).
constexpr double kLossLpRowTolerance = 1e-6;

/// Add piecewise-linear segment variables to linking and loss rows.
///
/// Segment k (1-based) has:
///   width      = seg_width
///   loss_coeff = seg_width · R · (2k−1) / V²   [1]
///
/// This approximates P_loss = R · f² / V² via a piecewise affine function.
///
/// `loss_row_scale` multiplies the segment coefficients in `lossrow`
/// only (the link row is untouched, so the bus-balance stamp on each
/// seg variable is unaffected).  Caller must apply the same scale to
/// `lossrow[loss_col]` so `s · loss − Σ s · loss_k · seg_k = 0` stays
/// consistent.
///
/// Per-segment dropout: when `|loss_k| < kLossCoeffTolerance` the
/// segment column is still created and stamped into the link row
/// (preserving the line's full piecewise capacity) but is NOT
/// stamped into the loss row.  Folding a `~1e-7`-scale coefficient
/// into the LP matrix would just pollute the row coefficient-range
/// statistics without contributing measurable physical loss.  See
/// the `kLossCoeffTolerance` block above for the rationale and the
/// matching `validate_line_reactance` clamp pattern in
/// `planning_lp.cpp`.
/// Compute (width_k, slope_k) for segment k under a given layout.
///
/// `envelope` is the upper bound of the PWL (typically `tmax` for EL≥1
/// or `2·tmax` for EL=0).  `K` is the segment count; `k` is 1-based.
///
/// All layouts approximate the convex `ℓ(f) = (R/V²)·f²` on `[0, B]`.
/// The returned width feeds the seg col's upper bound; the returned
/// slope (× R/V²) feeds the loss-row coefficient.
///
/// `uniform` (default; current behavior):
///   width_k = B/K           (equal widths)
///   slope_k = B·(2k−1)/K    (chord slope on segment [(k−1)B/K, kB/K])
///   chord error peaks at the outer segment: ≤ (B/K)²/4.
///
/// `equal_error` (sqrt-spaced minimax):
///   b_k     = √(k/K)·B
///   width_k = B·(√(k/K) − √((k−1)/K))
///   slope_k = b_k + b_{k−1} = B·(√(k/K) + √((k−1)/K))
///   max chord error is the same across all segments — falls as 1/K
///   instead of peaking on the outer segment.  Same K, same LP row
///   count, ~√K × better worst-case error.
///
/// `tangent` falls back to `uniform` here with a one-time warning at
/// the caller; the alternate LP structure (1 col + K tangent rows
/// instead of K cols + 1 row) is not yet wired.
// SegGeom now lives in line_losses.hpp so unit tests can reach it
// via the exposed ``loss_segment_geometry`` thin wrapper (defined
// below the anonymous namespace block).

/// De-bias offset for the ``midpoint`` layout.
///
/// The ``uniform`` secant chords lie ABOVE the convex quadratic
/// ``ℓ(f)=(R/V²)·f²`` (overstate loss by up to ``(w/2)²·R/V²`` at the
/// segment midpoints, zero at the breakpoints — a strictly positive,
/// systematic overstatement).  The ``midpoint`` layout keeps the SAME
/// chord slopes ``w·(2k−1)`` but shifts the whole PWL DOWN by the flat
/// constant ``(w/2)²·R/V²`` so each chord becomes the TANGENT to the
/// curve at its segment midpoint ``m_k=(2k−1)w/2``.
///
/// Crucially this offset is a SINGLE flat constant (it does NOT
/// accumulate per segment): adjacent midpoint tangents
/// ``ℓ(m_k)+ℓ'(m_k)(f−m_k)`` intersect EXACTLY at the breakpoints
/// ``b_k=kw``, so the max-of-tangents reconstruction is a continuous PWL
/// whose value at any flow ``f>0`` is ``secant(f) − (w/2)²·R/V²``.  The
/// loss row is therefore built as the inequality
///   ``s·loss − Σ s·loss_k·seg_k ≥ −s·(w/2)²·R/V²``
/// (vs the ``uniform`` equality with RHS 0).  Since ``loss`` is
/// minimised on the bus balance the inequality binds whenever the RHS is
/// positive and otherwise leaves ``loss`` at its ``0`` lower bound —
/// matching the curve clamped at 0 near ``f=0``.  Result: an UNBIASED
/// estimator (exact at midpoints, ≤ that constant UNDER at breakpoints)
/// instead of the all-positive secant overstatement.
[[nodiscard]] inline double midpoint_debias_offset(double envelope,
                                                   int nseg,
                                                   double resistance,
                                                   double V2) noexcept
{
  const double w = envelope / static_cast<double>(nseg);
  const double half_w = w / 2.0;
  return half_w * half_w * resistance / V2;
}

[[nodiscard]] inline SegGeom seg_geom(double envelope,
                                      int nseg,
                                      int k,
                                      LinePwlLayout layout) noexcept
{
  // For a convex quadratic ℓ(f) = (R/V²)·f², the chord-error on a
  // segment of width `w` is `w²/4` (max at midpoint).  Minimizing the
  // maximum error across K segments on a fixed envelope therefore
  // calls for **equal segment widths** = uniform partition — the
  // mathematically optimal static PWL.  Any other layout (including
  // the √-spaced "equal contribution" variant earlier explored under
  // the equal_error name) increases max chord error somewhere along
  // the curve, with measurable LP-feasibility consequences when low-
  // flow segments get steep slopes (verified: nseg=6 √-spaced
  // produced 213 GWh unserved on CEN PCP weekly).
  //
  // ``equal_error`` here is therefore a documented alias for
  // ``uniform`` — same geometry, same numerics — preserved as a
  // named option so future schedulers can express the intent without
  // a code change if a meaningful equal-error variant lands (e.g.
  // weighted by an empirical flow-distribution measure).
  //
  // ``tangent`` does NOT call seg_geom() at all — it builds a
  // structurally different LP (K outer-approximation rows on the
  // existing flow + loss columns; no per-segment variables).  See
  // add_piecewise_tangent().
  const double w = envelope / static_cast<double>(nseg);
  const double slope = w * static_cast<double>((2 * k) - 1);
  (void)layout;  // Reserved for future per-layout geometry overrides.
  (void)k;  // Used above; kept for symmetry once more modes are added.
  return {.width = w, .slope = slope};
}

/// Stamp the K outer-approximation tangent rows for the convex
/// quadratic loss ℓ(f) = k_loss · f² on `[0, envelope]`.  Adds K rows
/// of the form
///
///     s · loss − 2 · s · k_loss · t_k · f ≥ −s · k_loss · t_k²
///
/// (`s = loss_row_scale`), where `t_k = (2k−1)·envelope/(2K)` are the
/// midpoints of K uniform partitions.  Each tangent touches the curve
/// at `f = t_k` and lies BELOW the curve everywhere else (outer
/// approximation of a convex function).  The LP minimises `loss`, so
/// it drives `loss` down to `max_k(tangent_k(f))` at the chosen
/// operating point — exact at one tangent point, otherwise within
/// `(envelope/(2K))²·k_loss` of the true curve at the partition
/// boundaries.
///
/// LP-structure contrast with the secant-PWL path (`add_segments`):
///   * `add_segments`: K seg cols + 1 link row + 1 loss row.
///   * `add_tangents`: 0 seg cols + K tangent rows on existing flow
///     and loss cols.  Slightly fewer variables, similar row count
///     for moderate K, gives an **underestimate** of losses (LP picks
///     marginally too much flow on lossy lines) vs the secant
///     **overestimate**.  Reference: Aigner & Van Hentenryck,
///     *Line Loss Outer Approximation*, arXiv:2112.10975.
///
/// Reuses the existing `loss_col` already stamped by the caller into
/// the receiver-bus row (so loss is consumed on the bus balance as
/// in the secant path).  No `linkrow`/`lossrow` equalities — the K
/// tangent inequalities subsume both.
void add_tangents(LinearProblem& lp,
                  const ScenarioLP& scenario,
                  const StageLP& stage,
                  const BlockLP& block,
                  Uid uid,
                  double envelope,
                  double resistance,
                  double V2,
                  int nseg,
                  ColIndex loss_col,
                  std::optional<ColIndex> fp_col,
                  std::optional<ColIndex> fn_col,
                  double loss_row_scale)
{
  const double k_loss = resistance / V2;
  if (k_loss < kLossCoeffTolerance / std::max(envelope, 1.0)) {
    // Same dropout policy as add_segments: a tangent stamp at this
    // scale just pollutes coefficient statistics without contributing
    // measurable physical loss.  No rows added; loss col stays at its
    // [0, ∞) default and the bus balance simply sees loss = 0 (the
    // LP's natural optimum for an unconstrained minimised variable).
    return;
  }
  for (const auto k : iota_range(1, nseg + 1)) {
    const double t_k = envelope * static_cast<double>((2 * k) - 1)
        / (2.0 * static_cast<double>(nseg));
    // Per-tangent dropout, analogous to ``add_segments``'s per-segment
    // ``|loss_k| < kLossCoeffTolerance`` skip (see ``add_segments``
    // docstring + the ``kLossCoeffTolerance`` comment).  The slope of
    // tangent ``k`` on |f| is ``2 · k_loss · t_k`` (pre-scaling); when
    // it falls below the tolerance the row degenerates to
    // ``loss ≥ −k_loss · t_k² ≈ 0`` (always satisfied by ``loss ≥ 0``).
    // Stamping it would just pollute the coefficient-range statistics
    // and risk presolve numerical artifacts (suspected root cause of
    // the K=4 tangent MIP infeasibility on the CEN PCP bundle, where
    // a sub-set of lines had ``2 · k_loss · t_1`` near the tolerance).
    const double slope_coef = 2.0 * k_loss * t_k;
    const double scaled_slope = slope_coef * loss_row_scale;
    if (std::abs(slope_coef) < kLossCoeffTolerance
        || std::abs(scaled_slope) < kLossLpRowTolerance)
    {
      // Two-floor dropout matching `add_segments`: pre-scale
      // physical-noise floor + post-scale LP-coefficient noise floor.
      // See `kLossLpRowTolerance` comment for the rationale.
      continue;
    }
    auto trow =
        SparseRow {
            .class_name = Line::class_name.full_name(),
            .constraint_name = loss_link_constraint_name,
            .variable_uid = uid,
            .context =
                make_block_context(scenario.uid(), stage.uid(), block.uid(), k),
        }
            .greater_equal(-k_loss * t_k * t_k * loss_row_scale);
    trow.reserve(3);
    trow[loss_col] = +loss_row_scale;
    const double coef = -slope_coef * loss_row_scale;
    if (fp_col) {
      trow[*fp_col] = coef;
    }
    if (fn_col) {
      trow[*fn_col] = coef;
    }
    [[maybe_unused]] auto idx = lp.add_row(std::move(trow));
  }
}

void add_segments(LinearProblem& lp,
                  const ScenarioLP& scenario,
                  const StageLP& stage,
                  const BlockLP& block,
                  std::string_view seg_label,
                  Uid uid,
                  double envelope,
                  double resistance,
                  double V2,
                  int nseg,
                  SparseRow& linkrow,
                  SparseRow& lossrow,
                  double loss_row_scale,
                  double seg_uppb,
                  LinePwlLayout layout = LinePwlLayout::uniform)
{
  for (const auto k : iota_range(1, nseg + 1)) {
    const auto geom = seg_geom(envelope, nseg, k, layout);
    // loss_k matches the legacy uniform formula
    //   loss_k = seg_width · R · (2k−1) / V²
    // and generalises to `equal_error` via the chord slope formula
    // `loss_k = R/V² × (b_k + b_{k−1})`, where the previous code's
    // (2k−1) is the special case for uniform breakpoints `b_k = kB/K`.
    const double loss_k = geom.slope * resistance / V2;

    // Per-segment column upper bound.  Two regimes:
    //
    // (a) Bounded caller (``seg_uppb < DblMax`` — EL≥1 + no envelope
    //     decoupling): each segment is naturally capped at its own
    //     physical width ``geom.width``.  For ``uniform`` /
    //     ``midpoint`` all widths equal ``seg_width``, so ``min`` is a
    //     no-op; for ``equal_error`` widths differ, so cap at
    //     ``geom.width`` (tighter than ``seg_uppb`` for non-uniform).
    //
    // (b) Unbounded caller (``seg_uppb == DblMax`` — EL=0 OR
    //     ``decoupled_envelope = true`` for a lifted / soft-cap line):
    //     ONLY the last segment (``k == nseg``) keeps ``DblMax`` so it
    //     can absorb any flow past ``envelope`` at its steep slope.
    //     Segments 1..K−1 are STILL capped at ``geom.width`` to
    //     prevent the LP from stuffing low-slope segments (seg_1
    //     stuffing).  Without this cap the LP under-charges losses by
    //     3× on CEN PCP decoupled-envelope lines: it packs all flow
    //     into seg_1 (lowest slope) and pays near-zero loss instead of
    //     the convex-quadratic value.  Verified empirically:
    //     ``loss_sol`` per-cell ratio LP/analytic went from 0.087 on
    //     NvaPAzucar500→Polpaico500_I (f=1407, env=1000, K=8) up to
    //     the expected midpoint+debias value after capping.
    const bool overflow_segment = (k == nseg) && (seg_uppb >= DblMax);
    double col_uppb = DblMax;
    if (!overflow_segment) {
      // Default: cap at the segment's own width.  For ``uniform`` /
      // ``midpoint`` widths equal ``seg_width`` everywhere; for
      // ``equal_error`` widths differ across segments.
      col_uppb = std::min(seg_uppb, geom.width);
    }

    const auto seg_col = lp.add_col({
        .lowb = 0,
        // ``seg_uppb`` is normally ``seg_width`` (binding the
        // segment to its share of the total rating), but the caller
        // can pass ``DblMax`` when ``Line.enforce_level = 0`` to
        // let the LP discard the per-segment cap while keeping
        // ``loss_k`` (computed from the segment slope · R/V²)
        // numerically sensible.  Passing DblMax for both would
        // explode the loss-tracking row coefficients to ~1e+306
        // and break solver presolve.
        .uppb = col_uppb,
        .class_name = Line::class_name.full_name(),
        .variable_name = seg_label,
        .variable_uid = uid,
        .context =
            make_block_context(scenario.uid(), stage.uid(), block.uid(), k),
    });

    linkrow[seg_col] = -1.0;
    const double scaled_loss = loss_k * loss_row_scale;
    if (std::abs(loss_k) < kLossCoeffTolerance
        || std::abs(scaled_loss) < kLossLpRowTolerance)
    {
      // Skip the lossrow stamp — segment still participates in the
      // link row above (capacity preserved) but contributes zero
      // loss approximation.  Two independent floors:
      //   * pre-scale `kLossCoeffTolerance` (1e-6) — physical-noise
      //     floor: R/V² × seg_width below this is a virtually-
      //     lossless line (transformer / busbar / data outlier).
      //   * post-scale `kLossLpRowTolerance` (1e-3) — LP-coefficient
      //     noise floor: `loss_k × loss_row_scale` below this would
      //     add a sub-millis. matrix entry that the solver presolve
      //     drops anyway and that bloats κ via min/max coef spread.
      // No log here: the validation-time `validate_line_reactance`
      // warn covers the data-entry outlier case at the line level.
      continue;
    }
    lossrow[seg_col] = -scaled_loss;
  }
}

/// Labels for one direction of the bidirectional model.
struct DirLabels
{
  std::string_view flow;
  std::string_view seg;
  std::string_view loss;
  std::string_view link;
  std::string_view loss_link;
  std::string_view cap;
};

inline constexpr DirLabels positive_labels {
    .flow = LineLP::FlowpName,
    .seg = "flowp_seg",
    .loss = LineLP::LosspName,
    .link = "flowp_link",
    .loss_link = "lossp_link",
    .cap = LineLP::CapacitypName,
};

inline constexpr DirLabels negative_labels {
    .flow = LineLP::FlownName,
    .seg = "flown_seg",
    .loss = LineLP::LossnName,
    .link = "flown_link",
    .loss_link = "lossn_link",
    .cap = LineLP::CapacitynName,
};

// ─── Per-mode implementations ───────────────────────────────────────

/// No losses: single bidirectional flow variable.
/// Flow balance: P_send = P_recv (no dissipation).
BlockResult add_none(const ScenarioLP& scenario,
                     const StageLP& stage,
                     const BlockLP& block,
                     LinearProblem& lp,
                     SparseRow& brow_a,
                     SparseRow& brow_b,
                     double block_tmax_ab,
                     double block_tmax_ba,
                     double block_tcost,
                     Uid uid,
                     bool enforce_capacity)
{
  // ``enforce_capacity = false`` (PLEXOS ``Enforce Limits = 0``):
  // do NOT enforce any cap on the bidirectional flow — release the
  // bounds to ``±DblMax``.  ``add_none`` has no loss model, so the
  // ``2 × original`` segment-discretization rule does not apply here.
  const double lowb =
      enforce_capacity ? -block_tmax_ba : -LinearProblem::DblMax;
  const double uppb = enforce_capacity ? block_tmax_ab : LinearProblem::DblMax;
  const auto fpc = lp.add_col({
      .lowb = lowb,
      .uppb = uppb,
      .cost = block_tcost,
      .class_name = Line::class_name.full_name(),
      .variable_name = LineLP::FlowpName,
      .variable_uid = uid,
      .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
  });
  brow_a[fpc] = -1.0;
  brow_b[fpc] = +1.0;

  return {
      .fp_col = fpc,
      .fn_col = {},
      .lossp_col = {},
      .lossn_col = {},
      .capp_row = {},
      .capn_row = {},
      .seg_p_cols = {},
      .seg_n_cols = {},
  };
}

/// Linear loss factor: P_loss = λ · |f|.
/// Ref: Wood & Wollenberg [2], Ch. 13.
BlockResult add_linear(const LossConfig& config,
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
                       bool enforce_capacity)
{
  BlockResult result;
  // ``enforce_capacity = false`` (PLEXOS ``Enforce Limits = 0``):
  // do NOT enforce any cap on the directional flows — release the
  // upper bounds to ``DblMax``.  ``add_linear`` uses a per-line
  // ``λ`` (not segment widths), so the ``2 × original`` PWL
  // discretization rule does not apply here.
  const double flow_uppb_ab =
      enforce_capacity ? block_tmax_ab : LinearProblem::DblMax;
  const double flow_uppb_ba =
      enforce_capacity ? block_tmax_ba : LinearProblem::DblMax;

  // A→B direction: bus_a sends, bus_b receives.
  if (block_tmax_ab > 0.0) {
    const auto fpc = lp.add_col({
        .lowb = 0.0,
        .uppb = flow_uppb_ab,
        .cost = block_tcost,
        .class_name = Line::class_name.full_name(),
        .variable_name = LineLP::FlowpName,
        .variable_uid = uid,
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });
    result.fp_col = fpc;
    apply_linear_allocation(
        brow_a, brow_b, fpc, config.lossfactor, config.allocation);

    if (capacity_col) {
      result.capp_row = add_capacity_row(lp,
                                         scenario,
                                         stage,
                                         block,
                                         LineLP::CapacitypName,
                                         uid,
                                         *capacity_col,
                                         fpc);
    }
  }

  // B→A direction: bus_b sends, bus_a receives.
  if (block_tmax_ba > 0.0) {
    const auto fnc = lp.add_col({
        .lowb = 0.0,
        .uppb = flow_uppb_ba,
        .cost = block_tcost,
        .class_name = Line::class_name.full_name(),
        .variable_name = LineLP::FlownName,
        .variable_uid = uid,
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });
    result.fn_col = fnc;
    apply_linear_allocation(
        brow_b, brow_a, fnc, config.lossfactor, config.allocation);

    if (capacity_col) {
      result.capn_row = add_capacity_row(lp,
                                         scenario,
                                         stage,
                                         block,
                                         LineLP::CapacitynName,
                                         uid,
                                         *capacity_col,
                                         fnc);
    }
  }

  return result;
}

/// Piecewise-linear (single-direction): shared segments for |f| = fp + fn.
///
/// Approximates P_loss = R · f² / V²  [1] with K segments.
/// Variables per block: fp, fn, seg_1..seg_K, loss.
/// Constraints per block: 1 linking + 1 loss-tracking = 2 rows.
///
/// Linking:      fp + fn − Σ seg_k = 0
/// Loss track:   loss − Σ loss_k · seg_k = 0
///
/// Ref: Macedo et al. [1], single-direction formulation.
BlockResult add_piecewise(const LossConfig& config,
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
                          bool enforce_capacity)
{
  BlockResult result;
  const double fmax = std::max(block_tmax_ab, block_tmax_ba);
  if (fmax <= 0.0) {
    return result;
  }

  const int nseg = config.nseg;
  assert(nseg > 0 && "line_losses: nseg must be positive");
  // linkrow non-zeros: fp + fn + K segs ≤ K + 2.
  // lossrow non-zeros: loss + K segs    ≤ K + 1 (tighter, separate var).
  const auto link_reserve_sz = static_cast<size_t>(nseg) + 2;
  const auto loss_reserve_sz = static_cast<size_t>(nseg) + 1;
  // ``enforce_capacity = false`` (PLEXOS-mirror ``Enforce Limits = 0``):
  // two separate dials at work here.
  //
  //   1. ``seg_width`` — per-segment ``Δf`` used to compute the loss
  //      coefficient ``loss_k = seg_width · R · (2k−1) / V²``:
  //      WIDENED to ``2 × original / nseg`` so the PWL approximation
  //      covers the lifted dispatch range instead of being tuned for
  //      ``[0, original_tmax]``.  Without this widening the LP would
  //      see steeply ramping losses past the original cap because
  //      all flow above it would fall into the highest-loss segment.
  //
  //   2. ``seg_uppb`` (segment column upper bounds) and the
  //      directional ``fp / fn`` flow-column upper bounds:
  //      RELEASED to ``DblMax`` so the cap is NOT enforced.  The
  //      loss-PWL approximation past the original cap continues
  //      linearly with the segment-K slope (steepest), which is the
  //      "natural" extrapolation of a convex quadratic.
  //
  // The PWL envelope is ``fmax`` regardless of ``enforce_capacity``:
  // toggling a line from ``Enforce Limits = 1`` (EL=1) to
  // ``Enforce Limits = 0`` (EL=0) MUST keep the loss segment /
  // tangent geometry identical so the loss approximation doesn't
  // jump when the cap is relaxed.  Earlier versions widened the
  // envelope to ``2 × fmax`` for EL=0 — that broke EL-symmetry,
  // doubled the per-line ``loss_row_scale``, and (with uniform PWL)
  // halved every segment's loss slope vs the equivalent EL=1 line.
  //
  // ENVELOPE DECOUPLING: when the line supplies an explicit
  // ``config.loss_envelope`` (e.g. the ORIGINAL rating of a soft-cap /
  // ``enforce_level``-lifted line whose ``fmax`` flow cap is inflated),
  // spread the K loss segments over THAT range instead of ``fmax``.
  // The flow cap (``fp``/``fn`` upper bounds, capacity rows) stays on
  // ``fmax`` — only the loss approximation tightens.  Flow past the
  // envelope keeps accruing loss on the steepest (last) segment's slope
  // (the natural convex-quadratic extrapolation): the per-segment
  // ``seg_uppb`` is released to DblMax in that case so the topmost seg
  // absorbs the overflow.
  const bool decoupled_envelope = config.loss_envelope > 0.0;
  const double effective_fmax =
      decoupled_envelope ? config.loss_envelope : fmax;
  const double seg_width = effective_fmax / nseg;
  const double seg_uppb = (enforce_capacity && !decoupled_envelope)
      ? seg_width
      : LinearProblem::DblMax;

  // Hoisted once per (block, line): the AMPL/PAMPL block context is
  // identical for fp_col, fn_col, loss_col, linkrow and lossrow.
  // Pre-2026-05-14 each site rebuilt it from (scenario.uid(),
  // stage.uid(), block.uid()) — 5× per block, ≈ 5×K-of-segments calls
  // are similarly hoisted inside ``add_segments`` via the caller.
  const auto block_ctx =
      make_block_context(scenario.uid(), stage.uid(), block.uid());

  // Directional flow variables (for bus balance + Kirchhoff).
  if (block_tmax_ab > 0.0) {
    const auto fpc = lp.add_col({
        .lowb = 0,
        // No cap enforced when ``enforce_capacity = false``: release
        // to DblMax.  The ``2 ×`` widening lives in ``seg_width``
        // (loss-coefficient discretization), not in the flow bound.
        .uppb = enforce_capacity ? block_tmax_ab : LinearProblem::DblMax,
        .cost = block_tcost,
        .class_name = Line::class_name.full_name(),
        .variable_name = LineLP::FlowpName,
        .variable_uid = uid,
        .context = block_ctx,
    });
    result.fp_col = fpc;
    brow_a[fpc] = -1.0;
    brow_b[fpc] = +1.0;
  }

  if (block_tmax_ba > 0.0) {
    const auto fnc = lp.add_col({
        .lowb = 0,
        .uppb = enforce_capacity ? block_tmax_ba : LinearProblem::DblMax,
        .cost = block_tcost,
        .class_name = Line::class_name.full_name(),
        .variable_name = LineLP::FlownName,
        .variable_uid = uid,
        .context = block_ctx,
    });
    result.fn_col = fnc;
    brow_b[fnc] = -1.0;
    brow_a[fnc] = +1.0;
  }

  // Loss variable: tracks total power dissipated.
  const auto loss_col = lp.add_col({
      .lowb = 0,
      .uppb = LinearProblem::DblMax,
      .class_name = Line::class_name.full_name(),
      .variable_name = LineLP::LosspName,
      .variable_uid = uid,
      .context = block_ctx,
  });
  result.lossp_col = loss_col;

  apply_loss_allocation(brow_a, brow_b, loss_col, config.allocation);

  // Tangent (outer-approximation) PWL: K tangent inequalities on the
  // existing flow + loss columns, no per-segment variables.  Returns
  // early because the flow columns already carry the capacity bound
  // (``enforce_capacity ? block_tmax_{ab,ba} : DblMax``) — no need
  // for a separate sum-of-segments link row.  See ``add_tangents``
  // for the math.
  if (config.pwl_layout == LinePwlLayout::tangent) {
    add_tangents(lp,
                 scenario,
                 stage,
                 block,
                 uid,
                 effective_fmax,
                 config.resistance,
                 config.V2,
                 nseg,
                 loss_col,
                 result.fp_col,
                 result.fn_col,
                 config.loss_row_scale);
    return result;
  }

  // Linking: fp + fn − Σ seg_k = 0
  auto linkrow =
      SparseRow {
          .class_name = Line::class_name.full_name(),
          .constraint_name = flow_link_constraint_name,
          .variable_uid = uid,
          .context = block_ctx,
      }
          .equal(0);
  linkrow.reserve(link_reserve_sz);
  if (result.fp_col) {
    linkrow[*result.fp_col] = +1.0;
  }
  if (result.fn_col) {
    linkrow[*result.fn_col] = +1.0;
  }

  // Loss tracking: s · loss − Σ s · loss_k · seg_k {= 0 | ≥ −s·offset}
  // (s = loss_row_scale).  ``uniform`` keeps the equality (RHS 0,
  // strict secant upper bound); ``midpoint`` uses the de-biased
  // inequality with RHS ``−s·(w/2)²·R/V²`` so the PWL becomes the
  // midpoint-tangent estimator (see ``midpoint_debias_offset``).
  const bool debias = config.pwl_layout == LinePwlLayout::midpoint;
  const double debias_rhs = debias ? -config.loss_row_scale
          * midpoint_debias_offset(
              effective_fmax, nseg, config.resistance, config.V2)
                                   : 0.0;
  auto lossrow_proto = SparseRow {
      .class_name = Line::class_name.full_name(),
      .constraint_name = loss_link_constraint_name,
      .variable_uid = uid,
      .context = block_ctx,
  };
  auto lossrow = debias ? std::move(lossrow_proto).greater_equal(debias_rhs)
                        : std::move(lossrow_proto).equal(0);
  lossrow.reserve(loss_reserve_sz);
  lossrow[loss_col] = +config.loss_row_scale;

  // Pass ``seg_uppb`` separately from ``seg_width``: the loss
  // coefficients computed inside ``add_segments`` use ``seg_width``
  // (always the real value derived from the rating), while the
  // per-segment column upper bound is ``seg_uppb`` (``= seg_width``
  // when enforcing, ``= DblMax`` when ``Line.enforce_level = 0``).
  add_segments(
      lp,
      scenario,
      stage,
      block,
      "seg",
      uid,
      effective_fmax,  // full envelope (seg_geom divides by nseg internally)
      config.resistance,
      config.V2,
      nseg,
      linkrow,
      lossrow,
      config.loss_row_scale,
      seg_uppb,
      config.pwl_layout);

  [[maybe_unused]] auto linkrow_idx = lp.add_row(std::move(linkrow));
  [[maybe_unused]] auto lossrow_idx = lp.add_row(std::move(lossrow));

  // Capacity constraints (only for expansion lines).
  if (capacity_col) {
    if (result.fp_col) {
      result.capp_row = add_capacity_row(lp,
                                         scenario,
                                         stage,
                                         block,
                                         LineLP::CapacitypName,
                                         uid,
                                         *capacity_col,
                                         *result.fp_col);
    }
    if (result.fn_col) {
      result.capn_row = add_capacity_row(lp,
                                         scenario,
                                         stage,
                                         block,
                                         LineLP::CapacitynName,
                                         uid,
                                         *capacity_col,
                                         *result.fn_col);
    }
  }

  return result;
}

/// Add PWL loss model for one direction of the bidirectional model.
///
/// Creates K segment variables, 1 linking row, 1 loss-tracking row.
/// Ref: FERC [3], bidirectional decomposition.
struct DirResult
{
  std::optional<ColIndex> flow_col;
  std::optional<ColIndex> loss_col;
  std::optional<RowIndex> capacity_row;
};

DirResult add_direction(const LossConfig& config,
                        const ScenarioLP& scenario,
                        const StageLP& stage,
                        const BlockLP& block,
                        LinearProblem& lp,
                        SparseRow& sending_brow,
                        SparseRow& receiving_brow,
                        double block_tmax,
                        double block_tcost,
                        std::optional<ColIndex> capacity_col,
                        Uid uid,
                        const DirLabels& labels,
                        bool enforce_capacity)
{
  if (block_tmax <= 0.0) {
    return {};
  }

  const int nseg = config.nseg;
  assert(nseg > 0 && "line_losses: nseg must be positive");
  const auto reserve_sz = static_cast<size_t>(nseg) + 1;
  // EL-symmetric envelope: PWL uses ``block_tmax`` regardless of
  // ``enforce_capacity``.  EL=0 releases only the *bounds* (seg cols
  // and the directional flow col), keeping the loss coefficients
  // identical to the EL=1 case so flipping the EL value doesn't
  // alter the PWL approximation.  See ``add_piecewise`` for the
  // rationale.
  // ENVELOPE DECOUPLING (see ``add_piecewise``): spread the loss
  // segments over ``config.loss_envelope`` when set, else ``block_tmax``.
  // The flow cap stays on ``block_tmax``; only the loss approximation
  // tightens.  Overflow past the envelope accrues on the steepest
  // segment, so release ``seg_uppb`` to DblMax under decoupling.
  const bool decoupled_envelope = config.loss_envelope > 0.0;
  const double effective_envelope =
      decoupled_envelope ? config.loss_envelope : block_tmax;
  const double seg_width = effective_envelope / nseg;
  const double seg_uppb = (enforce_capacity && !decoupled_envelope)
      ? seg_width
      : LinearProblem::DblMax;

  const auto block_ctx =
      make_block_context(scenario.uid(), stage.uid(), block.uid());
  const auto flow_col = lp.add_col({
      .lowb = 0,
      .uppb = enforce_capacity ? block_tmax : LinearProblem::DblMax,
      .cost = block_tcost,
      .class_name = Line::class_name.full_name(),
      .variable_name = labels.flow,
      .variable_uid = uid,
      .context = block_ctx,
  });

  const auto loss_col = lp.add_col({
      .lowb = 0,
      .uppb = LinearProblem::DblMax,
      .class_name = Line::class_name.full_name(),
      .variable_name = labels.loss,
      .variable_uid = uid,
      .context = block_ctx,
  });

  // Bus balance: flow and loss allocation.
  sending_brow[flow_col] = -1.0;
  receiving_brow[flow_col] = +1.0;
  apply_loss_allocation(
      sending_brow, receiving_brow, loss_col, config.allocation);

  // Linking: f_total − Σ f_seg_k = 0
  // `block_ctx` already hoisted at top of function — reuse here
  // instead of rebuilding it twice more (linkrow + lossrow).
  auto linkrow =
      SparseRow {
          .class_name = Line::class_name.full_name(),
          .constraint_name = labels.link,
          .variable_uid = uid,
          .context = block_ctx,
      }
          .equal(0);
  linkrow.reserve(reserve_sz);
  linkrow[flow_col] = +1.0;

  // Loss tracking: s · loss − Σ s · loss_k · f_seg_k {= 0 | ≥ −s·offset}
  // (s = loss_row_scale).  ``midpoint`` uses the de-biased inequality;
  // see ``midpoint_debias_offset`` / ``add_piecewise``.
  const bool debias = config.pwl_layout == LinePwlLayout::midpoint;
  const double debias_rhs = debias ? -config.loss_row_scale
          * midpoint_debias_offset(
              effective_envelope, nseg, config.resistance, config.V2)
                                   : 0.0;
  auto lossrow_proto = SparseRow {
      .class_name = Line::class_name.full_name(),
      .constraint_name = labels.loss_link,
      .variable_uid = uid,
      .context = block_ctx,
  };
  auto lossrow = debias ? std::move(lossrow_proto).greater_equal(debias_rhs)
                        : std::move(lossrow_proto).equal(0);
  lossrow.reserve(reserve_sz);
  lossrow[loss_col] = +config.loss_row_scale;

  add_segments(lp,
               scenario,
               stage,
               block,
               labels.seg,
               uid,
               effective_envelope,  // PWL envelope; seg_geom divides by nseg
               config.resistance,
               config.V2,
               nseg,
               linkrow,
               lossrow,
               config.loss_row_scale,
               seg_uppb,
               config.pwl_layout);

  [[maybe_unused]] auto linkrow_idx = lp.add_row(std::move(linkrow));
  [[maybe_unused]] auto lossrow_idx = lp.add_row(std::move(lossrow));

  std::optional<RowIndex> cap_row;
  if (capacity_col) {
    cap_row = add_capacity_row(
        lp, scenario, stage, block, labels.cap, uid, *capacity_col, flow_col);
  }

  return {
      .flow_col = flow_col,
      .loss_col = loss_col,
      .capacity_row = cap_row,
  };
}

/// Bidirectional piecewise-linear: independent segments per direction.
/// Total: 4 rows + 2(K+2) columns per block.
/// Ref: FERC [3].
BlockResult add_bidirectional(const LossConfig& config,
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
                              bool enforce_capacity)
{
  auto [fp, lsp, capp] = add_direction(config,
                                       scenario,
                                       stage,
                                       block,
                                       lp,
                                       brow_a,
                                       brow_b,
                                       block_tmax_ab,
                                       block_tcost,
                                       capacity_col,
                                       uid,
                                       positive_labels,
                                       enforce_capacity);

  auto [fn, lsn, capn] = add_direction(config,
                                       scenario,
                                       stage,
                                       block,
                                       lp,
                                       brow_b,
                                       brow_a,
                                       block_tmax_ba,
                                       block_tcost,
                                       capacity_col,
                                       uid,
                                       negative_labels,
                                       enforce_capacity);

  return {
      .fp_col = fp,
      .fn_col = fn,
      .lossp_col = lsp,
      .lossn_col = lsn,
      .capp_row = capp,
      .capn_row = capn,
      .seg_p_cols = {},
      .seg_n_cols = {},
  };
}

/// PLP-direct per-direction helper.
///
/// For one direction (positive: a=sending, b=receiving; negative:
/// a=receiving, b=sending) builds K segment cols + 1 aggregation col +
/// 1 linking row.  Each segment is stamped directly into the bus rows
/// with its per-segment loss factor λ_k baked into the coefficients
/// (PLP `genpdlin.f:107-164`).
///
/// Returns the per-segment column list for one direction (PLP-faithful):
///   - K segment columns, each bounded `[0, w = tmax/K]`
///   - Per-segment bus-balance stamps with allocation-aware loss factor
///     `λ_k = w · (2k−1) · R / V²` (PLP `genpdlin.f:107-114`).
///
/// **No aggregator column, no link row.**  The Kirchhoff (KVL) row stamps
/// each segment with `±x_τ` so the algebraic identity `Σ seg_k = |f|` is
/// recovered without an explicit equality row.  This is what halves the
/// row count vs the older `piecewise` mode, and matches PLP exactly.
///
/// Per-segment transmission cost: split the line tcost across segments
/// (`tcost_k = block_tcost / K`) so `Σ tcost_k · seg_k = tcost · |f|`
/// after the segments saturate left-to-right.  At the optimum, segments
/// fill in order of increasing loss factor, so the cost stays consistent
/// with the legacy aggregator-based formulation.
[[nodiscard]] std::vector<ColIndex> add_direct_direction(
    const LossConfig& config,
    const ScenarioLP& scenario,
    const StageLP& stage,
    const BlockLP& block,
    LinearProblem& lp,
    SparseRow& sending_brow,
    SparseRow& receiving_brow,
    double block_tmax,
    double block_tcost,
    Uid uid,
    const DirLabels& labels,
    bool enforce_capacity)
{
  if (block_tmax <= 0.0) {
    return {};
  }

  const int nseg = config.nseg;
  assert(nseg > 0 && "line_losses: nseg must be positive");
  // EL-symmetric envelope: PWL uses ``block_tmax`` regardless of
  // ``enforce_capacity``.  EL=0 releases only the per-segment upper
  // bound, keeping the loss coefficients identical to the EL=1 case
  // so flipping the EL value doesn't alter the PWL approximation.
  // See ``add_piecewise`` for the rationale.
  const double seg_width = block_tmax / nseg;
  const double seg_tcost = block_tcost / nseg;
  const double seg_uppb = enforce_capacity ? seg_width : LinearProblem::DblMax;

  std::vector<ColIndex> seg_cols;
  seg_cols.reserve(static_cast<size_t>(nseg));

  for (const auto k : iota_range(1, nseg + 1)) {
    const double lf_k = seg_width * config.resistance
        * static_cast<double>((2 * k) - 1) / config.V2;

    // Per-segment column upper bound (same shape as ``add_segments``).
    // When ``seg_uppb == DblMax`` (EL=0 unbounded flow) only the LAST
    // segment keeps ``DblMax`` so it absorbs flow past the envelope at
    // its steepest slope; segs 1..K−1 stay capped at ``seg_width`` so
    // the LP can't pack everything into seg_1 (lowest slope) and
    // under-charge the convex-quadratic loss.  See ``add_segments``
    // for the rationale.
    const bool overflow_segment =
        (k == nseg) && (seg_uppb >= LinearProblem::DblMax);
    const double col_uppb = overflow_segment ? LinearProblem::DblMax
                                             : std::min(seg_uppb, seg_width);

    const auto seg_col = lp.add_col({
        .lowb = 0,
        .uppb = col_uppb,
        .cost = seg_tcost,
        .class_name = Line::class_name.full_name(),
        .variable_name = labels.seg,
        .variable_uid = uid,
        .context =
            make_block_context(scenario.uid(), stage.uid(), block.uid(), k),
    });

    apply_linear_allocation(
        sending_brow, receiving_brow, seg_col, lf_k, config.allocation);
    seg_cols.push_back(seg_col);
  }

  return seg_cols;
}

/// PLP-direct piecewise-linear: no loss variables, no loss-tracking
/// rows.  Per-segment bus stamps encode the quadratic loss curve
/// directly.  Requires no capacity column.
///
/// Variables per block: fp_agg, fn_agg, 2·K segment cols.
/// Constraints per block: 2 linking rows (one per direction).
///
/// Ref: PLP `genpdlin.f` (GenPDLinA).
BlockResult add_piecewise_direct(const LossConfig& config,
                                 const ScenarioLP& scenario,
                                 const StageLP& stage,
                                 const BlockLP& block,
                                 LinearProblem& lp,
                                 SparseRow& brow_a,
                                 SparseRow& brow_b,
                                 double block_tmax_ab,
                                 double block_tmax_ba,
                                 double block_tcost,
                                 Uid uid,
                                 bool enforce_capacity)
{
  auto seg_p_cols = add_direct_direction(config,
                                         scenario,
                                         stage,
                                         block,
                                         lp,
                                         brow_a,
                                         brow_b,
                                         block_tmax_ab,
                                         block_tcost,
                                         uid,
                                         positive_labels,
                                         enforce_capacity);

  auto seg_n_cols = add_direct_direction(config,
                                         scenario,
                                         stage,
                                         block,
                                         lp,
                                         brow_b,
                                         brow_a,
                                         block_tmax_ba,
                                         block_tcost,
                                         uid,
                                         negative_labels,
                                         enforce_capacity);

  return {
      .fp_col = {},
      .fn_col = {},
      .lossp_col = {},
      .lossn_col = {},
      .capp_row = {},
      .capn_row = {},
      .seg_p_cols = std::move(seg_p_cols),
      .seg_n_cols = std::move(seg_n_cols),
  };
}

}  // namespace

// ─── Public PWL geometry wrappers (unit-test entry points) ─────────

SegGeom loss_segment_geometry(double envelope,
                              int nseg,
                              int k,
                              LinePwlLayout layout) noexcept
{
  return seg_geom(envelope, nseg, k, layout);
}

TangentGeom loss_tangent_geometry(double envelope,
                                  int nseg,
                                  int k,
                                  LinePwlLayout layout) noexcept
{
  // Documented as meaningful only for ``layout = tangent``; return
  // an all-zero struct otherwise so callers (typically unit tests)
  // can assert on the precondition without throwing.
  if (layout != LinePwlLayout::tangent || nseg <= 0 || k < 1 || k > nseg) {
    return {.touch_point = 0.0, .slope_coef = 0.0, .intercept_coef = 0.0};
  }
  const double t_k = envelope * static_cast<double>((2 * k) - 1)
      / (2.0 * static_cast<double>(nseg));
  return {
      .touch_point = t_k,
      .slope_coef = 2.0 * t_k,
      .intercept_coef = -t_k * t_k,
  };
}

// ─── Adaptive per-line K allocation (cube-root rule) ───────────────

std::vector<int> compute_adaptive_loss_segments(
    std::span<const double> resistances,
    std::span<const double> peak_flows,
    const AdaptiveSegmentsOpts& opts)
{
  assert(resistances.size() == peak_flows.size());
  assert(opts.floor >= 1);
  assert(opts.ceiling >= opts.floor);

  const auto n = resistances.size();
  std::vector<int> K(n, 0);
  if (n == 0) {
    return K;
  }

  // Per-line peak loss L_max,i = R_i · f_max,i².  Lossless lines
  // (R ≤ 0 or fmax ≤ 0) keep K = 0 so the PWL builder can skip them.
  // Track total Σ L and Σ L^(1/3) on the lossy subset only.
  double L_total = 0.0;
  double S = 0.0;
  std::vector<double> L_cbrt(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    const double R = resistances[i];
    const double fmax = peak_flows[i];
    if (R > 0.0 && fmax > 0.0) {
      const double L = R * fmax * fmax;
      L_total += L;
      const double cb = std::cbrt(L);
      L_cbrt[i] = cb;
      S += cb;
    }
  }

  // No lossy lines → nothing to allocate.
  if (L_total <= 0.0) {
    return K;
  }

  // Uniform-K fallback: err_pct ≤ 0 means "don't try to be adaptive";
  // every lossy line gets the ceiling.  Matches the Python wrapper's
  // ``GTOPT_LOSS_ERROR_PCT=0`` contract.
  if (opts.err_pct <= 0.0) {
    for (std::size_t i = 0; i < n; ++i) {
      if (L_cbrt[i] > 0.0) {
        K[i] = opts.ceiling;
      }
    }
    return K;
  }

  // KKT cube-root rule:
  //   K_i ∝ L_i^(1/3) with constant c = √(S / (4·B)),  B = err_pct·Σ L.
  const double B = opts.err_pct * L_total;
  const double c = std::sqrt(S / (4.0 * B));
  for (std::size_t i = 0; i < n; ++i) {
    if (L_cbrt[i] > 0.0) {
      const double k_raw = c * L_cbrt[i];
      const auto k_int = static_cast<int>(std::ceil(k_raw));
      K[i] = std::clamp(k_int, opts.floor, opts.ceiling);
    }
  }
  return K;
}

// ─── Dynamic per-line PWL layout selection ─────────────────────────

std::vector<DynamicAssignment> compute_dynamic_loss_layout(
    std::span<const double> resistances,
    std::span<const double> peak_flows,
    const DynamicLayoutOpts& opts)
{
  assert(resistances.size() == peak_flows.size());
  assert(opts.floor >= 1);
  assert(opts.ceiling >= opts.floor);

  // Phase 1 — K via cube-root rule.  Reuse the existing function so
  // the two rules stay in lock-step: any improvement to the K
  // allocator flows through here automatically.
  const AdaptiveSegmentsOpts seg_opts {
      .err_pct = opts.err_pct,
      .floor = opts.floor,
      .ceiling = opts.ceiling,
  };
  const auto K =
      compute_adaptive_loss_segments(resistances, peak_flows, seg_opts);

  const auto n = resistances.size();
  std::vector<DynamicAssignment> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = {.K = K[i], .layout = LinePwlLayout::uniform};
  }

  // Uniform-K fallback: err_pct ≤ 0 means "don't try to be adaptive";
  // every lossy line gets the ceiling K with uniform layout.  Matches
  // ``compute_adaptive_loss_segments``'s behaviour and the Python
  // wrapper's ``GTOPT_LOSS_ERROR_PCT=0`` contract.
  if (opts.err_pct <= 0.0) {
    return out;
  }

  // Build the working set: index, L_max, K  for every lossy line.
  // Skip lossless (K == 0) — Phase 1 already marked them.
  struct Lossy
  {
    std::size_t idx;
    double L;
    int K;
  };
  std::vector<Lossy> lossy;
  lossy.reserve(n);
  double L_total = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    if (out[i].K > 0) {
      const double L = resistances[i] * peak_flows[i] * peak_flows[i];
      lossy.push_back({.idx = i, .L = L, .K = out[i].K});
      L_total += L;
    }
  }
  if (lossy.empty() || L_total <= 0.0) {
    return out;
  }

  // Phase 2 — system-wide signed mean error, all-uniform initial.
  //   E_sys = Σ_uniform L_i / (6 K_i²)  −  Σ_midpoint L_i / (12 K_i²)
  const double budget = opts.err_pct * L_total;
  double running = 0.0;
  for (const auto& ln : lossy) {
    running +=
        ln.L / (6.0 * static_cast<double>(ln.K) * static_cast<double>(ln.K));
  }
  if (running <= budget) {
    // Uniform mean meets the budget on its own — done.
    return out;
  }

  // Sort by mean-error contribution descending so the heaviest is
  // first.  ``L / K²`` is monotone in ``L_max,i / (6 K_i²)`` so it
  // gives the same ordering with one fewer division.
  std::sort(lossy.begin(),
            lossy.end(),
            [](const Lossy& a, const Lossy& b) noexcept
            {
              const double ka = static_cast<double>(a.K);
              const double kb = static_cast<double>(b.K);
              return (a.L / (ka * ka)) > (b.L / (kb * kb));
            });

  // Greedy flips while either (a) outside the ±budget zone, or
  // (b) the next flip strictly improves abs(running).  The latter
  // guards against overshooting into the negative-budget side, which
  // a naive ``while abs(running) > budget`` would do — the signed
  // running can run past zero and land at -X with abs > budget again.
  for (const auto& ln : lossy) {
    if (std::abs(running) <= budget) {
      break;
    }
    const double contribution =
        ln.L / (4.0 * static_cast<double>(ln.K) * static_cast<double>(ln.K));
    const double next_running = running - contribution;
    if (std::abs(next_running) >= std::abs(running)) {
      // Flip would not help — current state is the local minimum.
      break;
    }
    out[ln.idx].layout = LinePwlLayout::midpoint;
    running = next_running;
  }

  // ── Phase 1.5: try to reduce K on individual lines ───────────────
  // Phase 1 set K from the cube-root rule (worst-case bound).  Phase 2
  // chose layouts to satisfy the signed mean-error budget.  Phase 1.5
  // attempts to *reduce* K when both budgets have slack — typically
  // when ``err_pct`` is large enough that the floor=2 clamp has set K
  // well below the worst-case-binding value on the heavy lines.  See
  // the Python wrapper ``_apply_dynamic_loss_layout`` for the full
  // contract; the C++ implementation mirrors it exactly so any
  // converter (plp2gtopt, hand-written JSON) gets the same K assignment.
  //
  // No-op for ``err_pct`` regimes where the cube-root rule's KKT
  // optimum already saturates the worst-case budget (the typical
  // case on CEN-PCP-sized systems): every attempted reduction trips
  // either the worst-case or mean budget check and gets skipped.
  // The early-exit makes the no-op pass cheap.
  double current_worst = 0.0;
  for (const auto& ln : lossy) {
    current_worst += ln.L / (4.0 * static_cast<double>(ln.K * ln.K));
  }
  const double err_budget = opts.err_pct * L_total;

  // Mutable K map keyed by original line index.  Initialise from out[].
  // We sort by current K descending each pass so high-K lines are tried
  // first — they free the most LP segments per safe step.
  bool changed = true;
  while (changed) {
    changed = false;
    // Stable rebuild of the descending-K order each pass.
    std::vector<std::size_t> order(lossy.size());
    std::iota(order.begin(), order.end(), std::size_t {0});
    std::sort(order.begin(),
              order.end(),
              [&](std::size_t a, std::size_t b) noexcept
              { return out[lossy[a].idx].K > out[lossy[b].idx].K; });
    for (const auto pos : order) {
      const auto& ln = lossy[pos];
      auto& dst = out[ln.idx];
      const int k = dst.K;
      if (k <= opts.floor) {
        continue;
      }
      const int new_k = k - 1;
      const double new_kk =
          static_cast<double>(new_k) * static_cast<double>(new_k);
      const double old_kk = static_cast<double>(k) * static_cast<double>(k);
      const double new_worst =
          current_worst - ln.L / (4.0 * old_kk) + ln.L / (4.0 * new_kk);
      if (new_worst > err_budget) {
        continue;  // worst-case budget would burst
      }
      const double old_m = (dst.layout == LinePwlLayout::midpoint)
          ? (-ln.L / (12.0 * old_kk))
          : (+ln.L / (6.0 * old_kk));
      const double new_m = (dst.layout == LinePwlLayout::midpoint)
          ? (-ln.L / (12.0 * new_kk))
          : (+ln.L / (6.0 * new_kk));
      const double new_running = running - old_m + new_m;
      if (std::abs(new_running) > err_budget) {
        continue;  // mean budget would burst
      }
      // Commit
      dst.K = new_k;
      current_worst = new_worst;
      running = new_running;
      changed = true;
    }
  }

  return out;
}

// ─── Dispatcher ─────────────────────────────────────────────────────

BlockResult add_block(const LossConfig& config,
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
                      bool enforce_capacity)
{
  // ``nseg <= 0`` is a degenerate PWL configuration (no segments to
  // approximate the quadratic).  Rather than asserting deep in the
  // per-mode implementations, dispatch to the lossless ``none``
  // formulation so the LP stays well-formed: directional flow
  // variables get created, capacity is enforced as usual, but no
  // loss column or PWL row is added.  Matches the semantic that
  // "zero PWL segments → no loss approximation".  PWL-required
  // modes (``piecewise`` / ``bidirectional`` / ``piecewise_direct``)
  // share this fallback so the caller can pass ``nseg = 0`` without
  // tripping the per-mode ``assert(nseg > 0)``.
  if (config.nseg <= 0
      && (config.mode == LineLossesMode::piecewise
          || config.mode == LineLossesMode::bidirectional
          || config.mode == LineLossesMode::piecewise_direct))
  {
    return add_none(scenario,
                    stage,
                    block,
                    lp,
                    brow_a,
                    brow_b,
                    block_tmax_ab,
                    block_tmax_ba,
                    block_tcost,
                    uid,
                    enforce_capacity);
  }
  switch (config.mode) {
    case LineLossesMode::none:
      return add_none(scenario,
                      stage,
                      block,
                      lp,
                      brow_a,
                      brow_b,
                      block_tmax_ab,
                      block_tmax_ba,
                      block_tcost,
                      uid,
                      enforce_capacity);

    case LineLossesMode::linear:
      return add_linear(config,
                        scenario,
                        stage,
                        block,
                        lp,
                        brow_a,
                        brow_b,
                        block_tmax_ab,
                        block_tmax_ba,
                        block_tcost,
                        capacity_col,
                        uid,
                        enforce_capacity);

    case LineLossesMode::piecewise:
      return add_piecewise(config,
                           scenario,
                           stage,
                           block,
                           lp,
                           brow_a,
                           brow_b,
                           block_tmax_ab,
                           block_tmax_ba,
                           block_tcost,
                           capacity_col,
                           uid,
                           enforce_capacity);

    case LineLossesMode::bidirectional:
      return add_bidirectional(config,
                               scenario,
                               stage,
                               block,
                               lp,
                               brow_a,
                               brow_b,
                               block_tmax_ab,
                               block_tmax_ba,
                               block_tcost,
                               capacity_col,
                               uid,
                               enforce_capacity);

    case LineLossesMode::piecewise_direct:
      // `resolve_mode` demotes direct + expansion to `piecewise`,
      // so capacity_col must be empty here.  This is a **hard**
      // contract — if a future code path bypasses `resolve_mode`
      // and reaches `piecewise_direct` with `capacity_col` set,
      // the segments would be created without any capacity row to
      // constrain them, silently producing a model where flow can
      // exceed line capacity.  The Release build compiles asserts
      // out, so the prior `assert(!capacity_col, ...)` would have
      // missed it.  Promote to a runtime check that survives
      // -DNDEBUG.
      if (capacity_col) [[unlikely]] {
        spdlog::critical(
            "line_losses: piecewise_direct dispatcher reached with "
            "capacity_col (line uid={}); resolve_mode demotion was "
            "bypassed.  Aborting to avoid silently producing an "
            "unconstrained-capacity LP.",
            uid);
        flush_default_logger_best_effort();
        std::abort();
      }
      return add_piecewise_direct(config,
                                  scenario,
                                  stage,
                                  block,
                                  lp,
                                  brow_a,
                                  brow_b,
                                  block_tmax_ab,
                                  block_tmax_ba,
                                  block_tcost,
                                  uid,
                                  enforce_capacity);

    default:
      return add_none(scenario,
                      stage,
                      block,
                      lp,
                      brow_a,
                      brow_b,
                      block_tmax_ab,
                      block_tmax_ba,
                      block_tcost,
                      uid,
                      enforce_capacity);
  }
}

}  // namespace gtopt::line_losses
