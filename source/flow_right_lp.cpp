/**
 * @file      flow_right_lp.cpp
 * @brief     Implementation of FlowRightLP methods
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Unified design:
 *   Each FlowRight installs a single LP column `flow_b` per
 *   (scenario, stage, block), bounded by the resolved `[fmin_b, fmax_b]`
 *   band.  When the `target` schedule is resolved and at least one of
 *   `fcost` / `uvalue` is active, the kink at `target` is expressed via
 *   two non-negative slacks and one equality row:
 *
 *       flow_b − excess_b + fail_b = target_b      (`flow_kink` row)
 *       excess_b ≥ 0,  cost(excess_b) = -uvalue·cf
 *       fail_b   ≥ 0,  cost(fail_b)   = +fcost·cf
 *
 *   When `use_average = true`, the kink machinery is applied once at
 *   stage scope to a single `qeh` column instead of per-block (with
 *   stage-aggregate `target_e = Σ_b dur_ratio_b × target_b` and the
 *   stage-level `scenario_stage_ecost` cost helper).
 *
 *   The helper `attach_flow` is the single point of truth: it creates
 *   the primary column (`[fmin, fmax]`, zero cost) and, when a target
 *   is supplied with at least one non-zero slack cost, installs the
 *   kink slacks and equality row.  It is used identically per-block
 *   and once-per-stage on qeh.
 *
 * The flow right is consumptive: the per-block `flow_b` is subtracted
 * (coefficient -1) from the upstream Junction balance row when
 * `flow_right().junction_a` is set.
 */

#include <gtopt/cost_helper.hpp>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// LP time-resolution mode for a FlowRight, resolved from the
/// `flow_mode` JSON field (with `use_average` as legacy alias).
enum class FlowMode : std::uint8_t
{
  per_block,  ///< Per-block flow column, per-block kink.
  stage_average,  ///< Per-block flow column + stage qeh column; kink on qeh.
  stage_uniform,  ///< Single stage qeh column, used in every block balance.
};

/// Resolve the FlowMode from the JSON fields.  Explicit `flow_mode`
/// always wins; falls back to the legacy `use_average` boolean
/// (`true` → `stage_average`, `false` → `per_block`); unknown strings
/// log a warning and fall back to the legacy path.
[[nodiscard]] FlowMode resolve_flow_mode(const FlowRight& fr)
{
  if (fr.flow_mode.has_value()) {
    const auto& m = *fr.flow_mode;
    if (m == "per_block") {
      return FlowMode::per_block;
    }
    if (m == "stage_average") {
      return FlowMode::stage_average;
    }
    if (m == "stage_uniform") {
      return FlowMode::stage_uniform;
    }
    SPDLOG_WARN(
        "FlowRight uid={}: unknown flow_mode '{}'; falling back "
        "to use_average / per_block",
        fr.uid,
        m);
  }
  return fr.use_average.value_or(false) ? FlowMode::stage_average
                                        : FlowMode::per_block;
}

struct ResolvedBounds
{
  Real fmin {0.0};
  std::optional<Real> target {};
  Real fmax {0.0};
};

/// Resolve the `(fmin, target, fmax)` triple for one scope (a block or
/// a stage).  Defaults:
///   - `fmin` defaults to 0.  When `target` is set and no slack cost is
///     active (neither fcost nor uvalue), `fmin` defaults to `target`
///     so the column collapses to a forced-exact `[target, target]`.
///   - `fmax` defaults to `target` when set, else `fmin`.
///   - The `rule_cap` parameter clamps fmax (and pulls target / fmin
///     down with it when they exceed the cap).  Pass `LinearProblem::
///     DblMax` to skip the cap.
///
/// Also applies the back-compat reset: a literal `target == 0` with no
/// slack cost on either side (neither fcost nor uvalue) is
/// reinterpreted as "no soft requirement".  A zero target with an
/// active `uvalue` is kept: `target = 0` + negative `uvalue` is the
/// canonical encoding of a plain per-unit usage cost on the whole
/// flow (PLP's CQVar coefficients on the Laja/Maule rights flows),
/// and `target = 0` + positive `uvalue` a per-unit reward (PLP's
/// ValorRiego on delivered irrigation).
ResolvedBounds resolve_bounds(std::optional<Real> raw_fmin,
                              std::optional<Real> raw_target,
                              std::optional<Real> raw_fmax,
                              Real rule_cap,
                              bool fcost_active,
                              bool uvalue_active)
{
  std::optional<Real> target = raw_target;
  if (target.has_value() && *target == 0.0 && !fcost_active && !uvalue_active) {
    target.reset();
  }
  const bool any_slack_cost = fcost_active || uvalue_active;
  const Real fmin_default =
      (target.has_value() && !any_slack_cost) ? *target : 0.0;
  Real fmin = raw_fmin.value_or(fmin_default);
  Real fmax = raw_fmax.value_or(target.value_or(fmin));

  fmax = std::min(fmax, rule_cap);
  if (target.has_value()) {
    target = std::min(*target, fmax);
  }
  fmin = std::min(fmin, fmax);
  if (target.has_value()) {
    fmin = std::min(fmin, *target);
  }
  return ResolvedBounds {
      .fmin = fmin,
      .target = target,
      .fmax = fmax,
  };
}

/// Naming bundle for a flow point's LP entities.  Lets `attach_flow`
/// be invoked identically per-block (`{flow, excess, fail, flow_kink}`)
/// and per-stage (`{qeh, qeh_sp, qeh_sn, qkink}`).
struct FlowNames
{
  std::string_view class_name;
  std::string_view flow_name;
  std::string_view sp_name;
  std::string_view sn_name;
  std::string_view kink_name;
};

