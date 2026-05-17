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
 * `flow_right().junction` is set.
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
  Real fmin;
  std::optional<Real> target;
  Real fmax;
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
/// fcost is reinterpreted as "no soft requirement".
ResolvedBounds resolve_bounds(std::optional<Real> raw_fmin,
                              std::optional<Real> raw_target,
                              std::optional<Real> raw_fmax,
                              Real rule_cap,
                              bool fcost_active,
                              bool uvalue_active)
{
  std::optional<Real> target = raw_target;
  if (target.has_value() && *target == 0.0 && !fcost_active) {
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
/// passed a target AND at least one slack cost was non-zero.
struct FlowAttachment
{
  ColIndex flow_col;
  std::optional<ColIndex> excess_col;
  std::optional<ColIndex> fail_col;
  std::optional<RowIndex> kink_row;
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
  const auto flow_col = lp.add_col(SparseCol {
      .lowb = fmin,
      .uppb = fmax,
      .cost = 0.0,
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
  };

  if (target.has_value() && (sp_cost_cf != 0.0 || sn_cost_cf != 0.0)) {
    // Only emit the slack that carries a non-zero cost.  Skipping the
    // zero-cost side degenerates the kink into a one-sided cap / floor
    // (preserving the pre-refactor soft-target / soft-floor LP shapes):
    //   fcost-only  (sn > 0, sp = 0) ⇒ row "flow + fail = target",
    //                                  flow ≤ target.
    //   uvalue-only (sp > 0, sn = 0) ⇒ row "flow − excess = target",
    //                                  flow ≥ target.
    //   both        ⇒ full kink slack pair.
    std::optional<ColIndex> excess_col;
    std::optional<ColIndex> fail_col;
    if (sp_cost_cf != 0.0) {
      excess_col = lp.add_col(SparseCol {
          .lowb = 0.0,
          .uppb = LinearProblem::DblMax,
          .cost = sp_cost_cf,
          .class_name = names.class_name,
          .variable_name = names.sp_name,
          .variable_uid = variable_uid,
          .context = ctx,
      });
    }
    if (sn_cost_cf != 0.0) {
      fail_col = lp.add_col(SparseCol {
          .lowb = 0.0,
          .uppb = LinearProblem::DblMax,
          .cost = sn_cost_cf,
          .class_name = names.class_name,
          .variable_name = names.sn_name,
          .variable_uid = variable_uid,
          .context = ctx,
      });
    }
    auto row =
        SparseRow {
            .class_name = names.class_name,
            .constraint_name = names.kink_name,
            .variable_uid = variable_uid,
            .context = ctx,
        }
            .equal(*target);
    row[flow_col] = 1.0;
    if (excess_col) {
      row[*excess_col] = -1.0;
    }
    if (fail_col) {
      row[*fail_col] = 1.0;
    }
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

  // Resolve per-stage fcost / uvalue once for the whole stage.  Both are
  // expressed in raw $/(m³/s·h) so that `CostHelper::*_ecost` (which
  // multiplies by block / stage duration) produces $/m³ × m³ totals
  // consistent with `Demand::fcost`.  Falls back to the global
  // hydro_fail_cost / hydro_use_value ($/m³, ×3600 to share units).
  auto stage_fcost = sc.hydro_fail_cost(stage, fcost);
  if (stage_fcost.has_value() && !fcost.at(stage.uid()).has_value()) {
    stage_fcost = OptReal {*stage_fcost * 3600.0};
  }
  const Real stage_fcost_val = stage_fcost.value_or(0.0);
  const bool fcost_active = stage_fcost_val > 0.0;

  Real stage_uvalue_val = uvalue_sched.at(stage.uid()).value_or(0.0);
  if (stage_uvalue_val == 0.0) {
    const auto global_uv = options.hydro_use_value().value_or(0.0);
    if (global_uv > 0.0) {
      stage_uvalue_val = global_uv * 3600.0;
    }
  }
  const bool uvalue_active = stage_uvalue_val != 0.0;

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
    }

    if (!emit_block_flow) {
      continue;  // `stage_uniform`: no per-block flow column.
    }

    const auto block_ctx =
        make_block_context(scenario.uid(), stage.uid(), block.uid());

    // Per-block flow column.  Kink/target are attached at the block
    // scope only in `per_block` mode; in `stage_average` the kink
    // moves to qeh (target = nullopt at block scope).
    const auto block_target =
        block_kink_active ? rb.target : std::optional<Real> {};
    const Real sn_cost_cf = (block_kink_active && fcost_active)
        ? CostHelper::block_ecost(scenario, stage, block, stage_fcost_val)
        : 0.0;
    const Real sp_cost_cf = (block_kink_active && uvalue_active)
        ? -CostHelper::block_ecost(scenario, stage, block, stage_uvalue_val)
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
    // chosen name bundle (`qeh`, `qeh_sp`, `qeh_sn`, `qkink`).
    const auto stage_target =
        has_target_e ? std::optional<Real> {target_e} : std::optional<Real> {};
    const Real sn_cost_cf = fcost_active
        ? CostHelper::scenario_stage_ecost(scenario, stage, stage_fcost_val)
        : 0.0;
    const Real sp_cost_cf = uvalue_active
        ? -CostHelper::scenario_stage_ecost(scenario, stage, stage_uvalue_val)
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
  if (const auto& j_ref = flow_right().junction; j_ref.has_value()) {
    const JunctionLPSId j_sid(*j_ref);
    const auto& j_lp = sc.element(j_sid);
    const auto& j_brows = j_lp.balance_rows_at(scenario, stage);

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
    }
  }

  return true;
}

double FlowRightLP::fail_sol_at(const ScenarioLP& scenario,
                                const StageLP& stage,
                                const BlockLP& block,
                                const ScaledView& col_sol) const noexcept
{
  // Read `fail_b` directly from the LP primal — it IS the slack.
  const std::tuple st_key {scenario.uid(), stage.uid()};
  const auto stt_it = fail_cols.find(st_key);
  if (stt_it == fail_cols.end()) {
    return 0.0;
  }
  const auto b_it = stt_it->second.find(block.uid());
  if (b_it == stt_it->second.end()) {
    return 0.0;
  }
  return std::max(0.0, col_sol[b_it->second]);
}

double FlowRightLP::excess_sol_at(const ScenarioLP& scenario,
                                  const StageLP& stage,
                                  const BlockLP& block,
                                  const ScaledView& col_sol) const noexcept
{
  const std::tuple st_key {scenario.uid(), stage.uid()};
  const auto stt_it = excess_cols.find(st_key);
  if (stt_it == excess_cols.end()) {
    return 0.0;
  }
  const auto b_it = stt_it->second.find(block.uid());
  if (b_it == stt_it->second.end()) {
    return 0.0;
  }
  return std::max(0.0, col_sol[b_it->second]);
}

bool FlowRightLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();

  out.add_col_sol(cname, FlowName, id(), flow_cols);
  out.add_col_cost(cname, FlowName, id(), flow_cols);

  // Per-block kink slacks (primal = deficit / surplus quantity).
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

  // Re-resolve stage_fcost / stage_uvalue the same way add_to_lp does
  // (only used to drive resolve_bounds's defaults — the kink slacks
  // themselves are NOT touched on a bound_rule re-clamp).
  auto stage_fcost = fcost.at(stage.uid());
  if (!stage_fcost.has_value()) {
    const auto global_fc = options.hydro_fail_cost();
    if (global_fc.has_value()) {
      stage_fcost = OptReal {*global_fc * 3600.0};
    }
  }
  const bool fcost_active = stage_fcost.value_or(0.0) > 0.0;

  Real stage_uvalue_val = uvalue_sched.at(stage.uid()).value_or(0.0);
  if (stage_uvalue_val == 0.0) {
    const auto global_uv = options.hydro_use_value().value_or(0.0);
    if (global_uv > 0.0) {
      stage_uvalue_val = global_uv * 3600.0;
    }
  }
  const bool uvalue_active = stage_uvalue_val != 0.0;

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
        li.set_col_low(fcol_it->second, rb.fmin);
        li.set_col_upp(fcol_it->second, rb.fmax);
        ++total;
      }
    }
  }
  if (re_clamp_qeh) {
    const auto qeh_it = qeh_cols.find(st_key);
    if (qeh_it != qeh_cols.end()) {
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
