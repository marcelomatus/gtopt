// SPDX-License-Identifier: BSD-3-Clause

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>

#include <gtopt/constraint_names.hpp>
#include <gtopt/cost_helper.hpp>
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

// ─── Refactor TODO (issue #504 follow-up) ───────────────────────────
//
// This translation unit currently aggregates EVERY loss-mode
// implementation (none / linear / piecewise / bidirectional /
// piecewise_direct / tangent_signed_flow) plus the public geometry
// helpers (loss_segment_geometry, loss_tangent_geometry,
// compute_adaptive_loss_segments, compute_dynamic_loss_layout) plus
// the dispatcher add_block.  At ~2700 LOC it's the largest TU in
// gtopt and compiles in ~3.5 s — meaningful build-time tail and
// awkward review surface when adding a new mode.
//
// Pending modular split (post-#504, separate non-functional PR):
//
//   include/gtopt/line_losses_detail.hpp
//       — declares the per-mode `detail::add_*` API + shared helpers
//         (apply_loss_allocation, add_capacity_row,
//         kLossCoeffTolerance, kLossLpRowTolerance) in
//         `gtopt::line_losses::detail` namespace.
//   source/line_losses.cpp
//       — keeps resolve_mode, make_config, the dispatcher add_block,
//         and the public geometry helpers.  ~600 LOC.
//   source/line_losses_helpers.cpp
//       — shared `detail::` helpers (apply_*, add_capacity_row, the
//         seg_geom / midpoint_debias_offset utilities, the tangent
//         and segment stamping helpers).  ~500 LOC.
//   source/line_losses_linear.cpp                 — add_none + add_linear
//   source/line_losses_piecewise.cpp              — add_piecewise (incl.
//                                                    add_piecewise_shared)
//   source/line_losses_bidirectional.cpp          — add_bidirectional +
//                                                    add_direction
//   source/line_losses_piecewise_direct.cpp       — add_piecewise_direct
//   source/line_losses_tangent_signed_flow.cpp    — add_tangent_signed_flow
//                                                    (issue #504 home —
//                                                    L-secant + SOS2)
//
// The split is non-functional (just moves code) so it warrants its
// own focused commit with no schema or LP-row changes — easier to
// review, easier to bisect.  See the conversation that landed
// PR #511.
// ────────────────────────────────────────────────────────────────────

// ─── Mode resolution ────────────────────────────────────────────────

namespace
{

/// Map `adaptive` → piecewise/bidirectional; `dynamic` → piecewise;
/// demote `piecewise_direct` → `piecewise` if expansion is active.
///
/// `adaptive` now always picks `piecewise` (which itself wraps
/// `bidirectional` for every non-`tangent` layout — see `add_piecewise`
/// in this file).  Both KVL formulations get the same arbitrage-free
/// LP shape: 2K+4 cols and 4 rows per (line, block, scenario, stage):
///   - has expansion              → `bidirectional` (2K+4 cols, 4 rows)
///   - no expansion + cycle_basis → `piecewise`    (2K+4 cols, 4 rows)
///   - no expansion + node_angle  → `piecewise`    (2K+4 cols, 4 rows)
///
/// Prior `adaptive` routed `cycle_basis` + no-expansion to
/// `piecewise_direct` (2K cols, 0 rows — the smallest-LP option).  That
/// choice has been retired because `piecewise_direct` has no link row
/// and no `fp`/`fn` aggregator, so the LP can dump quadratic loss into
/// fictitious bidirectional flow at any meshed line whose receiving bus
/// has a negative LMP.  Verified empirically on CEN PCP v0407 LP-relax:
/// `piecewise_direct` produced ~1500 GWh of fictitious bidirectional
/// flow vs ~130 GWh under `piecewise` (which is itself 99 % cleaner
/// after the `piecewise` → `bidirectional` wrapping).  The 4-row cost
/// of `piecewise` is worth the arbitrage immunity in any case where
/// curtailment-priced demand or must-dispatch surplus can drive
/// receivers negative — i.e. virtually every realistic GTEP case.
///
/// `piecewise_direct` remains selectable explicitly when the caller
/// can guarantee non-negative receiver LMPs (PLP's historical
/// operating regime: no congestion + no curtailment pricing).
constexpr LineLossesMode resolve_adaptive_dynamic(
    LineLossesMode mode, bool has_expansion, KirchhoffMode /*kirchhoff_mode*/)
{
  switch (mode) {
    case LineLossesMode::adaptive:
      return has_expansion ? LineLossesMode::bidirectional
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
                       double loss_envelope,
                       double loss_cost_eps,
                       int nseg_secant,
                       bool use_sos2)
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
      || mode == LineLossesMode::piecewise_direct
      || mode == LineLossesMode::tangent_signed_flow)
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

  // Sanitize the L-secant inputs: ``nseg_secant <= 0`` is treated as
  // ``1`` (single-secant chord, current production behaviour).
  // ``use_sos2 = true`` with ``nseg_secant <= 1`` would emit a vacuous
  // SOS2 declaration over a single column; we collapse to off here
  // rather than at the LP-build site so the per-line invariant is
  // visible in one place.
  const int nseg_secant_eff = std::max(1, nseg_secant);
  const bool use_sos2_eff = use_sos2 && nseg_secant_eff > 1;