/// Output of `attach_flow`.  Kink slacks/row are present iff the call
/// passed a target AND BOTH slack costs were non-zero (full-kink path).
/// One-sided substitution (fcost-only or uvalue-only) folds the slack
/// + row into the primary flow column and reports the elision via
/// `substituted_target` + `substituted_fcost_only` so the caller can
/// cache the target for downstream reconstruction.
struct FlowAttachment
{
  ColIndex flow_col;
  std::optional<ColIndex> excess_col;
  std::optional<ColIndex> fail_col;
  std::optional<RowIndex> kink_row;
  /// Resolved target value used during the one-sided substitution.
  /// `nullopt` when the full-kink path fired or no target was given.
  std::optional<Real> substituted_target;
  /// True iff fcost-only substitution fired (caller reconstructs
  /// `fail = max(0, target − flow)`).  False iff uvalue-only
  /// substitution fired (caller reconstructs
  /// `excess = max(0, flow − target)`).
  bool substituted_fcost_only {false};
};

/// Single point of truth for installing one "flow point" into the LP.
/// Used identically in three call sites:
///
///   * per-block, non-avg mode (`fcol = flow_b`, target = target_b,
///     costs from `CostHelper::block_ecost`),
///   * per-block, use_average mode (`fcol = flow_b`, target = nullopt,
///     no kink installed at the block scope),
///   * once-per-stage, use_average mode (`fcol = qeh`, target = target_e,
///     costs from `CostHelper::scenario_stage_ecost`).
///
/// Creates the primary column with `[fmin, fmax]` bounds and zero cost.
/// If `target` is set AND at least one of `sp_cost_cf` / `sn_cost_cf`
/// is non-zero, installs the kink:
///
///     flow_col − excess_col + fail_col = target
///     excess_col ≥ 0,  cost = sp_cost_cf   (negative ⇒ bonus above target)
///     fail_col   ≥ 0,  cost = sn_cost_cf   (positive ⇒ penalty below target)
///
/// `fmax` may be `LinearProblem::DblMax` (unbounded above).
template<typename Context>
[[nodiscard]] FlowAttachment attach_flow(LinearProblem& lp,
                                         Real fmin,
                                         Real fmax,
                                         std::optional<Real> target,
                                         Real sp_cost_cf,
                                         Real sn_cost_cf,
                                         Uid variable_uid,
                                         FlowNames names,
                                         Context ctx)
{
  // One-sided kink elision (same pattern as DemandLP's failure
  // substitution): when only sn_cost is active (fcost-only soft
  // floor), `fail = target − flow` is the slack; substituting it
  // into the objective folds the slack col + kink row into the
  // primary flow column with cost `−... +obj_constant` baked in.
  // Mirror for sp_cost-only (uvalue-only soft cap).
  //
  // Algebra (fcost-only):
  //   row    : flow + fail = target,  fail ≥ 0,  cost(fail) = sn
  //   substitute fail = target − flow:
  //     cost(fail) × fail = sn × (target − flow)
  //                       = sn × target − sn × flow
  //                         \________/   \_______/
  //                         constant    coef on flow
  //   flow.uppb shrinks to min(fmax, target) since fail ≥ 0 forces
  //   flow ≤ target; the inequality is now expressed as a column
  //   bound rather than as a row.  Saves 1 col + 1 row per scope.
  //
  // The full-kink case (both sp and sn active) still emits the
  // explicit slacks + equality row — the two-sided substitution
  // would conflict and require both bounds to align with target.
  Real flow_lowb = fmin;
  Real flow_uppb = fmax;
  Real flow_cost = 0.0;
  const bool fcost_only =
      target.has_value() && sn_cost_cf != 0.0 && sp_cost_cf == 0.0;
  const bool uvalue_only =
      target.has_value() && sp_cost_cf != 0.0 && sn_cost_cf == 0.0;
  std::optional<Real> substituted_target;
  bool substituted_fcost_only = false;
  if (fcost_only) {
    flow_uppb = std::min(fmax, *target);
    flow_cost = -sn_cost_cf;  // see derivation above
    lp.add_obj_constant(sn_cost_cf * *target);
    substituted_target = target;
    substituted_fcost_only = true;
  } else if (uvalue_only) {
    // Symmetric mirror: excess = flow − target.
    //   cost(excess) × excess = sp × (flow − target)
    //                         = sp × flow − sp × target
    flow_lowb = std::max(fmin, *target);
    flow_cost = sp_cost_cf;  // sp_cost_cf is negative (bonus)
    lp.add_obj_constant(-sp_cost_cf * *target);
    substituted_target = target;
    substituted_fcost_only = false;
  }
  const auto flow_col = lp.add_col(SparseCol {
      .lowb = flow_lowb,
      .uppb = flow_uppb,
      .cost = flow_cost,
      .class_name = names.class_name,
      .variable_name = names.flow_name,
      .variable_uid = variable_uid,
      .context = ctx,
  });
  FlowAttachment out {
      .flow_col = flow_col,
      .excess_col = std::nullopt,
      .fail_col = std::nullopt,
      .kink_row = std::nullopt,
      .substituted_target = substituted_target,
      .substituted_fcost_only = substituted_fcost_only,
  };

  // Full-kink path (both sp_cost and sn_cost active): explicit
  // slack pair + equality row.  Skipped when the one-sided
  // substitution above folded the kink into the primary column.
  if (target.has_value() && sp_cost_cf != 0.0 && sn_cost_cf != 0.0) {
    // Cap each kink slack at its physically reachable maximum.  The kink
    // `flow − excess + fail = target` only models excess = max(0, flow −
    // target) and fail = max(0, target − flow) when at most one slack is
    // nonzero.  With BOTH slacks unbounded above, the solver can inflate
    // excess and fail together (flow fixed: Δexcess = Δfail) for a net
    // per-unit cost of `sp_cost_cf + sn_cost_cf = (fcost − uvalue)·cf`,
    // which is negative — an UNBOUNDED ray — whenever the excess reward
    // `uvalue` exceeds the shortfall penalty `fcost` (a real PLP datum:
    // e.g. maule_gasto_normal_riego ships uvalue=1100 > fcost=1000).
    // Since flow ∈ [fmin, fmax], excess ≤ fmax − target and fail ≤ target
    // − fmin; bounding the slacks at those caps removes the spurious ray
    // while leaving the true kink optimum untouched.  When fmax is +∞ the
    // flow itself is unbounded (a separate modelling choice), so the
    // excess cap stays +∞ too.
    const Real excess_uppb = is_infinity(fmax, lp.infinity())
        ? LinearProblem::DblMax
        : std::max(0.0, fmax - *target);
    const Real fail_uppb = std::max(0.0, *target - fmin);
    auto excess_col = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = excess_uppb,
        .cost = sp_cost_cf,
        .class_name = names.class_name,
        .variable_name = names.sp_name,
        .variable_uid = variable_uid,
        .context = ctx,
    });
    auto fail_col = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = fail_uppb,
        .cost = sn_cost_cf,
        .class_name = names.class_name,
        .variable_name = names.sn_name,
        .variable_uid = variable_uid,
        .context = ctx,
    });
    auto row =
        SparseRow {
            .class_name = names.class_name,
            .constraint_name = names.kink_name,
            .variable_uid = variable_uid,
            .context = ctx,
        }
            .equal(*target);
    row[flow_col] = 1.0;
    row[excess_col] = -1.0;
    row[fail_col] = 1.0;
    out.excess_col = excess_col;
    out.fail_col = fail_col;
    out.kink_row = lp.add_row(std::move(row));
  }
  return out;
}

}  // namespace

