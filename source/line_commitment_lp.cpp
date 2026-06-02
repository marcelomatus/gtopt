/**
 * @file      line_commitment_lp.cpp
 * @brief     LP formulation for Optimal Transmission Switching (issue #509)
 * @date      2026-06-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * v0 implementation: per (line, scenario, stage, block) emits a binary
 * status column ``u_l`` and two capacity gating rows that force the
 * flow to zero when the breaker opens (``u_l = 0``).  The KVL big-M
 * disjunction for Kirchhoff mode is documented in issue #509 §"LP
 * formulation" §3 but deferred to v0.5 — see ``line_commitment_lp.hpp``.
 */

#include <cmath>
#include <numbers>

#include <gtopt/line_commitment_lp.hpp>
#include <gtopt/line_enums.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

LineCommitmentLP::LineCommitmentLP(const LineCommitment& lc,
                                   const InputContext& ic)
    : Base(lc, ic, Element::class_name)
    , line_index_(ic.element_index(line_sid()))
    , fixed_status_(
          ic, Element::class_name, id(), std::move(object().fixed_status))
{
}

namespace
{

/// Resolve the per-block flow column owned by the linked LineLP.  We
/// check the three line-loss code paths in order and return the first
/// match:
///   * ``flowp_cols_at`` / ``flown_cols_at`` — split flow path (``none``
///     / ``linear`` / ``piecewise`` / ``bidirectional``)
///   * ``flows_cols_at`` — single signed flow (``tangent_signed_flow``)
struct FlowCols
{
  std::optional<ColIndex> fp;  ///< Forward (A→B) flow column.
  std::optional<ColIndex> fn;  ///< Reverse (B→A) flow column.
  std::optional<ColIndex> fs;  ///< Single signed flow column.
};

[[nodiscard]] FlowCols resolve_flow_cols(const LineLP& line_lp,
                                         const ScenarioLP& scenario,
                                         const StageLP& stage,
                                         BlockUid buid)
{
  FlowCols out;

  const auto& fp_map = line_lp.flowp_cols_at(scenario, stage);
  if (const auto it = fp_map.find(buid); it != fp_map.end()) {
    out.fp = it->second;
  }
  const auto& fn_map = line_lp.flown_cols_at(scenario, stage);
  if (const auto it = fn_map.find(buid); it != fn_map.end()) {
    out.fn = it->second;
  }
  const auto& fs_map = line_lp.flows_cols_at(scenario, stage);
  if (const auto it = fs_map.find(buid); it != fs_map.end()) {
    out.fs = it->second;
  }
  return out;
}

}  // namespace