  // Foot-gun warning (issue #504 review P2-3): the LP-arbitrage that
  // inflates ``Σ v_l`` past ``|f|`` — collapsing the L-secant chord to
  // a loose constant ceiling — is killed by EITHER ``loss_use_sos2 =
  // true``  (lambda-form MIP, full envelope, no ε needed) OR
  // ``loss_cost_eps > 0``  (ε on Σ v_l makes ``Σ v_l = |f|``  at the
  // LP optimum; the v distribution is then LP-indifferent because
  // the chord row is INACTIVE at optimum — the K-tangent lower bound
  // on ℓ binds first — but ``Σ v_l = |f|``  alone is enough to keep
  // the chord ≤ ``c · fmax²``  rather than ``c · fmax² · (2L−1)``
  // under the unbounded-Σ arbitrage).  The "ε-rely" generalisation
  // of the Coffrin L=1 single-secant recipe to L>1 segments.  Both
  // achieve a piecewise-linear chord ≥ true loss; lambda-form's is
  // exactly the secant at every distribution, ε-rely's is the secant
  // when bottom-up filled and looser otherwise (the LP can pick any
  // feasible distribution with no obj impact).  The warning fires
  // only when BOTH are off (the genuinely-broken config).  One-shot
  // so the misconfig surfaces during the first ``make_config`` call
  // without flooding the log on every (line, stage) pass.
  if (mode == LineLossesMode::tangent_signed_flow && nseg_secant_eff > 1
      && !use_sos2 && loss_cost_eps <= 0.0)
  {
    static bool warned_l_no_sos2 = false;
    if (!warned_l_no_sos2) {
      spdlog::warn(
          "line_losses: tangent_signed_flow with "
          "loss_secant_segments={} requires EITHER loss_use_sos2=true "
          "(lambda-form MIP, full envelope) OR loss_cost_eps > 0 "
          "(ε-rely: ε on Σv_l forces Σv_l=|f| at LP optimum, keeps "
          "the chord bounded; pure LP).  With both off the LP "
          "inflates Σ v_l and the chord collapses to a constant "
          "ceiling — STRICTLY WORSE than loss_secant_segments=1.  "
          "See issue #504.",
          nseg_secant_eff);
      warned_l_no_sos2 = true;
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
      .loss_cost_eps = (loss_cost_eps > 0.0) ? loss_cost_eps : 0.0,
      .nseg_secant = nseg_secant_eff,
      .use_sos2 = use_sos2_eff,
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
      .fp_loss = {},
      .fn_loss = {},
      .lossp_col = {},
      .lossn_col = {},
      .capp_row = {},
      .capn_row = {},
      .seg_p_cols = {},
      .seg_n_cols = {},
      .seg_p_loss = {},
      .seg_n_loss = {},
      .flow_col = {},
      .f_abs_col = {},
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
    // Capture the linear loss factor stamped on this column so
    // `LineLP::add_to_output` can reconstruct the per-cell loss
    // `fp_loss · primal(fp_col)` (linear mode has no loss/seg column).
    result.fp_loss = config.lossfactor;
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
    result.fn_loss = config.lossfactor;
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

/// Piecewise-linear (per-direction): delegates to the bidirectional
/// per-direction PWL.
///
/// Historical (pre-2026-05-31) `piecewise` used a SINGLE shared-segment
/// formulation:
///   Linking:    fp + fn − Σ seg_k = 0
///   Loss track: loss − Σ loss_k · seg_k = 0
/// with a single `loss` column consumed (allocation-aware) on one bus.
///
/// That formulation is structurally vulnerable to a phantom-flow
/// arbitrage: nothing in the LP forces `fp · fn = 0`, and because the
/// link row uses `fp + fn` (not `|fp − fn|`), the LP can inflate both
/// directions while only paying loss on ONE bus.  In meshed networks
/// with periods of negative LMP at the receiver, the LP profits by
/// inflating `fp + fn` and dumping the resulting quadratic loss "for
/// free" at the negative-LMP bus.  Verified empirically on CEN PCP
/// v0407: 99 % of dispatched blocks had both fp > 0 AND fn > 0,
/// producing 220 GWh of fictitious "waste" energy system-wide and a
/// 6× overstatement of `loss_sol` vs the physical reference.
///
/// The bidirectional formulation (per-direction segments + per-
/// direction loss column consumed at each direction's own receiver)
/// breaks this arbitrage: setting fp = fn = X pays loss on BOTH
/// receivers (cancelling any negative-LMP gain unless BOTH buses are
/// negative, which is rare), instead of being a single-bus free dump.
/// Verified empirically on the same case: bidirectional held the dual-
/// direction rate to 1.5 % of blocks.
///
/// We therefore implement `piecewise` as a thin wrapper around
/// `bidirectional`.  This roughly doubles the per-line PWL row /
/// column count vs the historical `piecewise` (2 × (K cols + 2 rows
/// + 1 loss col) instead of K cols + 2 rows + 1 loss col), but the
/// LP is physically correct.  The historical single-direction
/// behaviour has no use case — it was always either fine (network
/// with no negative-LMP buses, where the LP self-organised to one
/// direction) or broken (the documented case).
///
/// Recommended modern default for new cases is
/// `LineLossesMode::tangent_signed_flow` (Coffrin outer-approx on a
/// single signed flow column + |f|-aux chord upper bound); see
/// `add_tangent_signed_flow` below.  Use `bidirectional` when an
/// explicit per-direction loss accounting (separate `loss_p` /
/// `loss_n` columns at each receiver) is required.
///
/// Ref: Macedo et al. [1], single-direction formulation (historical);
///      FERC [3], bidirectional decomposition (fallback);
///      Coffrin & Van Hentenryck [4], tangent_signed_flow (default).
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
                          bool enforce_capacity);

/// Coffrin-style outer-approximation loss model on a SINGLE signed
/// flow column `f ∈ [−tmax_ba, +tmax_ab]`.  Emits, per (line, block):
///   - 1 loss col `ℓ ≥ 0`
///   - 1 abs-value aux col `v ∈ [0, fmax_phys]`     (task #102)
///   - K tangent rows `ℓ ≥ R/V²·(2 f_k f − f_k²)`   (k = 1..K)
///   - 2 abs-value rows `v ≥ +f` and `v ≥ −f`       (task #102)
///   - 1 chord upper-bound row `ℓ ≤ (R·fmax_phys/V²)·v`  (task #102)
/// Phantom-flow proof: structurally impossible — only one signed `f`
/// variable, so `fp · fn > 0` cannot arise.  Forward-declared here so
/// the `add_piecewise` dispatcher (below) can route to it on
/// `LineLossesMode::tangent_signed_flow` even though the body lives
/// after `add_bidirectional`.  See the definition for the math.
BlockResult add_tangent_signed_flow(const LossConfig& config,
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
                                    bool enforce_capacity);

/// Legacy single-direction PWL implementation, kept behind the
/// `tangent` layout path which still relies on the shared-loss column
/// + per-direction `fp/fn` aggregator (tangent rows reference both
/// `fp_col` and `fn_col` symmetrically; it has no per-direction
/// counterpart in the current code).  All other layouts now route
/// through `add_bidirectional` for the phantom-flow fix.
BlockResult add_piecewise_shared(const LossConfig& config,
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
  //
  // ``loss_cost_eps`` (default 0.0) stamps a tiny per-MWh cost on the
  // loss column so the LP strictly prefers single-direction dispatch
  // among otherwise-degenerate solutions.  ``add_piecewise_shared`` has
  // ONE shared loss column for both directions (``tangent`` layout only
  // post-2026-05-31), so the cost still applies — the LP minimises
  // ``loss`` which equals ``ε·(loss)`` here vs ``ε·(loss_p + loss_n)``
  // in the bidirectional path; either way it picks the smallest loss.
  const double loss_block_cost = config.loss_cost_eps > 0.0
      ? CostHelper::block_ecost(scenario, stage, block, config.loss_cost_eps)
      : 0.0;
  const auto loss_col = lp.add_col({
      .lowb = 0,
      .uppb = LinearProblem::DblMax,
      .cost = loss_block_cost,
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
  auto lossrow =
      debias ? lossrow_proto.greater_equal(debias_rhs) : lossrow_proto.equal(0);
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

  // ``loss_cost_eps`` (default 0.0) stamps a tiny per-MWh cost on each
  // per-direction loss column so the LP strictly prefers single-
  // direction dispatch.  The per-direction loss curve is convex; with
  // ε > 0 on both ``loss_p`` and ``loss_n`` the LP picks the unique
  // ``fn = 0`` (or ``fp = 0``) solution for any required net flow
  // ``f = fp − fn ≥ 0``, eliminating the LP-degeneracy phantom
  // bidirectional flow without SOS1, MIP, or binaries.
  const double loss_block_cost = config.loss_cost_eps > 0.0
      ? CostHelper::block_ecost(scenario, stage, block, config.loss_cost_eps)
      : 0.0;
  const auto loss_col = lp.add_col({
      .lowb = 0,
      .uppb = LinearProblem::DblMax,
      .cost = loss_block_cost,
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
  auto lossrow =
      debias ? lossrow_proto.greater_equal(debias_rhs) : lossrow_proto.equal(0);
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
      .fp_loss = {},
      .fn_loss = {},
      .lossp_col = lsp,
      .lossn_col = lsn,
      .capp_row = capp,
      .capn_row = capn,
      .seg_p_cols = {},
      .seg_n_cols = {},
      .seg_p_loss = {},
      .seg_n_loss = {},
      .flow_col = {},
      .f_abs_col = {},
  };
}

/// Coffrin-style outer approximation on a SINGLE signed flow column.
///
/// Per (line, block) this creates:
///   * 1 signed flow column `f ∈ [−tmax_ba, +tmax_ab]`
///     (`enforce_capacity == false` releases bounds to `±DblMax`).
///   * 1 loss column `ℓ ∈ [0, (R/V²)·fmax²]` — the quadratic upper
///     envelope on `[−fmax, +fmax]`, exact at `f = ±fmax` and a
///     strict over-estimate for `|f| < fmax`.  Without this upper
///     bound the LP can inflate `ℓ` arbitrarily whenever the
///     receiver-bus LMP propagates negative (same arbitrage we
///     observed on the `midpoint` layout in negative-LMP networks).
///   * K tangent inequalities `ℓ ≥ (2·R/V²)·f_k·f − (R/V²)·f_k²` at
///     tangent points `f_k = fmax · (2k − K − 1) / K`, k = 1..K
///     (uniform spacing centred on `[−fmax, +fmax]`, NOT touching
///     the endpoints).  Each tangent is the gradient of the convex
///     quadratic at `f_k`; the K-fold maximum is a piecewise-affine
///     LOWER envelope, exact at every tangent point and within
///     `(fmax/K)² · R/V²` at the partition boundaries.
///
/// Bus balance: `sender = −f`, `receiver = +f` (with loss allocated
/// per the standard `apply_loss_allocation` rule on `loss_col`).  KVL
/// uses the signed `f` column directly with a single `+x_τ`
/// coefficient — no `fp/fn` decomposition needed.
///
/// Phantom flow IMPOSSIBLE by construction: there is no way to
/// represent simultaneous bidirectional flow on a line because the
/// LP carries one (signed) variable per (line, block).
///
/// LP size: 2 cols + (K + 1) rows + 1 col upper-bound (the quadratic
/// envelope).  At K = 5 → 2 + 6 = 8 non-zero matrix entries per
/// (line, block); compare `bidirectional` at K = 5 (~18 non-zeros).
///
/// Refs: Coffrin & Van Hentenryck, *A Linear-Programming
///       Approximation of AC Power Flows*, INFORMS J. Computing
///       (2014); Aigner & Van Hentenryck, arXiv:2112.10975 (2022).
BlockResult add_tangent_signed_flow(const LossConfig& config,
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

  // Effective flow envelope: ``loss_envelope`` (when explicitly set;
  // typically the ORIGINAL rating of a soft-cap / lifted line) else the
  // max of the per-direction physical caps.  Used both for the quadratic
  // upper bound on ``ℓ`` and for the tangent point placement.
  const double fmax_phys = std::max(block_tmax_ab, block_tmax_ba);
  const bool decoupled_envelope = config.loss_envelope > 0.0;
  const double effective_fmax =
      decoupled_envelope ? config.loss_envelope : fmax_phys;

  if (effective_fmax <= 0.0) {
    return result;
  }

  const int nseg = config.nseg;
  assert(nseg > 0 && "line_losses: nseg must be positive");

  const double k_loss = config.resistance / config.V2;
  // Skip the entire model if R/V² is effectively zero — same physical-
  // noise floor used by ``add_segments`` / ``add_tangents``.  Falls
  // through to ``add_none`` semantics: a single bidirectional flow
  // column without any loss approximation.  Returning empty here lets
  // the caller decide whether to fall back; for symmetry with
  // ``add_segments`` we still create the signed flow column below if
  // possible so the line participates in KCL / KVL.
  // Note: we deliberately mirror the dropout policy of ``add_tangents``:
  // the col is still created so KVL has something to stamp.

  // ── Signed flow column ──────────────────────────────────────────────
  // Sign convention: positive = A→B, negative = B→A (matches the
  // ``fp``/``fn`` aggregator semantics in the other modes).
  const double flow_lowb =
      enforce_capacity ? -block_tmax_ba : -LinearProblem::DblMax;
  const double flow_uppb =
      enforce_capacity ? block_tmax_ab : LinearProblem::DblMax;

  const auto block_ctx =
      make_block_context(scenario.uid(), stage.uid(), block.uid());

  const auto flow_col = lp.add_col({
      .lowb = flow_lowb,
      .uppb = flow_uppb,
      .cost = block_tcost,
      // Pin the Ruiz column scale: the loss apparatus (the |f| abs rows +
      // chord/tangent rows carrying ``R/V²`` / ``1/x`` coefficients) otherwise
      // drives this signed-flow column's Ruiz scale down to ≈ reactance, so
      // its SCALED bound balloons to ~tmax/reactance (e.g. 3480 → 89,450).
      // That wide column bound is what cuOpt's Dual-Simplex basis chokes on.
      // Pinning keeps the bound at its physical ±tmax; the row equilibration
      // is unaffected, so the optimum and duals are unchanged.
      .pin_scale = true,
      .class_name = Line::class_name.full_name(),
      .variable_name = LineLP::FlowsName,
      .variable_uid = uid,
      .context = block_ctx,
  });
  result.flow_col = flow_col;

  // Bus balance: sender contributes -f, receiver contributes +f.  The
  // loss column (created below) absorbs the dissipated power via
  // ``apply_loss_allocation``.  ``f`` carries its own sign, so the
  // bus-balance stamp does NOT change sign per direction — exactly
  // what the loss allocation expects from a unified flow column.
  brow_a[flow_col] = -1.0;
  brow_b[flow_col] = +1.0;

  // ── Loss column ─────────────────────────────────────────────────────
  // Upper bound: a LINEAR-IN-|f| chord rather than the loose constant
  // ``(R/V²) · fmax²``.  Concretely we introduce an auxiliary
  // ``v ≥ |f|`` column (``f_abs_col`` below, bounded ``[0, fmax]``) tied
  // to the signed flow column by two rows
  //
  //     v − f ≥ 0      (so v ≥ +f)
  //     v + f ≥ 0      (so v ≥ −f)
  //
  // and replace the previous constant column bound with the row
  //
  //     ℓ ≤ (R · fmax / V²) · v
  //
  // i.e. the secant chord of the convex quadratic ``R·v²/V²`` between
  // ``(0, 0)`` and ``(fmax, R·fmax²/V²)``.  Because the curve is convex
  // the secant lies ABOVE it on ``[0, fmax]``, so the row is a valid
  // upper bound on ``ℓ`` for any ``|f| ∈ [0, fmax]``.  It is tight at
  // both endpoints (``v = 0`` ⇒ ``ℓ ≤ 0``; ``v = fmax`` ⇒
  // ``ℓ ≤ R·fmax²/V²``, matching the legacy constant bound) and
  // linearly interpolates in between — so for intermediate ``|f|`` it is
  // 2–4× tighter than the constant bound.  This closes the 0.3 %
  // CEN-PCP objective gap vs ``bidirectional`` where the LP was
  // previously free to inflate ``ℓ`` up to ``R·fmax²/V²`` regardless of
  // how small ``|f|`` was.
  //
  // Arbitrage immunity still holds: with the chord row, ``ℓ`` cannot
  // exceed ``(R·fmax/V²) · |f|`` at the optimum, so a negative-LMP
  // receiver cannot draw "free" power by inflating ``ℓ`` past the
  // physical loss.  The K tangent LOWER bounds remain unchanged.
  //
  // Asymmetric ratings (``tmax_ab ≠ tmax_ba``): we anchor the chord at
  // ``fmax = max(tmax_ab, tmax_ba)`` (via ``fmax_phys`` above), so the
  // chord remains a valid upper bound across the FULL signed flow range.
  // The chord is slightly loose at ``|f| > min(tmax_ab, tmax_ba)``
  // (lives in the unreachable half-space for the smaller direction) but
  // never violates feasibility.

  // Optional per-MWh ε on the loss column.  Inert for this mode under
  // ordinary conditions (the LP already drives ``ℓ`` to its tangent
  // envelope, which is the unique minimiser at any chosen ``f``) but
  // preserved so the wiring stays consistent with the other PWL paths
  // — a future contract that needs ε > 0 here works identically.
  const double loss_block_cost = config.loss_cost_eps > 0.0
      ? CostHelper::block_ecost(scenario, stage, block, config.loss_cost_eps)
      : 0.0;

  // Physical upper bound on ℓ instead of an unbounded (``DblMax``) column.
  // The chord row above already drives ℓ to its tangent envelope, so this
  // bound is NON-BINDING — ``ℓ ≤ (R/V²)·f²`` and ``|f| ≤ effective_fmax``
  // give ``ℓ ≤ k_loss·m²`` with ``m = max(fmax_phys, effective_fmax)``,
  // which dominates every secant/piecewise-secant chord value on
  // ``[0, effective_fmax]`` for any ``nseg``.  Leaving it at +∞ instead
  // hands first-order / dual-simplex solvers (cuOpt PDLP) an unboxed
  // direction: the iterate escapes to ~1e22 and the solve diverges.  The
  // optimum and all duals are unchanged (CPLEX value identical).  When
  // ``k_loss == 0`` (lossless line) the bound is 0, pinning the vestigial
  // column at ℓ = 0, which the zero-slope chord already implies.
  const double loss_fmax = std::max(fmax_phys, effective_fmax);
  const double loss_uppb = k_loss * loss_fmax * loss_fmax;

  const auto loss_col = lp.add_col({
      .lowb = 0.0,
      .uppb = loss_uppb,
      .cost = loss_block_cost,
      // Pin the Ruiz column scale (see ``flow_col`` above): the chord/tangent
      // ``R/V²`` coefficients otherwise inflate this column's scaled bound.
      .pin_scale = true,
      .class_name = Line::class_name.full_name(),
      .variable_name = LineLP::LosspName,
      .variable_uid = uid,
      .context = block_ctx,
  });
  result.lossp_col = loss_col;

  // ── Loss allocation (forced ``split`` for signed flow) ─────────────
  // The signed-flow formulation has ONE loss column ``ℓ ≥ 0`` and
  // ONE flow column ``f ∈ [−tmax_ba, +tmax_ab]``.  The per-direction
  // ``sender`` and ``receiver`` allocation modes assume a FIXED
  // physical direction (sender = bus_a, receiver = bus_b) — but for
  // signed flow the actual physical sender/receiver SWAP when f<0.
  // ``apply_loss_allocation`` cannot express direction-dependent
  // allocation in pure LP (would need binary indicators or SOS1).
  // Concretely, emitting ``-1·ℓ`` on bus_a (sender mode) is WRONG
  // when f<0 because bus_a is the actual receiver in that block:
  // the LP gets a free arbitrage channel at the misallocated bus
  // whenever it has negative LMP, inflating ``ℓ`` asymmetrically
  // between the two flow directions.  Empirically (v0407 K=12 L=3
  // ε=0.01): allocation=receiver showed R/A=1.86× for A→B blocks
  // but R/A=2.57× for B→A blocks, with some lines giving R/A=0.8×
  // in one direction and R/A=18× in the other.
  //
  // The only sign-symmetric allocation is ``split`` (each bus pays
  // 50% of ℓ regardless of flow direction), so we force it here.
  // A one-time warning fires if the caller requested a different
  // mode so the override is visible.  Per-direction loss accounting
  // for signed flow would require splitting ℓ into ℓ_p + ℓ_n with
  // SOS1-style mutual exclusion — out of scope for pure LP.
  if (config.allocation != LossAllocationMode::split) {
    static bool warned = false;
    if (!warned) {
      spdlog::warn(
          "tangent_signed_flow: loss_allocation_mode '{}' is "
          "direction-blind for signed flow and was overridden to "
          "'split' (only sign-symmetric mode).  This is the cure for "
          "the R/A asymmetry observed in v0407 (see task #107).",
          (config.allocation == LossAllocationMode::sender) ? "sender"
                                                            : "receiver");
      warned = true;
    }
  }
  apply_loss_allocation(brow_a, brow_b, loss_col, LossAllocationMode::split);

  // ── |f|-envelope columns + chord upper bound ───────────────────────
  //
  // Three regimes, all converging to a tight piecewise-linear over-
  // approximation of the quadratic loss ``c·f²`` (``c = R/V²``):
  //
  //   (A) **Coffrin classic, L = 1 (default)**.  Single segment col
  //       ``v ∈ [0, fmax]`` with ``v ≥ |f|`` (two abs rows) and
  //       chord ``ℓ ≤ c·fmax · v`` (origin-to-fmax secant).  Worst-
  //       case overstatement ``c·fmax²/4``.
  //
  //   (B) **ε-rely L-secant, L > 1, !use_sos2 (issue #504 / this
  //       commit)**.  Natural extension of (A) to ``L`` segment cols
  //       ``v_l ∈ [0, w]``,  ``w = fmax/L``, tied to ``|f|`` via the
  //       same two abs rows ``Σ v_l ≥ ±f``.  Chord becomes the
  //       piecewise secant ``ℓ ≤ Σ chord_slope_l · v_l``  with
  //       ``chord_slope_l = c·w·(2l−1)``.  With ``loss_cost_eps > 0``
  //       on Σ v_l, the LP picks ``Σ v_l = |f|`` at LP optimum (the
  //       cheapest feasible Σ).  The v distribution is then **LP-
  //       indifferent**: the chord row is INACTIVE at LP optimum
  //       (the K-tangent lower bound on ℓ binds first), so the LP
  //       has no obj preference between e.g. ``v = {50, 25, 0, 0}``
  //       (bottom-up secant, chord_min) and ``v = {0, 25, 50, 0}``
  //       (degenerate, chord ≫ secant).  Both deliver the same ℓ_LP
  //       = max_tangent(f).  What ``ε > 0``  buys is purely
  //       structural: keeps ``Σ v_l``  bounded at ``|f|``  rather
  //       than letting it inflate to ``L·w = envelope``  (which
  //       would push the chord up to the loose constant ceiling
  //       ``c·envelope²·(2L−1)`` — STRICTLY WORSE than L=1).
  //       Pure LP, full envelope reachable, no MIP.  Worst-case
  //       overstatement (over the LP-indifferent set of v
  //       distributions) drops to ``c·fmax²/(4·L²)`` (O(1/L²)) at
  //       the bottom-up corner — the lambda-form (regime C) attains
  //       the same tightness structurally on every solve.
  //
  //   (C) **Lambda-form SOS2, L > 1, use_sos2 (issue #504 SOS2 path,
  //       lambda-form refactor)**.  Symmetric ``2L+1`` breakpoint
  //       weight cols ``λ_l ∈ [0, 1]``  at ``b_l = (l-L)·w``  for
  //       l = 0..2L (covers [-fmax, +fmax] including the sign).
  //       Convexity row ``Σ λ_l = 1``,  flow row ``f = Σ b_l · λ_l``,
  //       chord row ``ℓ ≤ Σ c·b_l² · λ_l``,  SOS2 on ``{λ_0, …,
  //       λ_{2L}}``.  Canonical Beale–Tomlin: at most two adjacent λ_l
  //       non-zero ⇒ LP lands on or between two adjacent breakpoints
  //       ⇒ piecewise-linear interpolation reaching ``|f| ≤ fmax``.
  //       MIP because of SOS2, slightly more cols (2L+1 vs L), but no
  //       ε needed and no flow cap.  Same O(1/L²) chord tightness as
  //       (B).
  //
  // STRUCTURAL CAVEAT — regime (B) without ε.  ``Σ v_l ≥ |f|`` is
  // LP-equivalent to ``fp + fn ≥ |fp − fn|`` so without ε > 0 the LP
  // inflates ``Σ v_l`` to ``fmax`` and the chord collapses to a loose
  // constant ceiling ``c·fmax · L·w·(2L-1) ≈ c·envelope²·(2L-1)`` —
  // STRICTLY WORSE than L=1.  ``make_config`` warns one-shot when
  // ``L > 1 && !use_sos2 && loss_cost_eps <= 0``.
  const int L = std::max(1, config.nseg_secant);
  const double seg_width = effective_fmax / static_cast<double>(L);

  // Regime dispatch — see the doc block above for (A)/(B)/(C):
  //   * L = 1                  → segment-form, single secant (regime A)
  //   * L > 1 && !use_sos2     → segment-form, ε-rely (regime B)
  //   * L > 1 &&  use_sos2     → lambda-form SOS2 (regime C)
  // ``make_config`` already sanitises ``use_sos2 && L<=1`` → false
  // so ``config.use_sos2`` here implies L > 1.
  if (config.use_sos2) {
    // ── Regime (C): Lambda-form SOS2 L-secant ────────────────────────
    //
    // ``2L+1`` breakpoint weight cols ``λ_l ∈ [0, 1]`` placed at
    // ``b_l = (l − L) · seg_width``  for ``l = 0..2L``:
    //   l=0  → b = -fmax  (lower envelope)
    //   l=L  → b = 0      (zero-flow breakpoint)
    //   l=2L → b = +fmax  (upper envelope)
    //
    // Rows:
    //   convexity   :  Σ λ_l = 1
    //   flow link   :  Σ b_l · λ_l − f = 0
    //   chord (≤)   :  Σ c·b_l² · λ_l − ℓ ≥ 0   ⇒  ℓ ≤ Σ c·b_l² · λ_l
    //   SOS2        :  on {λ_0, …, λ_{2L}}
    //
    // SOS2's "at most 2 adjacent non-zero" property combined with
    // convexity makes the LP land between two adjacent breakpoints
    // ``b_l ≤ f ≤ b_{l+1}`` with chord = secant(c·f²) on that
    // segment.  Tight at every breakpoint.  Reaches |f| = fmax at
    // both ends — NO ``2w`` segment cap.
    //
    // The legacy segment-form ``f_abs_col`` hook is not populated
    // under this regime (no segment col exists); downstream consumers
    // that need ``|f|`` should sum the SOS2-active lambdas weighted
    // by ``|b_l|``.
    const int lambda_count = (2 * L) + 1;
    std::vector<ColIndex> lambda_cols;
    lambda_cols.reserve(static_cast<std::size_t>(lambda_count));
    for (int l = 0; l < lambda_count; ++l) {
      lambda_cols.push_back(lp.add_col({
          .lowb = 0.0,
          .uppb = 1.0,
          .cost = 0.0,
          // SOS2 lambda weight — keep the [0,1] convex weight scale-exempt
          // (pin_scale) so Ruiz/equilibration cannot expand its bound and
          // distort the SOS2 ladder.
          .pin_scale = true,
          .class_name = Line::class_name.full_name(),
          .variable_name = LineLP::FlowLambdaName,
          .variable_uid = uid,
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid(), l),
      }));
    }

    // Convexity row: Σ λ_l = 1
    {
      auto crow =
          SparseRow {
              .class_name = Line::class_name.full_name(),
              .constraint_name = loss_lambda_convex_constraint_name,
              .variable_uid = uid,
              .context = make_block_context(
                  scenario.uid(), stage.uid(), block.uid(), 0),
          }
              .equal(1.0);
      crow.reserve(static_cast<std::size_t>(lambda_count));
      for (const auto& lc : lambda_cols) {
        crow[lc] = +1.0;
      }
      [[maybe_unused]] auto idx = lp.add_row(std::move(crow));
    }

    // Flow row: Σ b_l · λ_l − f = 0   ⇔   Σ b_l · λ_l = f
    {
      auto frow =
          SparseRow {
              .class_name = Line::class_name.full_name(),
              .constraint_name = loss_lambda_flow_constraint_name,
              .variable_uid = uid,
              .context = make_block_context(
                  scenario.uid(), stage.uid(), block.uid(), 1),
          }
              .equal(0.0);
      frow.reserve(static_cast<std::size_t>(lambda_count) + 1);
      for (int l = 0; l < lambda_count; ++l) {
        const double b_l = static_cast<double>(l - L) * seg_width;
        if (std::abs(b_l) > 0.0) {
          frow[lambda_cols[static_cast<std::size_t>(l)]] = b_l;
        }
      }
      frow[flow_col] = -1.0;
      [[maybe_unused]] auto idx = lp.add_row(std::move(frow));
    }

    // Chord row: Σ c·b_l² · λ_l − ℓ ≥ 0   (ℓ ≤ Σ c·b_l² · λ_l).
    // Same row orientation and ``loss_row_scale`` as the K tangent
    // rows below.  Context tag matches the segment-form chord row
    // (``nseg + 1``) so write-out keeps a single "chord upper bound"
    // label regardless of which regime emitted it.
    {
      auto ubrow =
          SparseRow {
              .class_name = Line::class_name.full_name(),
              .constraint_name = loss_link_constraint_name,
              .variable_uid = uid,
              .context = make_block_context(
                  scenario.uid(), stage.uid(), block.uid(), nseg + 1),
          }
              .greater_equal(0.0);
      ubrow.reserve(static_cast<std::size_t>(lambda_count) + 1);
      for (int l = 0; l < lambda_count; ++l) {
        const double b_l = static_cast<double>(l - L) * seg_width;
        const double coef = k_loss * b_l * b_l;
        if (coef > 0.0) {
          ubrow[lambda_cols[static_cast<std::size_t>(l)]] =
              +coef * config.loss_row_scale;
        }
      }
      ubrow[loss_col] = -config.loss_row_scale;
      [[maybe_unused]] auto idx = lp.add_row(std::move(ubrow));
    }

    // SOS2 on the full lambda ladder.  Backends without SOS2
    // (CBC/OSI default-throw) raise a structured error from
    // ``SolverBackend::add_sos2`` at ``load_flat`` time.
    lp.add_sos2(
        std::span<const ColIndex> {lambda_cols.data(), lambda_cols.size()});
  } else {
    // ── Regimes (A) and (B): Segment-form L-secant ───────────────────
    //
    // L cols ``v_l ∈ [0, w]`` with ``w = fmax/L``, tied to ``|f|`` via
    // the two abs rows ``Σ v_l ≥ ±f``.  Chord ``ℓ ≤ Σ chord_slope_l ·
    // v_l`` with ``chord_slope_l = c·w·(2l−1)``.
    //
    // ε (``loss_cost_eps > 0``) on Σ v_l is REQUIRED for L > 1 to
    // close the inflate-v arbitrage by forcing ``Σ v_l = |f|``  at
    // LP optimum.  The v distribution is then LP-indifferent (the
    // chord row is INACTIVE at the LP optimum — the K-tangent lower
    // bound binds first), but ``Σ v_l = |f|``  alone is enough to
    // keep the chord bounded by the piecewise secant rather than
    // the loose constant ceiling.  See the (B) doc block above for
    // the full explanation.  L=1 also benefits from ε but works
    // (loosely) without it because there's only one segment to
    // inflate.

    // ε contributes a tiny per-MWh cost on Σ v_l.  ``make_config``
    // already converted nothing → ``loss_cost_eps`` is the raw user
    // value (0 if unset).  The foot-gun warning fires on
    // ``L > 1 && ε == 0`` to surface the misconfig.
    const double v_cost = config.loss_cost_eps > 0.0
        ? CostHelper::block_ecost(scenario, stage, block, config.loss_cost_eps)
        : 0.0;

    // Helper: build the context for v_col[l].  L = 1 uses the legacy
    // 3-tuple ``block_ctx`` so write_lp keeps emitting
    // ``…flow_abs_<scen>_<stage>_<block>`` unchanged; L > 1 appends
    // the 1-based segment index so cols distinguish as
    // ``…flow_abs_l1`` / ``…flow_abs_l2`` / ….
    auto v_ctx_for = [&](int l) -> LpContext
    {
      if (L == 1) {
        return block_ctx;
      }
      return make_block_context(scenario.uid(), stage.uid(), block.uid(), l);
    };

    std::vector<ColIndex> v_cols;
    v_cols.reserve(static_cast<std::size_t>(L));
    for (int l = 1; l <= L; ++l) {
      v_cols.push_back(lp.add_col({
          .lowb = 0.0,
          .uppb = seg_width,
          .cost = v_cost,
          // Pin the Ruiz column scale (see ``flow_col``): the |f| abs column
          // sits in the same chord/tangent loss rows that otherwise inflate
          // its scaled bound far past the physical ``fmax``.
          .pin_scale = true,
          .class_name = Line::class_name.full_name(),
          .variable_name = LineLP::FlowAbsName,
          .variable_uid = uid,
          .context = v_ctx_for(l),
      }));
    }
    // FIRST segment col is the public ``f_abs_col`` (back-compat with
    // legacy L=1 tests; gives downstream code a hook to reach the
    // segment family on L > 1).
    result.f_abs_col = v_cols.front();

    // Helper: emit one abs row ``Σ v_l + flow_sign · f ≥ 0``.
    //   flow_sign = -1  ⇒  Σ v_l ≥ +f  (seg tag 1)
    //   flow_sign = +1  ⇒  Σ v_l ≥ −f  (seg tag 2)
    auto emit_abs_row = [&](int seg_tag, double flow_sign)
    {
      auto row =
          SparseRow {
              .class_name = Line::class_name.full_name(),
              .constraint_name = flow_abs_constraint_name,
              .variable_uid = uid,
              .context = make_block_context(
                  scenario.uid(), stage.uid(), block.uid(), seg_tag),
          }
              .greater_equal(0.0);
      row.reserve(static_cast<std::size_t>(L) + 1);
      for (const auto vc : v_cols) {
        row[vc] = +1.0;
      }
      row[flow_col] = flow_sign;
      [[maybe_unused]] auto idx = lp.add_row(std::move(row));
    };
    emit_abs_row(1, -1.0);  // Σ v_l ≥ +f
    emit_abs_row(2, +1.0);  // Σ v_l ≥ −f

    // Chord row: ℓ ≤ Σ chord_slope_l · v_l ⇔ Σ chord_slope_l · v_l − ℓ ≥ 0.
    // L = 1 reduces to the legacy ``ℓ ≤ (R·fmax/V²) · v`` chord
    // (chord_slope_1 = k_loss · fmax · 1 = k_loss · effective_fmax).
    // L > 1 emits the per-segment chord slopes
    //   chord_slope_l = k_loss · seg_width · (2l − 1)
    // matching the secant of the convex quadratic on
    // ``[(l−1)w, l·w]``.
    {
      auto ubrow =
          SparseRow {
              .class_name = Line::class_name.full_name(),
              .constraint_name = loss_link_constraint_name,
              .variable_uid = uid,
              .context = make_block_context(
                  scenario.uid(), stage.uid(), block.uid(), nseg + 1),
          }
              .greater_equal(0.0);
      ubrow.reserve(static_cast<std::size_t>(L) + 1);
      for (int l = 1; l <= L; ++l) {
        const double chord_slope =
            k_loss * seg_width * static_cast<double>((2 * l) - 1);
        ubrow[v_cols[static_cast<std::size_t>(l - 1)]] =
            +chord_slope * config.loss_row_scale;
      }
      ubrow[loss_col] = -config.loss_row_scale;
      [[maybe_unused]] auto idx = lp.add_row(std::move(ubrow));
    }
  }

  // ── K tangent inequalities ──────────────────────────────────────────
  // Tangent points: f_k = fmax · (2k − K − 1) / K, k = 1..K.
  // For K=5 / fmax=10: f_k ∈ {−8, −4, 0, +4, +8}.  Uniform spacing on
  // (−fmax, +fmax) excluding the endpoints (the col bound on ``ℓ``
  // already pins the quadratic at f = ±fmax).
  //
  // Row form (after row scaling ``s = loss_row_scale``):
  //   s · ℓ − 2 · s · k_loss · f_k · f ≥ −s · k_loss · f_k²
  // i.e. ``loss ≥ (2·R/V²)·f_k·f − (R/V²)·f_k²``.
  //
  // Per-tangent dropout (matches ``add_tangents`` / ``add_segments``):
  // if the slope coefficient ``2·k_loss·f_k`` lands below the pre-scale
  // or post-scale numerical noise floor, the row degenerates to
  // ``loss ≥ −k_loss·f_k² ≤ 0`` (always satisfied by ``loss ≥ 0``).
  // Stamping it just pollutes the coefficient-range statistics.
  for (const auto k : iota_range(1, nseg + 1)) {
    const double f_k = effective_fmax * static_cast<double>((2 * k) - nseg - 1)
        / static_cast<double>(nseg);
    const double slope_coef = 2.0 * k_loss * f_k;
    const double scaled_slope = slope_coef * config.loss_row_scale;
    if (std::abs(slope_coef) < kLossCoeffTolerance
        || std::abs(scaled_slope) < kLossLpRowTolerance)
    {
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
            .greater_equal(-k_loss * f_k * f_k * config.loss_row_scale);
    trow.reserve(2);
    trow[loss_col] = +config.loss_row_scale;
    trow[flow_col] = -scaled_slope;
    [[maybe_unused]] auto idx = lp.add_row(std::move(trow));
  }

  // ── Capacity row (expandable lines only) ────────────────────────────
  // Two-sided capacity: ``capacity − |f| ≥ 0`` is non-linear, so we
  // express it via two one-sided rows ``capacity − f ≥ 0`` and
  // ``capacity + f ≥ 0`` reusing ``add_capacity_row`` for the +
  // direction and an inline mirror for the − direction.
  if (capacity_col) {
    // +direction: capacity_col − flow_col ≥ 0  (binds when f > 0)
    result.capp_row = add_capacity_row(lp,
                                       scenario,
                                       stage,
                                       block,
                                       LineLP::CapacitypName,
                                       uid,
                                       *capacity_col,
                                       flow_col);
    // −direction: capacity_col + flow_col ≥ 0  (binds when f < 0)
    auto neg_cap_row =
        SparseRow {
            .class_name = Line::class_name.full_name(),
            .constraint_name = LineLP::CapacitynName,
            .variable_uid = uid,
            .context =
                make_block_context(scenario.uid(), stage.uid(), block.uid()),
        }
            .greater_equal(0);
    neg_cap_row[*capacity_col] = 1;
    neg_cap_row[flow_col] = +1;
    result.capn_row = lp.add_row(std::move(neg_cap_row));
  }

  return result;
}

/// `add_piecewise` definition — see forward declaration above.
///
/// For every non-`tangent` layout (uniform / equal_error / midpoint)
/// we delegate to `add_bidirectional` to defuse the phantom-flow
/// arbitrage of the legacy single-direction formulation (see the
/// forward declaration's docstring for the empirical evidence on the
/// CEN PCP v0407 case).  The bidirectional structure prevents the LP
/// from inflating `fp + fn` to dump quadratic loss at a single
/// negative-LMP bus, because each direction's loss is paid at its
/// own direction's receiver.
///
/// The `tangent` layout is preserved on the legacy single-direction
/// shared-loss path (`add_piecewise_shared`): the tangent
/// inequalities reference both `fp_col` and `fn_col` symmetrically on
/// a single shared `loss_col`, and the outer-approximation math has
/// no obvious per-direction counterpart in the current code base.
/// The phantom-flow risk for tangent layout is bounded structurally:
/// the tangent rows enforce `loss ≥ 2 · k · t · (fp + fn) − k · t²`
/// (with the LP minimising `loss`), so inflating `fp + fn` can only
/// raise the binding tangent's lower bound on `loss`.  Combined with
/// the fact that `tangent` is currently a documented placeholder
/// (`add_tangents` is the only non-`add_segments` PWL path), the
/// single-direction route is acceptable for that mode.
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
  if (config.pwl_layout == LinePwlLayout::tangent) {
    return add_piecewise_shared(config,
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
  }
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
/// @param lf_out  Out-param filled (cleared first) with the per-segment
///                physical loss factor `lf_k`, parallel to the returned
///                segment columns.  Used by `add_piecewise_direct` so
///                the output layer can reconstruct the exact
///                LP-consistent loss `Σ_k lf_k · seg_k_sol`.
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
    bool enforce_capacity,
    std::vector<double>& lf_out)
{
  lf_out.clear();
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
    lf_out.push_back(lf_k);
  }

  return seg_cols;
}

/// PLP-direct piecewise-linear: no loss variables, no loss-tracking
/// rows, no aggregator columns, no link rows.  Per-segment bus
/// stamps encode the quadratic loss curve directly.  Requires no
/// capacity column.
///
/// Variables per block: 2·K segment cols (K positive + K negative).
/// Constraints per block: 0 extra rows (segments stamp directly into
///                        the existing sending/receiving bus-balance
///                        rows).
///
/// ⚠ Phantom-flow caveat: with no link row and no fp/fn aggregator,
/// the LP can have BOTH positive- and negative-direction segments
/// non-zero simultaneously.  In meshed networks with negative-LMP
/// receiving buses this is exploited to dump quadratic loss "for
/// free" at the cheap bus.  Use ``add_bidirectional`` (per-direction
/// loss column at each direction's OWN receiver) when phantom-flow
/// purity matters; ``piecewise_direct`` is only safe in networks
/// that never see negative LMPs at receiving buses.  See
/// ``LineLossesMode::piecewise_direct`` in include/gtopt/line_enums.hpp
/// for full discussion.
///
/// Ref: PLP Fortran `genpdlin.f` (GenPDLinA).
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
  std::vector<double> seg_p_loss;
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
                                         enforce_capacity,
                                         seg_p_loss);

  std::vector<double> seg_n_loss;
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
                                         enforce_capacity,
                                         seg_n_loss);

  return {
      .fp_col = {},
      .fn_col = {},
      .fp_loss = {},
      .fn_loss = {},
      .lossp_col = {},
      .lossn_col = {},
      .capp_row = {},
      .capn_row = {},
      .seg_p_cols = std::move(seg_p_cols),
      .seg_n_cols = std::move(seg_n_cols),
      .seg_p_loss = std::move(seg_p_loss),
      .seg_n_loss = std::move(seg_n_loss),
      .flow_col = {},
      .f_abs_col = {},
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

  const double budget = opts.err_pct * L_total;

  // ── Phase 1' — recompute K under the two-sided budget ────────────
  // The two-sided worst-case bound (Σ_uniform L/(4K²) ≤ budget AND
  // Σ_midpoint L/(4K²) ≤ budget) gives 2× total worst-case headroom
  // vs the unsigned single-sided budget the cube-root rule used in
  // ``compute_adaptive_loss_segments`` (Phase 1).  Re-run the cube-
  // root rule here with the effective ``2 × budget``: KKT gives
  //
  //     K_i = ⌈c · L_i^(1/3)⌉,  c = √(S / (4·2·budget))
  //         ≈ K_i_old / √2  ≈ 71 % of Phase 1's K
  //
  // — i.e. ~29 % fewer LP segments per line on the unclamped middle
  // band.  Measured 30 % Σ K savings on CEN PCP weekly at err_pct
  // = 0.01 default.  Floor / ceiling clamps still bound K.
  double S_dyn = 0.0;
  for (const auto& ln : lossy) {
    S_dyn += std::cbrt(ln.L);
  }
  const double B_dyn = 2.0 * budget;
  const double c_dyn = (B_dyn > 0.0) ? std::sqrt(S_dyn / (4.0 * B_dyn)) : 0.0;
  for (auto& ln : lossy) {
    const double k_raw = c_dyn * std::cbrt(ln.L);
    const int k = std::clamp(
        static_cast<int>(std::ceil(k_raw)), opts.floor, opts.ceiling);
    ln.K = k;
    out[ln.idx].K = k;
  }

  // Phase 2 — system-wide signed mean error, all-uniform initial.
  //   E_sys = Σ_uniform L_i / (6 K_i²)  −  Σ_midpoint L_i / (12 K_i²)
  double running = 0.0;
  double all_uniform_worst = 0.0;
  for (const auto& ln : lossy) {
    const double kk = static_cast<double>(ln.K) * static_cast<double>(ln.K);
    running += ln.L / (6.0 * kk);
    all_uniform_worst += ln.L / (4.0 * kk);
  }
  // Early return: all-uniform satisfies BOTH the mean budget AND the
  // one-sided worst-case budget (refined 2026-05-29 — without the
  // worst-case check the all-uniform path returned even when worst_uni
  // was over budget, violating the documented two-sided budget invariant).
  if (running <= budget && all_uniform_worst <= budget) {
    return out;
  }

  // Sort by mean-error contribution descending so the heaviest is
  // first.  ``L / K²`` is monotone in ``L_max,i / (6 K_i²)`` so it
  // gives the same ordering with one fewer division.
  std::ranges::sort(lossy,
                    [](const Lossy& a, const Lossy& b) noexcept
                    {
                      const auto ka = static_cast<double>(a.K);
                      const auto kb = static_cast<double>(b.K);
                      return (a.L / (ka * ka)) > (b.L / (kb * kb));
                    });

  // Two-sided worst-case tracking: each layout's error has a fixed
  // sign so the system-wide worst-case bound becomes
  //   Σ_uniform L/(4K²) ≤ budget  AND  Σ_midpoint L/(4K²) ≤ budget
  // (each side ≤ budget independently — see the Python
  // ``_apply_dynamic_loss_layout`` docstring for the full derivation).
  double worst_uni = all_uniform_worst;
  double worst_mid = 0.0;

  // Phase 2 — original mean-budget-driven flipping (unchanged contract;
  // pins the existing dynamic-mode tests).  Each flip subtracts its
  // worst-case contribution from worst_uni and adds it to worst_mid.
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
    worst_uni -= contribution;
    worst_mid += contribution;
  }

  // Phase 2.5 — extra flips to balance worst-case across layouts so
  // Phase 1.5 below has uniform-side headroom for K reduction.  Only
  // flips that (a) don't burst the mean budget AND (b) strictly
  // reduce |worst_uni − worst_mid| are accepted.
  for (const auto& ln : lossy) {
    if (out[ln.idx].layout == LinePwlLayout::midpoint) {
      continue;
    }
    const double contribution =
        ln.L / (4.0 * static_cast<double>(ln.K) * static_cast<double>(ln.K));
    const double next_running = running - contribution;
    if (std::abs(next_running) > budget) {
      continue;  // would burst mean budget
    }
    const double old_imbalance = std::abs(worst_uni - worst_mid);
    const double new_imbalance =
        std::abs((worst_uni - contribution) - (worst_mid + contribution));
    if (new_imbalance >= old_imbalance) {
      continue;  // would not improve worst-case balance
    }
    out[ln.idx].layout = LinePwlLayout::midpoint;
    running = next_running;
    worst_uni -= contribution;
    worst_mid += contribution;
  }

  // ── Phase 1.5: try to reduce K on individual lines ───────────────
  // Two-sided worst-case bound (refined 2026-05-29): each layout's
  // error sign is fixed, so the signed system-wide error is bounded
  // by ``max(worst_uni, worst_mid) ≤ budget`` — i.e. each side
  // independently.  This gives ``2 × budget`` total worst-case
  // headroom vs the prior unsigned ``Σ_all L/(4K²) ≤ budget``
  // formulation.  Phase 1.5 exploits the headroom by reducing K_i
  // on the side that still has slack.
  //
  // Reductions only fire when both Phase 2 + Phase 2.5 left actual
  // headroom on one side — typically when the K distribution avoids
  // ceiling clamps.  On CEN-PCP-shape bundles where Phase 1's KKT
  // cube-root rule already lands within both sides' budgets, this is
  // a cheap no-op (the early-return catches the all-uniform case
  // above, and Phase 2/2.5 already balanced when needed).
  //
  // ``worst_uni`` and ``worst_mid`` are already maintained by the
  // Phase 2 + Phase 2.5 flipping loops above — reuse them as the
  // starting state for Phase 1.5.
  bool changed = true;
  while (changed) {
    changed = false;
    // Stable rebuild of the descending-K order each pass.
    std::vector<std::size_t> order(lossy.size());
    std::ranges::iota(order, std::size_t {0});
    std::ranges::sort(order,
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
      const double delta_worst =
          (ln.L / (4.0 * new_kk)) - (ln.L / (4.0 * old_kk));
      // Per-side worst-case check: only the side this line lives on
      // grows; the other side is unchanged.
      if (dst.layout == LinePwlLayout::midpoint) {
        if (worst_mid + delta_worst > budget) {
          continue;  // negative-side budget would burst
        }
      } else {
        if (worst_uni + delta_worst > budget) {
          continue;  // positive-side budget would burst
        }
      }
      const double old_m = (dst.layout == LinePwlLayout::midpoint)
          ? (-ln.L / (12.0 * old_kk))
          : (+ln.L / (6.0 * old_kk));
      const double new_m = (dst.layout == LinePwlLayout::midpoint)
          ? (-ln.L / (12.0 * new_kk))
          : (+ln.L / (6.0 * new_kk));
      const double new_running = running - old_m + new_m;
      if (std::abs(new_running) > budget) {
        continue;  // mean budget would burst
      }
      // Commit
      dst.K = new_k;
      if (dst.layout == LinePwlLayout::midpoint) {
        worst_mid += delta_worst;
      } else {
        worst_uni += delta_worst;
      }
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
          || config.mode == LineLossesMode::piecewise_direct
          || config.mode == LineLossesMode::tangent_signed_flow))
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

    case LineLossesMode::tangent_signed_flow:
      return add_tangent_signed_flow(config,
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