FlowRightLP::FlowRightLP(const FlowRight& pflow, const InputContext& ic)
    : ObjectLP<FlowRight>(pflow, ic, Element::class_name)
    , fmin_sched(ic, Element::class_name, id(), std::move(flow_right().fmin))
    , fmax_sched(ic, Element::class_name, id(), std::move(flow_right().fmax))
    , target_sched(
          ic, Element::class_name, id(), std::move(flow_right().target))
    , direction(flow_right().direction.value_or(-1))
    , fcost(ic, Element::class_name, id(), std::move(flow_right().fcost))
    , uvalue_sched(
          ic, Element::class_name, id(), std::move(flow_right().uvalue))
{
}

bool FlowRightLP::add_to_lp(const SystemContext& sc,
                            const ScenarioLP& scenario,
                            const StageLP& stage,
                            LinearProblem& lp)
{
  static constexpr auto ampl_name = Element::class_name.snake_case();
  static constexpr auto cname = Element::class_name.full_name();

  if (!is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();
  const auto& options = sc.options();

  BIndexHolder<ColIndex> fcols;
  BIndexHolder<ColIndex> bfail_cols;
  BIndexHolder<ColIndex> bexcess_cols;
  BIndexHolder<RowIndex> bkink_rows;
  // Per-block one-sided-substitution cache (see
  // `block_target_values_` doc in `flow_right_lp.hpp`).  One entry
  // per block where attach_flow elided the explicit fail/excess
  // slack into the primary flow column.
  BIndexHolder<double> btarget_values;
  BIndexHolder<std::uint8_t> bfcost_only;
  map_reserve(fcols, blocks.size());

  // Evaluate initial bound rule if present.
  const auto& opt_rule = flow_right().bound_rule;
  auto initial_rule_bound = LinearProblem::DblMax;
  if (opt_rule.has_value()) {
    const auto axis_value = resolve_bound_rule_axis_value(
        *opt_rule,
        stage.month(),
        [&]() -> Real
        {
          if (!axis_uses_reservoir(opt_rule->axis)) {
            return 0.0;
          }
          const auto rsv_sid = ReservoirLPSId(opt_rule->reservoir);
          const auto& rsv = sc.element<ReservoirLP>(rsv_sid);
          return rsv.reservoir().eini.value_or(0.0);
        });
    initial_rule_bound = evaluate_bound_rule(*opt_rule, axis_value);
  }

  // `fcost` / `uvalue` are now per-(stage, block).  Both fields are
  // expressed in raw $/(m³/s·h) so that `CostHelper::*_ecost` (which
  // multiplies by block / stage duration) produces $/m³ × m³ totals
  // consistent with `Demand::fcost`.  Falls back to the global
  // hydro_spill_cost / hydro_use_value ($/m³, ×3600 to share units).
  //
  // Activity flag treats any non-zero cell / global fallback as
  // "active"; a literal `0.0` global (e.g. test fixtures explicitly
  // zeroing `hydro_spill_cost`) leaves the kink machinery dormant —
  // preserving the legacy `stage_fcost_val > 0.0` decision boundary.
  const auto global_fc_opt = options.hydro_spill_cost();
  const bool global_fc_nonzero =
      global_fc_opt.has_value() && global_fc_opt.value() != 0.0;
  const bool fcost_active = fcost.has_value() || global_fc_nonzero;

  const auto global_uv_opt = options.hydro_use_value();
  const bool uvalue_active = uvalue_sched.has_value()
      || (global_uv_opt.has_value() && global_uv_opt.value() != 0.0);

  // Per-block fcost / uvalue resolver.  Mirrors the per-stage logic
  // (×3600 lift for the global-fallback case, raw value for explicit
  // schedule cells) on a per-block grain.  A literal `0.0` global
  // produces `0.0` (the `*3600.0` lift is a no-op then anyway).
  const auto block_fcost_at = [&](const BlockLP& b) -> Real
  {
    const auto fc_cell = fcost.optval(stage.uid(), b.uid());
    if (fc_cell.has_value()) {
      return *fc_cell;
    }
    return global_fc_nonzero ? *global_fc_opt * 3600.0 : 0.0;
  };
  const auto block_uvalue_at = [&](const BlockLP& b) -> Real
  {
    const auto uv_cell = uvalue_sched.optval(stage.uid(), b.uid());
    if (uv_cell.has_value()) {
      return *uv_cell;
    }
    return (global_uv_opt.has_value() && global_uv_opt.value() != 0.0)
        ? *global_uv_opt * 3600.0
        : 0.0;
  };

  // Resolve the time-resolution mode (see `FlowMode` doc).
  const auto mode = resolve_flow_mode(flow_right());
  const bool emit_block_flow = (mode != FlowMode::stage_uniform);
  const bool emit_stage_qeh = (mode != FlowMode::per_block);
  const bool block_kink_active = (mode == FlowMode::per_block);

  // Stage-aggregate accumulators consumed by the stage-scope
  // `attach_flow` call (used by both stage_average and stage_uniform).
  Real fmin_e = 0.0;
  Real fmax_e = 0.0;
  Real target_e = 0.0;
  bool has_target_e = false;
  // Duration-weighted mean of fcost / uvalue across the stage's
  // blocks; consumed by stage-scope `attach_flow` for stage_average
  // and stage_uniform modes (mirrors the rb.fmin / rb.target volume-
  // weighted aggregation already in place).
  Real fcost_e = 0.0;
  Real uvalue_e = 0.0;

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    const auto raw_fmin_block = fmin_sched.at(stage.uid(), buid);
    const auto raw_fmax_block = fmax_sched.at(stage.uid(), buid);
    const auto raw_target_block = target_sched.at(stage.uid(), buid);

    const auto rb = resolve_bounds(raw_fmin_block,
                                   raw_target_block,
                                   raw_fmax_block,
                                   initial_rule_bound,
                                   fcost_active,
                                   uvalue_active);

    // Accumulate stage-aggregate triple (used by `stage_average` and
    // `stage_uniform` modes).  Any infinite `fmax_b` propagates
    // `fmax_e → +inf` without overflow.  `is_infinity` consults
    // `lp.infinity()` so the comparison matches the solver-backend
    // convention rather than the raw DblMax sentinel.
    if (emit_stage_qeh) {
      const auto dur_ratio = block.duration() / stage.duration();
      fmin_e += dur_ratio * rb.fmin;
      if (is_infinity(rb.fmax, lp.infinity())) {
        fmax_e = lp.infinity();
      } else if (!is_infinity(fmax_e, lp.infinity())) {
        fmax_e += dur_ratio * rb.fmax;
      }
      if (rb.target.has_value()) {
        target_e += dur_ratio * *rb.target;
        has_target_e = true;
      }
      fcost_e += dur_ratio * block_fcost_at(block);
      uvalue_e += dur_ratio * block_uvalue_at(block);
    }

    if (!emit_block_flow) {
      continue;  // `stage_uniform`: no per-block flow column.
    }

    const auto block_ctx =
        make_block_context(scenario.uid(), stage.uid(), block.uid());

    // Per-block fcost / uvalue resolution.  Falls back to the global
    // hydro_spill_cost / hydro_use_value (lifted ×3600) when the
    // schedule cell is unset — same fallback chain as the legacy
    // per-stage path.
    const Real block_fcost_val = block_fcost_at(block);
    const Real block_uvalue_val = block_uvalue_at(block);

    // Per-block flow column.  Kink/target are attached at the block
    // scope only in `per_block` mode; in `stage_average` the kink
    // moves to qeh (target = nullopt at block scope).
    const auto block_target =
        block_kink_active ? rb.target : std::optional<Real> {};
    const Real sn_cost_cf = (block_kink_active && block_fcost_val > 0.0)
        ? CostHelper::block_ecost(scenario, stage, block, block_fcost_val)
        : 0.0;
    const Real sp_cost_cf = (block_kink_active && block_uvalue_val != 0.0)
        ? -CostHelper::block_ecost(scenario, stage, block, block_uvalue_val)
        : 0.0;

    const auto fa = attach_flow(lp,
                                rb.fmin,
                                rb.fmax,
                                block_target,
                                sp_cost_cf,
                                sn_cost_cf,
                                uid(),
                                FlowNames {
                                    .class_name = cname,
                                    .flow_name = FlowName,
                                    .sp_name = ExcessName,
                                    .sn_name = FailName,
                                    .kink_name = FlowKinkName,
                                },
                                block_ctx);

    fcols[buid] = fa.flow_col;
    if (fa.excess_col) {
      bexcess_cols[buid] = *fa.excess_col;
    }
    if (fa.fail_col) {
      bfail_cols[buid] = *fa.fail_col;
    }
    if (fa.kink_row) {
      bkink_rows[buid] = *fa.kink_row;
    }
    if (fa.substituted_target.has_value()) {
      btarget_values[buid] = *fa.substituted_target;
      bfcost_only[buid] =
          fa.substituted_fcost_only ? std::uint8_t {1} : std::uint8_t {0};
    }
  }

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  flow_cols[st_key] = std::move(fcols);
  if (!bfail_cols.empty()) {
    fail_cols[st_key] = std::move(bfail_cols);
  }
  if (!bexcess_cols.empty()) {
    excess_cols[st_key] = std::move(bexcess_cols);
  }
  if (!bkink_rows.empty()) {
    flow_kink_rows[st_key] = std::move(bkink_rows);
  }
  if (!btarget_values.empty()) {
    block_target_values_[st_key] = std::move(btarget_values);
    block_fcost_only_[st_key] = std::move(bfcost_only);
  }

  // PAMPL-visible registration: a single `flow` column per block.
  if (!flow_cols.at(st_key).empty()) {
    sc.add_ampl_variable(
        ampl_name, uid(), FlowName, scenario, stage, flow_cols.at(st_key));
  }

  // Store bound rule state for `update_lp`.
  if (opt_rule.has_value()) {
    BoundState bs {.current_bound = initial_rule_bound};
    if (axis_uses_reservoir(opt_rule->axis)) {
      const auto rsv_sid = ReservoirLPSId(opt_rule->reservoir);
      const auto& rsv = sc.element<ReservoirLP>(rsv_sid);
      bs.reservoir_cache = make_reservoir_ref_cache(rsv, scenario, stage);
    }
    m_bound_states_[st_key] = bs;
  }

  // ── Stage-scope qeh (stage_average / stage_uniform modes) ────────
  if (emit_stage_qeh) {
    const auto stage_dur = stage.duration();
    const auto stage_ctx = make_stage_context(scenario.uid(), stage.uid());

    // The stage-scope `attach_flow` is identical to the per-block one;
    // the only differences are scope-appropriate cost factors and the
    // chosen name bundle (`qeh`, `qeh_sp`, `qeh_sn`, `qkink`).  Per-
    // block fcost / uvalue are duration-weighted into the stage-level
    // `fcost_e` / `uvalue_e` aggregates above so the qeh kink sees a
    // single representative cost per scope.
    const auto stage_target =
        has_target_e ? std::optional<Real> {target_e} : std::optional<Real> {};
    const Real sn_cost_cf = (fcost_active && fcost_e > 0.0)
        ? CostHelper::scenario_stage_ecost(scenario, stage, fcost_e)
        : 0.0;
    const Real sp_cost_cf = (uvalue_active && uvalue_e != 0.0)
        ? -CostHelper::scenario_stage_ecost(scenario, stage, uvalue_e)
        : 0.0;
    const auto fa = attach_flow(lp,
                                fmin_e,
                                fmax_e,
                                stage_target,
                                sp_cost_cf,
                                sn_cost_cf,
                                uid(),
                                FlowNames {
                                    .class_name = cname,
                                    .flow_name = QehName,
                                    .sp_name = QehSpName,
                                    .sn_name = QehSnName,
                                    .kink_name = QkinkName,
                                },
                                stage_ctx);
    qeh_cols[st_key] = fa.flow_col;
    if (fa.excess_col) {
      qeh_sp_cols[st_key] = *fa.excess_col;
    }
    if (fa.fail_col) {
      qeh_sn_cols[st_key] = *fa.fail_col;
    }
    if (fa.kink_row) {
      qkink_rows[st_key] = *fa.kink_row;
    }
    // qeh-scope substitution cache: consumed by `update_lp` to keep
    // the target-side clamp on a bound-rule re-clamp (there is still
    // no stage-scope output reconstruction path).
    if (fa.substituted_target.has_value()) {
      qeh_target_values_[st_key] = *fa.substituted_target;
      qeh_fcost_only_[st_key] =
          fa.substituted_fcost_only ? std::uint8_t {1} : std::uint8_t {0};
    }

    // `stage_average` mode: install the qavg row that links qeh to the
    // per-block flow columns.  `stage_uniform` skips this — there are
    // no per-block flow columns and qeh is itself the served flow.
    if (mode == FlowMode::stage_average) {
      auto avg_row =
          SparseRow {
              .class_name = cname,
              .constraint_name = QavgName,
              .variable_uid = uid(),
              .context = stage_ctx,
          }
              .equal(0.0);
      avg_row[fa.flow_col] = 1.0;
      const auto& my_fcols = flow_cols.at(st_key);
      for (auto&& block : blocks) {
        const auto buid = block.uid();
        const auto dur_ratio = block.duration() / stage_dur;
        avg_row[my_fcols.at(buid)] = -dur_ratio;
      }
      avg_rows[st_key] = lp.add_row(std::move(avg_row));
    }
  }

  // Consumptive coupling: subtract the served flow from each block of
  // the upstream Junction balance row.  In `per_block` / `stage_average`
  // the per-block `flow_b` is subtracted; in `stage_uniform` the same
  // `qeh` column is subtracted from every block's balance row (the
  // right's flow is uniform across the stage).
  if (const auto& j_ref = flow_right().junction_a; j_ref.has_value()) {
    const JunctionLPSId j_sid(*j_ref);
    const auto& j_lp = sc.element(j_sid);
    const auto& j_brows = j_lp.balance_rows_at(scenario, stage);

    // Non-consumptive mode: the served flow is NOT lost at `junction_a` —
    // it is also credited (+1) to `junction_b`, so the right keeps the
    // water in the river (debit `junction_a`, credit `junction_b`, like
    // a Waterway/Turbine arc) while still enforcing its [fmin,fmax]/target
    // band.  Default (`consumptive` unset/true) keeps the legacy
    // pure-consumer behaviour.  Requires `junction_b`.
    const bool return_flow = !flow_right().consumptive.value_or(true)
        && flow_right().junction_b.has_value();

    if (mode == FlowMode::stage_uniform) {
      // Same qeh column subtracted from every block's balance row —
      // physically: the right takes a constant rate `qeh` across the
      // entire stage.
      const auto qeh_it = qeh_cols.find(st_key);
      if (qeh_it != qeh_cols.end()) {
        for (auto&& block : blocks) {
          const auto brow_it = j_brows.find(block.uid());
          if (brow_it != j_brows.end()) {
            lp.row_at(brow_it->second)[qeh_it->second] = -1.0;
          }
        }
        if (return_flow) {
          const JunctionLPSId ret_sid(*flow_right().junction_b);
          const auto& ret_brows =
              sc.element(ret_sid).balance_rows_at(scenario, stage);
          for (auto&& block : blocks) {
            const auto rit = ret_brows.find(block.uid());
            if (rit != ret_brows.end()) {
              lp.row_at(rit->second)[qeh_it->second] = +1.0;
            }
          }
        }
      }
    } else {
      const auto& my_fcols = flow_cols[st_key];
      for (auto&& block : blocks) {
        const auto buid = block.uid();
        auto brow_it = j_brows.find(buid);
        auto fcol_it = my_fcols.find(buid);
        if (brow_it != j_brows.end() && fcol_it != my_fcols.end()) {
          lp.row_at(brow_it->second)[fcol_it->second] = -1.0;
        }
      }
      if (return_flow) {
        const JunctionLPSId ret_sid(*flow_right().junction_b);
        const auto& ret_brows =
            sc.element(ret_sid).balance_rows_at(scenario, stage);
        for (auto&& block : blocks) {
          const auto buid = block.uid();
          auto rit = ret_brows.find(buid);
          auto fcol_it = my_fcols.find(buid);
          if (rit != ret_brows.end() && fcol_it != my_fcols.end()) {
            lp.row_at(rit->second)[fcol_it->second] = +1.0;
          }
        }
      }
    }
  }

  // Optional pass-through bypass.  When ``junction_b`` is set,
  // emit one ``bypass_X`` column per block, contribute it negatively
  // to ``junction_a``'s balance (water leaves the source junction
  // alongside the consumption flow) and positively to
  // ``junction_b``'s balance (water arrives downstream).  Only
  // active for per-block / stage_average modes; stage_uniform is
  // typically used for steady environmental releases that don't need
  // a pass-through alternative.
  const auto& bypass_ref = flow_right().junction_b;
  const auto& junction_ref = flow_right().junction_a;
  if (bypass_ref.has_value() && mode != FlowMode::stage_uniform
      && junction_ref.has_value())
  {
    const JunctionLPSId src_sid(*junction_ref);
    const JunctionLPSId dst_sid(*bypass_ref);
    const auto& src_lp = sc.element(src_sid);
    const auto& dst_lp = sc.element(dst_sid);
    const auto& src_brows = src_lp.balance_rows_at(scenario, stage);
    const auto& dst_brows = dst_lp.balance_rows_at(scenario, stage);
    const Real bypass_cost_raw = flow_right().bypass_cost.value_or(0.0);

    for (auto&& block : blocks) {
      const auto buid = block.uid();
      const auto block_ctx =
          make_block_context(scenario.uid(), stage.uid(), buid);
      const Real bypass_cost_cf = bypass_cost_raw != 0.0
          ? CostHelper::block_ecost(scenario, stage, block, bypass_cost_raw)
          : 0.0;
      const auto bypass_col = lp.add_col(SparseCol {
          .lowb = 0.0,
          .uppb = LinearProblem::DblMax,
          .cost = bypass_cost_cf,
          .class_name = cname,
          .variable_name = BypassName,
          .variable_uid = uid(),
          .context = block_ctx,
      });
      if (auto src_it = src_brows.find(buid); src_it != src_brows.end()) {
        lp.row_at(src_it->second)[bypass_col] = -1.0;
      }
      if (auto dst_it = dst_brows.find(buid); dst_it != dst_brows.end()) {
        lp.row_at(dst_it->second)[bypass_col] = +1.0;
      }
    }
  }

  return true;
}