bool LineCommitmentLP::add_to_lp(SystemContext& sc,
                                 const ScenarioLP& scenario,
                                 const StageLP& stage,
                                 LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  // Chronological-stage guard.  Mirrors ``Commitment`` semantics — on
  // non-chronological stages (duration-weighted representative blocks)
  // the binary u_l has no cross-block meaning; silently skip so the
  // line behaves as if no LineCommitment row existed.
  if (!stage.is_chronological()) {
    return true;
  }

  auto&& line_lp = sc.element(line_index_);
  if (!line_lp.is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();
  if (blocks.empty()) {
    return true;
  }

  static constexpr std::string_view cname = Element::class_name.full_name();
  static constexpr auto ampl_name = Element::class_name.snake_case();
  const auto cuid = uid();

  const auto& lc = line_commitment();
  const auto is_relax = lc.relax.value_or(false)
      || sc.simulation().phases()[stage.phase_index()].is_continuous();
  const auto is_must_run = lc.must_run.value_or(false);

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};

  BIndexHolder<ColIndex> ucols;
  BIndexHolder<RowIndex> cprows;
  BIndexHolder<RowIndex> cnrows;
  map_reserve(ucols, blocks.size());
  map_reserve(cprows, blocks.size());
  map_reserve(cnrows, blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto ctx = make_block_context(scenario.uid(), stage.uid(), buid);

    // Locate the flow column(s) owned by the linked LineLP for this
    // (scenario, stage, block).  When LineLP elided the block entirely
    // (``in_service = 0`` / inactive stage), there is no flow column to
    // gate — skip silently; the line is already a true open circuit
    // and the commitment binary has no role.
    const auto flow = resolve_flow_cols(line_lp, scenario, stage, buid);
    if (!flow.fp && !flow.fn && !flow.fs) {
      continue;
    }

    // Per-block flow caps.  Prefer the per-(stage, block) ``tmax_*``
    // schedule values.  When unset (capacity inherits from the
    // expansion variable) read the upper bound directly off the
    // flow column the LineLP stamped — that bound already
    // encodes whatever stage_capacity / expmod logic ran upstream.
    const auto sentinel = LinearProblem::DblMax;
    auto block_tmax_ab =
        line_lp.param_tmax_ab(stage.uid(), buid).value_or(sentinel);
    auto block_tmax_ba =
        line_lp.param_tmax_ba(stage.uid(), buid).value_or(sentinel);
    if (block_tmax_ab == sentinel) {
      if (flow.fp) {
        block_tmax_ab = lp.get_col_uppb(*flow.fp);
      } else if (flow.fs) {
        block_tmax_ab = lp.get_col_uppb(*flow.fs);
      }
    }
    if (block_tmax_ba == sentinel) {
      if (flow.fn) {
        block_tmax_ba = lp.get_col_uppb(*flow.fn);
      } else if (flow.fs) {
        // Signed flow's lower bound is -tmax_ba.  Negate to get
        // tmax_ba magnitude.
        block_tmax_ba = -lp.get_col_lowb(*flow.fs);
      }
    }

    // Resolve the per-block forced status: per-block ``fixed_status``
    // wins, else ``must_run`` covers the rest.  Per-block exogenous
    // outages (``Line.in_service = 0``) trump both — but that path
    // already skipped this block above (no flow column).
    const auto fixed =
        fixed_status_.at(stage.uid(), buid).value_or(LinearProblem::DblMax);
    const bool has_fixed = fixed != LinearProblem::DblMax;
    double u_lowb = 0.0;
    double u_uppb = 1.0;
    if (has_fixed) {
      u_lowb = u_uppb = (fixed >= 0.5) ? 1.0 : 0.0;
    } else if (is_must_run) {
      u_lowb = 1.0;
    }

    // First-block pin: ``LineCommitment.initial_status`` pins the
    // breaker state at ``t = 0``.  Lower-precedence than ``fixed_status``
    // / ``must_run`` (both of which set both bounds), higher-precedence
    // than the default ``[0, 1]`` (we only narrow one of the bounds).
    // Applied only to the FIRST block of the stage so that future
    // blocks remain decided by the solver.  Self-review P2-4 follow-up.
    if (!has_fixed && !is_must_run && buid == blocks.front().uid()
        && lc.initial_status.has_value())
    {
      const double init = lc.initial_status.value();
      u_lowb = u_uppb = (init >= 0.5) ? 1.0 : 0.0;
    }

    // Create binary status variable u_l.  IntegerScope::Block matches
    // the per-block grouping used by SimpleCommitment.
    const auto u_domain =
        is_relax ? IntegerDomain::Relaxed : IntegerDomain::Binary;
    const std::array<BlockUid, 1> u_blocks {buid};
    auto ucol = sc.add_integer_col(lp,
                                   IntegerVariable::key(scenario,
                                                        stage,
                                                        Element::class_name,
                                                        cuid,
                                                        StatusName,
                                                        IntegerScope::Block,
                                                        buid),
                                   SparseCol {
                                       .lowb = u_lowb,
                                       .uppb = u_uppb,
                                       .cost = 0.0,
                                       .class_name = cname,
                                       .variable_name = StatusName,
                                       .variable_uid = cuid,
                                       .context = ctx,
                                   },
                                   u_domain,
                                   IntegerScope::Block,
                                   buid,
                                   std::span<const BlockUid> {u_blocks});
    ucols[buid] = ucol;

    // ── Capacity gating ────────────────────────────────────────────
    //
    // Force ``f_l = 0`` whenever ``u_l = 0`` regardless of whether the
    // flow is split (``fp`` / ``fn``) or signed (``fs``).
    //
    //   +direction:  f^+ - F^max_ab · u  ≤ 0
    //   −direction:  F^max_ba · u + f^-  ≥ 0      (or for fs: + f)
    //
    // Both rows reduce to the existing physical capacity bound when
    // ``u_l = 1`` (the looser of the two flow-column upper bounds and
    // the gating row both bind at ``f = ±F^max``).  When ``u_l = 0``
    // both rows pin ``f = 0`` so the bus balance loses the line's
    // contribution — equivalent to an open circuit.
    //
    // The signed-flow path uses the SAME column for both rows; the
    // split path uses ``fp`` for the +direction row and ``fn`` for
    // the −direction row.  We treat them uniformly:

    // + direction gating row.
    {
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = CapacityPName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .less_equal(0.0);
      if (flow.fp) {
        row[*flow.fp] = 1.0;
      } else if (flow.fs) {
        row[*flow.fs] = 1.0;
      }
      row[ucol] = -block_tmax_ab;
      cprows[buid] = lp.add_row(std::move(row));
    }

    // − direction gating row.  For split flow this stamps ``fn``; for
    // signed flow it stamps ``-fs`` so the same ``≤ 0`` orientation
    // covers both legs:
    //
    //   split   : F^max_ba · u − fn ≥ 0  ⇔  fn − F^max_ba · u ≤ 0
    //   signed  : F^max_ba · u + fs ≥ 0  ⇔  -fs − F^max_ba · u ≤ 0
    {
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = CapacityNName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .less_equal(0.0);
      if (flow.fn) {
        row[*flow.fn] = 1.0;
      } else if (flow.fs) {
        row[*flow.fs] = -1.0;
      }
      row[ucol] = -block_tmax_ba;
      cnrows[buid] = lp.add_row(std::move(row));
    }

    // ── KVL big-M disjunction (Kirchhoff node_angle mode) ──────────
    //
    // The existing equality row stamped by ``LineLP::add_to_lp`` is
    //
    //     -θ_a + θ_b + x_τ · f = -φ_rad   (≡  f = b_eff · (θ_a − θ_b − φ))
    //
    // For OTS this must become a big-M disjunction so that ``u_l = 0``
    // decouples ``θ_a`` from ``θ_b`` (opening the breaker physically):
    //
    //     -θ_a + θ_b + x_τ · f + M·u_l  ≤  M - φ_rad
    //     -θ_a + θ_b + x_τ · f - M·u_l  ≥ -M - φ_rad
    //
    // At ``u_l = 1`` both inequalities collapse to the equality.  At
    // ``u_l = 0`` both rows allow ``-θ_a + θ_b ∈ [-M-φ, M-φ]``, i.e.
    // ``θ_a, θ_b`` decouple as long as ``M ≥ 2·θ_max + |φ|``.
    //
    // Default big-M = ``2·θ_max + |φ_rad|`` (Fisher 2008 baseline,
    // refined for the φ shift).  Per-line ``LineCommitment.kvl_big_m``
    // overrides — the v1 iterative-tightening pre-solve (Pineda 2024)
    // writes back into that override field.
    //
    // **Derivation note** (review P1-2): Issue #509 §"Big-M source"
    // states ``M_l = |b_eff| · (θ_max − θ_min)`` for the canonical
    // form ``f = b_eff · Δθ``.  Our row form ``-θ_a + θ_b + x_τ · f
    // = -φ`` is the canonical form multiplied by ``x_τ``, so the
    // canonical M ``|b_eff| · 2θ_max = 2θ_max/x_τ`` becomes ``x_τ ·
    // 2θ_max/x_τ = 2θ_max`` in our row.  Adding ``|φ|`` covers the
    // ``-θ_a + θ_b + φ_rad`` slack range under ``f = 0`` exactly.
    //
    // Skips silently in: cycle_basis Kirchhoff mode (no per-line KVL
    // row to rewrite — cycle_basis stamps cycle-aggregate rows
    // post-LineLP); transport mode (no KVL row at all); blocks where
    // LineLP omitted the row (``in_service = 0`` / no flow column).
    if (sc.options().use_kirchhoff()
        && sc.options().kirchhoff_mode() == KirchhoffMode::node_angle)
    {
      const auto& theta_rows = line_lp.theta_rows_at(scenario, stage);
      if (const auto trow_it = theta_rows.find(buid);
          trow_it != theta_rows.end())
      {
        const double phi_rad =
            line_lp.param_phase_shift_deg(stage.uid()).value_or(0.0)
            * std::numbers::pi / 180.0;
        const double theta_max = sc.options().theta_max();
        const auto big_m_override = lc.kvl_big_m.value_or(0.0);
        const double big_m = (big_m_override > 0.0)
            ? big_m_override
            : (2.0 * theta_max + std::abs(phi_rad));

        auto& original = lp.row_at(trow_it->second);
        // Original is an equality: lowb == uppb == -φ_rad.  Capture
        // its coefficient map and bounds before mutation so we can
        // build the lower-side ``≥`` row off the same template.
        const double rhs_eq = original.uppb;  // = -φ_rad
        // Copy the coefficient map — flat_map is range-iterable.
        SparseRow::cmap_t coeffs_copy = original.cmap;

        // 1) Mutate the original equality into the ``≤`` half:
        //
        //    -θ_a + θ_b + x_τ · f + M·u_l  ≤  M + rhs_eq
        //
        // Stamp +M on u_l and widen the bounds to a one-sided
        // inequality.
        original[ucol] = +big_m;
        original.uppb = big_m + rhs_eq;
        original.lowb = -SparseRow::DblMax;

        // 2) Add the lower-side ``≥`` row from the captured template:
        //
        //    -θ_a + θ_b + x_τ · f - M·u_l  ≥  -M + rhs_eq
        SparseRow lower_row {
            .lowb = -big_m + rhs_eq,
            .uppb = SparseRow::DblMax,
            .cmap = std::move(coeffs_copy),
            .class_name = cname,
            .constraint_name = KvlMinusName,
            .variable_uid = cuid,
            .context = ctx,
        };
        lower_row[ucol] = -big_m;
        [[maybe_unused]] auto lower_idx = lp.add_row(std::move(lower_row));
      }
    }
  }

  // Store index holders.
  if (!ucols.empty()) {
    status_cols_[st_key] = std::move(ucols);
  }
  if (!cprows.empty()) {
    capacity_p_rows_[st_key] = std::move(cprows);
  }
  if (!cnrows.empty()) {
    capacity_n_rows_[st_key] = std::move(cnrows);
  }

  // Register PAMPL-visible status columns.
  if (const auto it = status_cols_.find(st_key);
      it != status_cols_.end() && !it->second.empty())
  {
    sc.add_ampl_variable(
        ampl_name, uid(), StatusName, scenario, stage, it->second);
  }

  return true;
}

bool LineCommitmentLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto pid = id();

  // ``status`` is a binary LP variable — emit via the integer-snapping
  // overload so the output Parquet carries exact 0/1 values instead
  // of a sub-percent tail of fractional reports.
  out.add_col_sol_integer(cname, StatusName, pid, status_cols_);
  out.add_col_cost(cname, StatusName, pid, status_cols_);

  out.add_row_dual(cname, CapacityPName, pid, capacity_p_rows_);
  out.add_row_dual(cname, CapacityNName, pid, capacity_n_rows_);

  return true;
}

}  // namespace gtopt
