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

#include <gtopt/cost_helper.hpp>
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
    // (scenario, stage, block).  Two very different cases have no
    // aggregated flow column:
    //   * LineLP elided the block entirely (``in_service = 0`` /
    //     inactive stage): no columns at all — the line is a true open
    //     circuit and the commitment binary has no role.  Skip.
    //   * ``piecewise_direct``: flow lives ONLY in per-direction
    //     segment columns (no fp/fn aggregator).  Gating those is not
    //     implemented, so the commitment would be silently IGNORED —
    //     the "open" line keeps carrying full segment flow (channel E,
    //     ``test_line_losses_commitment_leak.cpp``).  Warn loudly
    //     instead of silently discarding the operator's breaker
    //     instruction.
    const auto flow = resolve_flow_cols(line_lp, scenario, stage, buid);
    if (!flow.fp && !flow.fn && !flow.fs) {
      const auto& segp = line_lp.flowp_seg_cols_at(scenario, stage);
      const auto& segn = line_lp.flown_seg_cols_at(scenario, stage);
      if (segp.contains(buid) || segn.contains(buid)) {
        static bool warned_direct_ungated = false;
        if (!warned_direct_ungated) {
          spdlog::warn(
              "LineCommitment on a piecewise_direct line has NO effect: "
              "the mode has no flow aggregator to gate, so the "
              "commitment (breaker) status is ignored and the line "
              "keeps carrying segment flow while 'open'.  Use "
              "'piecewise'/'bidirectional'/'tangent_signed_flow' for "
              "committed lines.  See "
              "test_line_losses_commitment_leak.cpp.");
          warned_direct_ungated = true;
        }
      }
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
    //
    // (v1.1) When the u/v/w decomposition is active
    // (``startup_cost > 0 || shutdown_cost > 0``), ``initial_status``
    // has a DIFFERENT meaning: it is the breaker state at ``t = −1``
    // (pre-stage) used as the RHS of C1₀ to count a startup event at
    // ``t = 0`` when transitioning from open to closed.  Pinning
    // ``u[0]`` in that mode would over-constrain the LP (force the
    // breaker open at ``t = 0`` even when serving demand requires it
    // closed), so the pin is suppressed and the C1₀ row carries the
    // semantics instead.
    const bool uvw_active = lc.startup_cost.value_or(0.0) != 0.0
        || lc.shutdown_cost.value_or(0.0) != 0.0;
    if (!has_fixed && !is_must_run && !uvw_active
        && buid == blocks.front().uid() && lc.initial_status.has_value())
    {
      const double init = lc.initial_status.value();
      u_lowb = u_uppb = (init >= 0.5) ? 1.0 : 0.0;
    }

    // Create binary status variable u_l.  IntegerScope::Block matches
    // the per-block grouping used by SimpleCommitment.
    const auto u_domain =
        is_relax ? IntegerDomain::Relaxed : IntegerDomain::Binary;
    const std::array<BlockUid, 1> u_blocks {buid};
    auto ucol =
        sc.add_integer_col(lp,
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
                               // pin scale on all three commitment
                               // vars (status u + startup v + shutdown
                               // w), not just the integer one — u is
                               // semantically binary even when LP-relaxed
                               .pin_scale = true,
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
            : ((2.0 * theta_max) + std::abs(phi_rad));

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

  // ── (v1.1) u/v/w decomposition for line startup / shutdown costs ──
  //
  // Active iff ``LineCommitment.startup_cost`` or ``.shutdown_cost`` is
  // set.  Mirrors the CommitmentLP three-binary formulation (Knueven,
  // Ostrowski & Watson 2020 / Morales-España et al. 2013):
  //
  //   v_l[t]  startup indicator  (continuous-in-[0,1], implied binary)
  //   w_l[t]  shutdown indicator (continuous-in-[0,1], implied binary)
  //   C1:     u_l[t] − u_l[t−1] − v_l[t] + w_l[t] = 0       (t > 0)
  //   C1₀:    u_l[0] − v_l[0]            + w_l[0] = initial_status
  //   C3:     v_l[t] + w_l[t] ≤ 1
  //
  // Declaring only u_l integer (not v_l, w_l) is correct: the C1 logic
  // equality plus the C3 exclusion plus the non-negative startup /
  // shutdown costs force v_l, w_l to the integer up/down transition of
  // an integer u_l at every optimal vertex.  Cuts branching variables
  // by ~2/3 with the same feasible set.
  const auto startup_cost = lc.startup_cost.value_or(0.0);
  const auto shutdown_cost = lc.shutdown_cost.value_or(0.0);
  if (!ucols.empty() && (startup_cost != 0.0 || shutdown_cost != 0.0)) {
    BIndexHolder<ColIndex> vcols;
    BIndexHolder<ColIndex> wcols;
    BIndexHolder<RowIndex> lrows;
    BIndexHolder<RowIndex> erows;
    map_reserve(vcols, blocks.size());
    map_reserve(wcols, blocks.size());
    map_reserve(lrows, blocks.size());
    map_reserve(erows, blocks.size());

    // C1's first-block RHS: ``initial_status`` (0 or 1) if the user
    // pinned the breaker's pre-stage state; default to 0 (assume open
    // before the stage starts — matches CommitmentLP convention).
    double initial_u = 0.0;
    if (lc.initial_status.has_value()) {
      initial_u = (lc.initial_status.value() >= 0.5) ? 1.0 : 0.0;
    }

    // Per-event costs are FACE-VALUE (not multiplied by block
    // duration): startup is a one-time cost when the breaker
    // closes, not a per-hour cost.  Apply scenario probability +
    // stage discount via ``cost_factor`` only.  Mirrors
    // ``CommitmentLP::add_to_lp`` v/w cost wiring.  Hoisted out of
    // the block loop — both factors are (scenario, stage)-invariant.
    //
    // (v1.3) Startup tiers: when ``has_startup_tiers()`` is true the
    // tier columns y_hot / y_warm / y_cold carry the ENTIRE startup
    // cost, so ``v_cost`` is FORCED to zero here (otherwise the LP
    // would double-charge: v_cost on v[t] plus tier_cost on the
    // selected tier).  Mirrors CommitmentLP exactly.
    const bool tiers_active = lc.has_startup_tiers();
    const auto v_cost = (tiers_active ? 0.0 : startup_cost)
        * CostHelper::cost_factor(scenario.probability_factor(),
                                  stage.discount_factor());
    const auto w_cost = shutdown_cost
        * CostHelper::cost_factor(scenario.probability_factor(),
                                  stage.discount_factor());

    ColIndex prev_ucol {};
    bool first_block = true;

    for (const auto& block : blocks) {
      const auto buid = block.uid();
      const auto u_it = ucols.find(buid);
      if (u_it == ucols.end()) {
        // No u_col for this block (LineLP elided it — outage).  Skip
        // the v/w/C1/C3 emission too: an open-circuit block has no
        // switching event.
        //
        // BEHAVIOR ON INTRA-STAGE OUTAGE GAPS: if blocks
        // [t_0, t_a, …, t_b, t_c] skip [t_a..t_b] (outage), the next
        // emitted C1 row links ``u[t_c] − u[t_0]``, treating the gap
        // as if it didn't happen.  This is intentional: the breaker
        // state survives the outage from the LP's perspective — the
        // outage just removes the flow capability, not the commitment.
        // An alternative semantics (reset to initial_status at t_c)
        // would require an outage flag on the schedule; deferred.
        continue;
      }
      const auto ucol = u_it->second;
      const auto ctx = make_block_context(scenario.uid(), stage.uid(), buid);

      // Continuous-in-[0,1] v_l with ``pin_scale = true`` (the C1
      // equality references its coefficient literally; we don't want
      // VariableScaleMap to silently rescale it).
      const auto vcol = lp.add_col(SparseCol {
          .lowb = 0.0,
          .uppb = 1.0,
          .cost = v_cost,
          .is_integer = false,
          .pin_scale = true,
          .class_name = cname,
          .variable_name = StartupName,
          .variable_uid = cuid,
          .context = ctx,
      });
      vcols[buid] = vcol;

      const auto wcol = lp.add_col(SparseCol {
          .lowb = 0.0,
          .uppb = 1.0,
          .cost = w_cost,
          .is_integer = false,
          .pin_scale = true,
          .class_name = cname,
          .variable_name = ShutdownName,
          .variable_uid = cuid,
          .context = ctx,
      });
      wcols[buid] = wcol;

      // C1: u[t] − u[t−1] − v[t] + w[t] = 0
      //   t = 0:  u[0] − v[0] + w[0] = initial_u
      {
        SparseRow row {
            .class_name = cname,
            .constraint_name = LogicName,
            .variable_uid = cuid,
            .context = ctx,
        };
        row.equal(first_block ? initial_u : 0.0);
        row[ucol] = 1.0;
        if (!first_block) {
          row[prev_ucol] = -1.0;
        }
        row[vcol] = -1.0;
        row[wcol] = 1.0;
        lrows[buid] = lp.add_row(std::move(row));
      }

      // C3: v[t] + w[t] ≤ 1
      {
        SparseRow row {
            .class_name = cname,
            .constraint_name = ExclusionName,
            .variable_uid = cuid,
            .context = ctx,
        };
        row.less_equal(1.0);
        row[vcol] = 1.0;
        row[wcol] = 1.0;
        erows[buid] = lp.add_row(std::move(row));
      }

      prev_ucol = ucol;
      first_block = false;
    }

    if (!vcols.empty()) {
      startup_cols_[st_key] = std::move(vcols);
    }
    if (!wcols.empty()) {
      shutdown_cols_[st_key] = std::move(wcols);
    }
    if (!lrows.empty()) {
      logic_rows_[st_key] = std::move(lrows);
    }
    if (!erows.empty()) {
      exclusion_rows_[st_key] = std::move(erows);
    }

    // ── (v1.2) Min up time ──────────────────────────────────────────
    // Σ_{q=t..t+UT-1} u[q] ≥ UT · v[t].
    // UT (in blocks) is the smallest k s.t. Σ duration[t..t+k-1] ≥ min_up_time.
    // Trivially satisfied when UT ≤ 1 (skip the row).  Mirrors
    // ``CommitmentLP`` C6.
    const double min_up_hours = lc.min_up_time.value_or(0.0);
    const auto& v_holder = startup_cols_.find(st_key);
    const bool have_v_cols =
        (v_holder != startup_cols_.end()) && !v_holder->second.empty();
    if (min_up_hours > 0.0 && have_v_cols) {
      BIndexHolder<RowIndex> mut_rows;
      map_reserve(mut_rows, blocks.size());
      for (size_t t = 0; t < blocks.size(); ++t) {
        const auto buid_t = blocks[t].uid();
        const auto v_it = v_holder->second.find(buid_t);
        if (v_it == v_holder->second.end()) {
          continue;
        }
        // Forward-walk to find UT (in blocks) covering min_up_hours.
        double acc = 0.0;
        size_t ut = 0;
        for (size_t q = t; q < blocks.size() && acc < min_up_hours; ++q) {
          acc += blocks[q].duration();
          ++ut;
        }
        if (ut <= 1) {
          continue;
        }
        SparseRow row {
            .class_name = cname,
            .constraint_name = MinUpTimeName,
            .variable_uid = cuid,
            .context = make_block_context(scenario.uid(), stage.uid(), buid_t),
        };
        row.greater_equal(0.0);
        for (size_t q = t; q < t + ut && q < blocks.size(); ++q) {
          const auto u_it = ucols.find(blocks[q].uid());
          if (u_it != ucols.end()) {
            row[u_it->second] = 1.0;
          }
        }
        row[v_it->second] = -static_cast<double>(ut);
        mut_rows[buid_t] = lp.add_row(std::move(row));
      }
      if (!mut_rows.empty()) {
        min_up_time_rows_[st_key] = std::move(mut_rows);
      }
    }

    // ── (v1.2) Min down time ────────────────────────────────────────
    // Σ_{q=t..t+DT-1} u[q] + DT · w[t] ≤ span.
    // Derived from Σ (1 − u[q]) ≥ DT · w[t] over the DT-block window.
    // Mirrors ``CommitmentLP`` C7.
    const double min_down_hours = lc.min_down_time.value_or(0.0);
    const auto& w_holder = shutdown_cols_.find(st_key);
    const bool have_w_cols =
        (w_holder != shutdown_cols_.end()) && !w_holder->second.empty();
    if (min_down_hours > 0.0 && have_w_cols) {
      BIndexHolder<RowIndex> mdt_rows;
      map_reserve(mdt_rows, blocks.size());
      for (size_t t = 0; t < blocks.size(); ++t) {
        const auto buid_t = blocks[t].uid();
        const auto w_it = w_holder->second.find(buid_t);
        if (w_it == w_holder->second.end()) {
          continue;
        }
        double acc = 0.0;
        size_t dt = 0;
        for (size_t q = t; q < blocks.size() && acc < min_down_hours; ++q) {
          acc += blocks[q].duration();
          ++dt;
        }
        if (dt <= 1) {
          continue;
        }
        const auto span = std::min(t + dt, blocks.size()) - t;
        SparseRow row {
            .class_name = cname,
            .constraint_name = MinDownTimeName,
            .variable_uid = cuid,
            .context = make_block_context(scenario.uid(), stage.uid(), buid_t),
        };
        row.less_equal(static_cast<double>(span));
        for (size_t q = t; q < t + dt && q < blocks.size(); ++q) {
          const auto u_it = ucols.find(blocks[q].uid());
          if (u_it != ucols.end()) {
            row[u_it->second] = 1.0;
          }
        }
        row[w_it->second] = static_cast<double>(dt);
        mdt_rows[buid_t] = lp.add_row(std::move(row));
      }
      if (!mdt_rows.empty()) {
        min_down_time_rows_[st_key] = std::move(mdt_rows);
      }
    }

    // ── (v1.2) max_starts / min_starts rolling-window cap ───────────
    // Two-sided bound:
    //   min_starts ≤ Σ_{t ∈ window} v[t] ≤ max_starts
    // Window length resolved from ``starts_scope`` via
    // ``starts_window_hours()``: 0 (or unset) ⇒ horizon (one row per
    // stage); positive N ⇒ flush when accumulated block duration ≥ N.
    // Mirrors ``CommitmentLP`` C9.
    const bool has_max_starts = lc.max_starts.has_value();
    const bool has_min_starts = lc.min_starts.has_value();
    if ((has_max_starts || has_min_starts) && have_v_cols) {
      const double window_hours = lc.starts_window_hours();
      const auto max_starts_v =
          has_max_starts ? static_cast<double>(*lc.max_starts) : 0.0;
      const auto min_starts_v =
          has_min_starts ? static_cast<double>(*lc.min_starts) : 0.0;

      BIndexHolder<RowIndex> ms_rows;
      auto fresh_row = [&]() -> SparseRow
      {
        SparseRow r;
        r.class_name = cname;
        r.constraint_name = MaxStartsName;
        r.variable_uid = cuid;
        return r;
      };
      SparseRow window_row = fresh_row();
      bool row_open = false;
      double acc_hours = 0.0;
      BlockUid window_end_block {};

      auto flush_window = [&]()
      {
        if (!row_open) {
          return;
        }
        if (has_max_starts) {
          SparseRow upper {window_row};
          auto bound = std::move(upper).less_equal(max_starts_v);
          const auto idx = lp.add_row(std::move(bound));
          ms_rows[window_end_block] = idx;
        }
        // LP-size: ``min_starts == 0`` makes the lower row ``Σ v ≥ 0``,
        // trivially satisfied (v ≥ 0) and never binding — skip it.  The
        // row index is never stored, so no dual output is lost.
        if (has_min_starts && min_starts_v > 0.0) {
          SparseRow lower {window_row};
          // Distinct constraint_name so the row metadata dedup in
          // LinearProblem::add_row doesn't collide with the upper
          // row (both share the same context).
          lower.constraint_name = MinStartsName;
          auto bound = std::move(lower).greater_equal(min_starts_v);
          [[maybe_unused]] const auto lower_idx = lp.add_row(std::move(bound));
        }
        window_row = fresh_row();
        row_open = false;
        acc_hours = 0.0;
      };

      for (const auto& block : blocks) {
        const auto buid_b = block.uid();
        const auto v_it = v_holder->second.find(buid_b);
        if (v_it == v_holder->second.end()) {
          continue;
        }
        if (!row_open) {
          window_row.context =
              make_block_context(scenario.uid(), stage.uid(), buid_b);
          row_open = true;
        }
        window_row[v_it->second] = 1.0;
        acc_hours += block.duration();
        window_end_block = buid_b;
        if (window_hours > 0.0 && acc_hours >= window_hours) {
          flush_window();
        }
      }
      flush_window();
      if (!ms_rows.empty()) {
        max_starts_rows_[st_key] = std::move(ms_rows);
      }
    }

    // ── (v1.3) Startup-cost tiers (hot/warm/cold) ────────────────────
    // Mirrors CommitmentLP C8/C9/C10.  Skipped if any tier field is
    // missing or if cold_start_time < hot_start_time (defensive — also
    // caught by validate_planning if/when added).
    if (tiers_active && have_v_cols && have_w_cols) {
      const auto hot_cost = *lc.hot_start_cost;
      const auto warm_cost = *lc.warm_start_cost;
      const auto cold_cost = *lc.cold_start_cost;
      const auto hot_time = *lc.hot_start_time;
      const auto cold_time = *lc.cold_start_time;
      if (cold_time < hot_time) {
        spdlog::warn(
            "LineCommitment '{}': cold_start_time ({}) < hot_start_time "
            "({}), skipping startup tiers",
            lc.name,
            cold_time,
            hot_time);
      } else {
        // Pre-stage offline hours used to size t=0's hot/warm windows.
        // Default 1e6 hours when unset ⇒ always cold-start at t=0.
        const auto initial_offline =
            (initial_u < 0.5) ? lc.initial_hours.value_or(1e6) : 0.0;

        const auto tier_factor = CostHelper::cost_factor(
            scenario.probability_factor(), stage.discount_factor());
        const auto h_cost = hot_cost * tier_factor;
        const auto wm_cost = warm_cost * tier_factor;
        const auto c_cost = cold_cost * tier_factor;

        BIndexHolder<ColIndex> hcols;
        BIndexHolder<ColIndex> wmcols;
        BIndexHolder<ColIndex> ccols;
        BIndexHolder<RowIndex> st_rows;
        BIndexHolder<RowIndex> hr_rows;
        BIndexHolder<RowIndex> wr_rows;
        map_reserve(hcols, blocks.size());
        map_reserve(wmcols, blocks.size());
        map_reserve(ccols, blocks.size());
        map_reserve(st_rows, blocks.size());
        map_reserve(hr_rows, blocks.size());
        map_reserve(wr_rows, blocks.size());

        for (size_t t = 0; t < blocks.size(); ++t) {
          const auto buid_t = blocks[t].uid();
          const auto v_it = v_holder->second.find(buid_t);
          if (v_it == v_holder->second.end()) {
            continue;
          }
          const auto vcol = v_it->second;
          const auto ctx_t =
              make_block_context(scenario.uid(), stage.uid(), buid_t);

          // Tier indicator columns — continuous in [0, 1], implied
          // binary at the optimum thanks to C8 + nonnegative costs.
          const auto hcol = lp.add_col(SparseCol {
              .lowb = 0.0,
              .uppb = 1.0,
              .cost = h_cost,
              .is_integer = false,
              .pin_scale = true,
              .class_name = cname,
              .variable_name = HotStartName,
              .variable_uid = cuid,
              .context = ctx_t,
          });
          hcols[buid_t] = hcol;
          const auto wmcol = lp.add_col(SparseCol {
              .lowb = 0.0,
              .uppb = 1.0,
              .cost = wm_cost,
              .is_integer = false,
              .pin_scale = true,
              .class_name = cname,
              .variable_name = WarmStartName,
              .variable_uid = cuid,
              .context = ctx_t,
          });
          wmcols[buid_t] = wmcol;
          const auto ccol = lp.add_col(SparseCol {
              .lowb = 0.0,
              .uppb = 1.0,
              .cost = c_cost,
              .is_integer = false,
              .pin_scale = true,
              .class_name = cname,
              .variable_name = ColdStartName,
              .variable_uid = cuid,
              .context = ctx_t,
          });
          ccols[buid_t] = ccol;

          // C8: v[t] = y_hot[t] + y_warm[t] + y_cold[t]
          {
            SparseRow row {
                .class_name = cname,
                .constraint_name = TierSelectName,
                .variable_uid = cuid,
                .context = ctx_t,
            };
            row.equal(0.0);
            row[vcol] = 1.0;
            row[hcol] = -1.0;
            row[wmcol] = -1.0;
            row[ccol] = -1.0;
            st_rows[buid_t] = lp.add_row(std::move(row));
          }

          // C9 / C10: pre-block offline-hour windows.  Walking
          // BACKWARDS from t we accumulate block durations to identify
          // which past blocks' ``w[q]`` count as in-window.
          //
          //   * hot window:  blocks whose duration sum (from q+1 to
          //     t-1) is ≤ hot_time.
          //   * warm window: blocks whose duration sum is in
          //     (hot_time, cold_time].
          //
          // For ``t = 0``, the past offline duration is the user-
          // supplied ``initial_offline``; the LP constant ``1`` slack
          // on the RHS lets the LP pick the appropriate tier for the
          // first startup.
          SparseRow hot_row {
              .class_name = cname,
              .constraint_name = HotWindowName,
              .variable_uid = cuid,
              .context = ctx_t,
          };
          hot_row.less_equal(0.0);
          hot_row[hcol] = 1.0;
          SparseRow warm_row {
              .class_name = cname,
              .constraint_name = WarmWindowName,
              .variable_uid = cuid,
              .context = ctx_t,
          };
          warm_row.less_equal(0.0);
          warm_row[wmcol] = 1.0;

          // Accumulate prior-block durations (walking back).  Past
          // ``w[q]`` for which the cumulative offline window covers
          // [0, hot_time] feeds hot_row; the slice (hot_time,
          // cold_time] feeds warm_row.
          double back = 0.0;
          for (size_t q = t; q > 0; --q) {
            const auto buid_q = blocks[q - 1].uid();
            const auto w_q_it = w_holder->second.find(buid_q);
            if (w_q_it == w_holder->second.end()) {
              continue;
            }
            const auto wcol_q = w_q_it->second;
            back += blocks[q - 1].duration();
            if (back <= hot_time) {
              hot_row[wcol_q] = -1.0;
            }
            if (back <= cold_time) {
              warm_row[wcol_q] = -1.0;
            }
            if (back > cold_time) {
              break;  // further-back blocks are in cold window only
            }
          }
          // First-block constant offset via initial_offline:
          //   initial_offline ≤ hot_time   ⇒ hot allowed at t (RHS ≥ 1
          //                                   for y_hot[0]).
          //   initial_offline ≤ cold_time  ⇒ warm allowed at t.
          // We encode this as a relaxation of the RHS only at t = 0
          // — the C8 row plus the LP's preference for the cheapest
          // tier ensures cold is the residual fallback otherwise.
          if (t == 0) {
            if (initial_offline <= hot_time) {
              hot_row.less_equal(1.0);
            }
            if (initial_offline <= cold_time) {
              warm_row.less_equal(1.0);
            }
          }
          hr_rows[buid_t] = lp.add_row(std::move(hot_row));
          wr_rows[buid_t] = lp.add_row(std::move(warm_row));
        }

        if (!hcols.empty()) {
          hot_start_cols_[st_key] = std::move(hcols);
        }
        if (!wmcols.empty()) {
          warm_start_cols_[st_key] = std::move(wmcols);
        }
        if (!ccols.empty()) {
          cold_start_cols_[st_key] = std::move(ccols);
        }
        if (!st_rows.empty()) {
          tier_select_rows_[st_key] = std::move(st_rows);
        }
        if (!hr_rows.empty()) {
          hot_window_rows_[st_key] = std::move(hr_rows);
        }
        if (!wr_rows.empty()) {
          warm_window_rows_[st_key] = std::move(wr_rows);
        }
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

  // u/v/w decomposition outputs (v1.1).  All empty when startup_cost
  // and shutdown_cost are both zero — ``add_col_sol_*`` / ``add_row_dual``
  // are no-ops on empty STBIndexHolders.
  out.add_col_sol_integer(cname, StartupName, pid, startup_cols_);
  out.add_col_cost(cname, StartupName, pid, startup_cols_);
  out.add_col_sol_integer(cname, ShutdownName, pid, shutdown_cols_);
  out.add_col_cost(cname, ShutdownName, pid, shutdown_cols_);
  out.add_row_dual(cname, LogicName, pid, logic_rows_);
  out.add_row_dual(cname, ExclusionName, pid, exclusion_rows_);

  // v1.2 time-based row duals (empty when the corresponding schema
  // field is unset; ``add_row_dual`` no-ops on empty holders).
  out.add_row_dual(cname, MinUpTimeName, pid, min_up_time_rows_);
  out.add_row_dual(cname, MinDownTimeName, pid, min_down_time_rows_);
  out.add_row_dual(cname, MaxStartsName, pid, max_starts_rows_);

  // v1.3 startup-tier outputs.  All empty when ``has_startup_tiers()``
  // is false on the schema.
  out.add_col_sol_integer(cname, HotStartName, pid, hot_start_cols_);
  out.add_col_cost(cname, HotStartName, pid, hot_start_cols_);
  out.add_col_sol_integer(cname, WarmStartName, pid, warm_start_cols_);
  out.add_col_cost(cname, WarmStartName, pid, warm_start_cols_);
  out.add_col_sol_integer(cname, ColdStartName, pid, cold_start_cols_);
  out.add_col_cost(cname, ColdStartName, pid, cold_start_cols_);
  out.add_row_dual(cname, TierSelectName, pid, tier_select_rows_);
  out.add_row_dual(cname, HotWindowName, pid, hot_window_rows_);
  out.add_row_dual(cname, WarmWindowName, pid, warm_window_rows_);

  return true;
}

}  // namespace gtopt