namespace
{
/// Which side of the one-sided kink substitution to reconstruct.
enum class SlackSide : std::uint8_t
{
  Fail,  ///< fcost-only ⇒ `max(0, target − flow)`
  Excess,  ///< uvalue-only ⇒ `max(0, flow − target)`
};

/// Look up the LP primal of an explicit slack column at (s, t, b).
/// Returns `nullopt` when the (s, t) or block entry is absent — the
/// caller (fail_sol_at / excess_sol_at / add_to_output) then falls
/// back to substitution reconstruction.
[[nodiscard]] std::optional<double> primal_at_block(
    const STBIndexHolder<ColIndex>& cols,
    const std::tuple<ScenarioUid, StageUid>& st_key,
    BlockUid buid,
    const ScaledView& col_sol) noexcept
{
  const auto stt_it = cols.find(st_key);
  if (stt_it == cols.end()) {
    return std::nullopt;
  }
  const auto b_it = stt_it->second.find(buid);
  if (b_it == stt_it->second.end()) {
    return std::nullopt;
  }
  return std::max(0.0, col_sol[b_it->second]);
}

/// Reconstruct the slack value at (s, t, b) under one-sided
/// substitution.  Returns 0 when the cache is absent or the cell
/// belongs to the other side.  Centralises the `fcost_only` /
/// target / flow lookup so `fail_sol_at`, `excess_sol_at`, and the
/// `add_to_output` reconstruction path share a single
/// implementation.
[[nodiscard]] double reconstruct_substituted_slack_at_block(
    SlackSide side,
    const std::tuple<ScenarioUid, StageUid>& st_key,
    BlockUid buid,
    double flow_primal,
    const STBIndexHolder<double>& block_target_values,
    const STBIndexHolder<std::uint8_t>& block_fcost_only) noexcept
{
  const auto tgt_it = block_target_values.find(st_key);
  if (tgt_it == block_target_values.end()) {
    return 0.0;
  }
  const auto fc_it = block_fcost_only.find(st_key);
  if (fc_it == block_fcost_only.end()) {
    return 0.0;
  }
  const auto fc_b_it = fc_it->second.find(buid);
  if (fc_b_it == fc_it->second.end()) {
    return 0.0;
  }
  const bool is_fcost_side = fc_b_it->second != 0;
  if ((side == SlackSide::Fail) != is_fcost_side) {
    return 0.0;
  }
  const auto t_b_it = tgt_it->second.find(buid);
  if (t_b_it == tgt_it->second.end()) {
    return 0.0;
  }
  const double target_v = t_b_it->second;
  return side == SlackSide::Fail ? std::max(0.0, target_v - flow_primal)
                                 : std::max(0.0, flow_primal - target_v);
}
}  // namespace

namespace
{
/// Shared dispatcher: full-kink primal first, then substituted
/// reconstruction.  Used by both `fail_sol_at` and `excess_sol_at`
/// so the lookup chain lives in exactly one place.
[[nodiscard]] double slack_sol_at_impl(
    SlackSide side,
    const std::tuple<ScenarioUid, StageUid>& st_key,
    BlockUid buid,
    const STBIndexHolder<ColIndex>& explicit_cols,
    const STBIndexHolder<ColIndex>& flow_cols_in,
    const STBIndexHolder<double>& target_values,
    const STBIndexHolder<std::uint8_t>& fcost_only,
    const ScaledView& col_sol) noexcept
{
  if (auto v = primal_at_block(explicit_cols, st_key, buid, col_sol)) {
    return *v;
  }
  const auto flow_v_opt = primal_at_block(flow_cols_in, st_key, buid, col_sol);
  if (!flow_v_opt) {
    return 0.0;
  }
  return reconstruct_substituted_slack_at_block(
      side, st_key, buid, *flow_v_opt, target_values, fcost_only);
}
}  // namespace

double FlowRightLP::fail_sol_at(const ScenarioLP& scenario,
                                const StageLP& stage,
                                const BlockLP& block,
                                const ScaledView& col_sol) const noexcept
{
  return slack_sol_at_impl(SlackSide::Fail,
                           {scenario.uid(), stage.uid()},
                           block.uid(),
                           fail_cols,
                           flow_cols,
                           block_target_values_,
                           block_fcost_only_,
                           col_sol);
}

double FlowRightLP::excess_sol_at(const ScenarioLP& scenario,
                                  const StageLP& stage,
                                  const BlockLP& block,
                                  const ScaledView& col_sol) const noexcept
{
  return slack_sol_at_impl(SlackSide::Excess,
                           {scenario.uid(), stage.uid()},
                           block.uid(),
                           excess_cols,
                           flow_cols,
                           block_target_values_,
                           block_fcost_only_,
                           col_sol);
}

bool FlowRightLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();

  out.add_col_sol(cname, FlowName, id(), flow_cols);
  out.add_col_cost(cname, FlowName, id(), flow_cols);

  // Per-block kink slacks (primal = deficit / surplus quantity).
  // Full-kink path emits the explicit slack-column primals directly.
  if (!fail_cols.empty()) {
    out.add_col_sol(cname, FailName, id(), fail_cols);
    out.add_col_cost(cname, FailName, id(), fail_cols);
  }
  if (!excess_cols.empty()) {
    out.add_col_sol(cname, ExcessName, id(), excess_cols);
    out.add_col_cost(cname, ExcessName, id(), excess_cols);
  }
  if (!flow_kink_rows.empty()) {
    out.add_row_dual(cname, FlowKinkName, id(), flow_kink_rows);
  }

  // ── One-sided-substitution reconstruction (per-block) ────────────
  // Mirrors `DemandLP::add_to_output`'s fail_sol reconstruction: the
  // explicit fail/excess slack columns were folded into the primary
  // flow column, so the per-block primal values are reconstructed
  // from the cached target + flow primal.  The branching
  // (`fcost_only` ⇒ fail; else excess) reuses the same
  // `reconstruct_substituted_slack_at_block` helper that
  // `fail_sol_at` / `excess_sol_at` use, keeping a single source
  // of truth for the algebra.  `Demand/load_cost`-style reduced
  // costs are NOT emitted: the (now-elided) slack column's reduced
  // cost was a duplicate of `−flow_cost` in the substituted form
  // and has no downstream consumer.
  if (!block_target_values_.empty()) {
    STBIndexHolder<double> fail_recon;
    STBIndexHolder<double> excess_recon;
    for (const auto& [st_key, target_block] : block_target_values_) {
      const auto fl_it = flow_cols.find(st_key);
      if (fl_it == flow_cols.end()) {
        continue;
      }
      BIndexHolder<double> fail_block;
      BIndexHolder<double> excess_block;
      for (const auto& [buid, target_v] : target_block) {
        const auto fl_b_it = fl_it->second.find(buid);
        if (fl_b_it == fl_it->second.end()) {
          continue;
        }
        const double flow_v = out.primal(fl_b_it->second);
        // Try both sides — the helper returns 0 for the side that
        // doesn't match the cached `fcost_only` flag, so exactly one
        // call produces a non-zero contribution per (s, t, b).
        if (const double fv =
                reconstruct_substituted_slack_at_block(SlackSide::Fail,
                                                       st_key,
                                                       buid,
                                                       flow_v,
                                                       block_target_values_,
                                                       block_fcost_only_);
            fv > 0.0)
        {
          fail_block[buid] = fv;
        }
        if (const double ev =
                reconstruct_substituted_slack_at_block(SlackSide::Excess,
                                                       st_key,
                                                       buid,
                                                       flow_v,
                                                       block_target_values_,
                                                       block_fcost_only_);
            ev > 0.0)
        {
          excess_block[buid] = ev;
        }
      }
      if (!fail_block.empty()) {
        fail_recon[st_key] = std::move(fail_block);
      }
      if (!excess_block.empty()) {
        excess_recon[st_key] = std::move(excess_block);
      }
    }
    if (!fail_recon.empty()) {
      out.add_col_sol_values(cname, FailName, id(), fail_recon);
    }
    if (!excess_recon.empty()) {
      out.add_col_sol_values(cname, ExcessName, id(), excess_recon);
    }
  }

  // Stage-scope qeh + qavg + (optional) stage-level kink slacks.
  if (!qeh_cols.empty()) {
    out.add_col_sol(cname, QehName, id(), qeh_cols);
    out.add_row_dual(cname, QavgName, id(), avg_rows);
  }
  if (!qeh_sp_cols.empty()) {
    out.add_col_sol(cname, QehSpName, id(), qeh_sp_cols);
    out.add_col_cost(cname, QehSpName, id(), qeh_sp_cols);
  }
  if (!qeh_sn_cols.empty()) {
    out.add_col_sol(cname, QehSnName, id(), qeh_sn_cols);
    out.add_col_cost(cname, QehSnName, id(), qeh_sn_cols);
  }
  if (!qkink_rows.empty()) {
    out.add_row_dual(cname, QkinkName, id(), qkink_rows);
  }
  // Stage-scope (qeh) one-sided-substitution reconstruction is
  // intentionally not emitted: no downstream consumer reads
  // `FlowRight/qeh_sp_sol.csv` / `_sn_sol.csv` at stage scope, and
  // `add_col_sol_values` has no stage-keyed overload.  The LP-side
  // elision still happens inside `attach_flow` (col bounds + cost
  // + obj_constant); the algebraic objective is preserved.

  return true;
}

int FlowRightLP::update_lp(SystemLP& sys,
                           const ScenarioLP& scenario,
                           const StageLP& stage)
{
  const auto& opt_rule = flow_right().bound_rule;
  if (!opt_rule.has_value()) {
    return 0;
  }

  auto& li = sys.linear_interface();
  const auto& options = sys.options();

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  auto& state = m_bound_states_.at(st_key);

  // Re-resolve the fcost / uvalue activity flags the same way
  // add_to_lp does (only used to drive resolve_bounds's defaults —
  // the kink slacks themselves are NOT touched on a bound_rule
  // re-clamp).  Both fields are now per-(stage, block); the schedule
  // is "active" when any cell is set OR the global fallback is
  // present AND non-zero.
  const auto global_fc = options.hydro_spill_cost();
  const bool global_fc_nonzero =
      global_fc.has_value() && global_fc.value() != 0.0;
  const bool fcost_active = fcost.has_value() || global_fc_nonzero;

  const auto global_uv = options.hydro_use_value();
  const bool uvalue_active = uvalue_sched.has_value()
      || (global_uv.has_value() && global_uv.value() != 0.0);

  const auto axis_value = resolve_bound_rule_axis_value(
      *opt_rule,
      stage.month(),
      [&]() -> Real
      {
        return average_volume_from_cache(
            sys, scenario, stage, state.reservoir_cache);
      });

  const auto new_bound = evaluate_bound_rule(*opt_rule, axis_value);

  if (new_bound == state.current_bound) {
    return 0;
  }

  int total = 0;
  const auto mode = resolve_flow_mode(flow_right());
  const bool re_clamp_blocks = (mode != FlowMode::stage_uniform);
  const bool re_clamp_qeh = (mode != FlowMode::per_block);

  // Accumulate stage-aggregate (fmin_e, fmax_e) used by stage_average
  // and stage_uniform; the per-block clamp also runs for both
  // per_block and stage_average.
  Real fmin_e = 0.0;
  Real fmax_e = 0.0;
  for (auto&& block : stage.blocks()) {
    const auto buid = block.uid();
    const auto rb = resolve_bounds(fmin_sched.at(stage.uid(), buid),
                                   target_sched.at(stage.uid(), buid),
                                   fmax_sched.at(stage.uid(), buid),
                                   new_bound,
                                   fcost_active,
                                   uvalue_active);
    if (re_clamp_qeh) {
      const auto dur_ratio = block.duration() / stage.duration();
      fmin_e += dur_ratio * rb.fmin;
      // Mirror add_to_lp: route the unboundedness check through the
      // backend's infinity sentinel rather than the raw DblMax.
      if (li.is_pos_inf(rb.fmax)) {
        fmax_e = li.infinity();
      } else if (!li.is_pos_inf(fmax_e)) {
        fmax_e += dur_ratio * rb.fmax;
      }
    }
    if (re_clamp_blocks) {
      const auto& my_fcols = flow_cols.at(st_key);
      const auto fcol_it = my_fcols.find(buid);
      if (fcol_it != my_fcols.end()) {
        // Preserve the one-sided kink substitution: the folded slack
        // cost on the flow column is only valid on the kink side of
        // the CACHED target (the algebra was fixed at build time), so
        // the re-clamp must keep uppb = min(fmax, target) under
        // fcost-only and lowb = max(fmin, target) under uvalue-only.
        // Without this the re-clamp re-opens the far side of the kink
        // and the folded cost pays/charges beyond the target.
        Real low = rb.fmin;
        Real upp = rb.fmax;
        if (const auto tgt_it = block_target_values_.find(st_key);
            tgt_it != block_target_values_.end())
        {
          if (const auto t_b = tgt_it->second.find(buid);
              t_b != tgt_it->second.end())
          {
            const bool is_fcost_only =
                block_fcost_only_.at(st_key).at(buid) != 0;
            if (is_fcost_only) {
              upp = std::min(upp, t_b->second);
            } else {
              low = std::max(low, t_b->second);
            }
          }
        }
        li.set_col_low(fcol_it->second, low);
        li.set_col_upp(fcol_it->second, upp);
        ++total;
      }
    }
  }
  if (re_clamp_qeh) {
    const auto qeh_it = qeh_cols.find(st_key);
    if (qeh_it != qeh_cols.end()) {
      // Same substitution preservation as the per-block clamp above,
      // at stage scope.
      if (const auto tgt_it = qeh_target_values_.find(st_key);
          tgt_it != qeh_target_values_.end())
      {
        if (qeh_fcost_only_.at(st_key) != 0) {
          fmax_e = std::min(fmax_e, tgt_it->second);
        } else {
          fmin_e = std::max(fmin_e, tgt_it->second);
        }
      }
      li.set_col_low(qeh_it->second, fmin_e);
      li.set_col_upp(qeh_it->second, fmax_e);
      ++total;
    }
  }

  spdlog::trace(
      "FlowRightLP uid={}: updated flow bounds "
      "(axis_value={:.1f}, bound={:.1f})",
      uid(),
      axis_value,
      new_bound);

  state.current_bound = new_bound;
  return total;
}

}  // namespace gtopt
