/**
 * @file      flow_right_lp.cpp
 * @brief     Implementation of FlowRightLP methods
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements LP formulation for flow-based water rights.
 * Creates flow and deficit variables per block, with penalty costs
 * for unmet demand in the objective function.
 *
 * The flow right is NOT part of the hydrological topology —
 * it does not modify junction balance rows.
 */

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

struct BlockBounds
{
  Real lowb;
  Real uppb;
};

/// Compute the per-block flow column bounds from the per-block schedule
/// values and an optional bound-rule cap.  Shared by `add_to_lp` (initial
/// construction) and `update_lp` (volume-dependent re-clamping) so the
/// two stay in sync — historically `update_lp` only patched the upper
/// bound, which left fixed-mode FlowRights with `lowb > uppb` whenever
/// the rule shrank the cap below the original discharge value.
///
/// `rule_bound` is `LinearProblem::DblMax` when no rule is configured, so
/// the `std::min` calls below are no-ops in that case.
BlockBounds compute_block_bounds(Real block_discharge,
                                 Real block_fmax,
                                 Real block_fail_cost,
                                 Real rule_bound)
{
  // Variable mode: fmax > 0 and discharge == 0 -> [0, fmax]
  // Fixed mode (default):                       [discharge, discharge]
  auto lowb =
      (block_fmax > 0.0 && block_discharge == 0.0) ? 0.0 : block_discharge;
  auto uppb = (block_fmax > 0.0 && block_discharge == 0.0) ? block_fmax
                                                           : block_discharge;

  // Apply bound-rule cap (no-op when rule_bound == DblMax).
  uppb = std::min(uppb, rule_bound);
  lowb = std::min(lowb, uppb);

  // When a fail variable is active and there is a target discharge,
  // relax the lower bound to 0 so the optimizer can under-deliver
  // when constrained.  The deficit coupling row captures the shortfall.
  if (block_fail_cost > 0.0 && block_discharge > 0.0) {
    lowb = 0.0;
  }
  return BlockBounds {.lowb = lowb, .uppb = uppb};
}

/// Resolve the effective per-block fail cost: per-element schedule wins,
/// otherwise fall back to the global `hydro_fail_cost` ($/m³) converted
/// to $/(m³/s) for this block via `× duration[h] × 3600`.
Real resolve_block_fail_cost(Real sched_fail_cost,
                             Real block_duration_hours,
                             const auto& options)
{
  if (sched_fail_cost > 0.0) {
    return sched_fail_cost;
  }
  const auto global_fc = options.hydro_fail_cost().value_or(0.0);
  if (global_fc > 0.0) {
    return global_fc * block_duration_hours * 3600.0;
  }
  return 0.0;
}

}  // namespace

FlowRightLP::FlowRightLP(const FlowRight& pflow, const InputContext& ic)
    : ObjectLP<FlowRight>(pflow, ic, ClassName)
    , discharge(ic, ClassName, id(), std::move(flow_right().discharge))
    , direction(static_cast<int>(flow_right().direction.value_or(-1)))
    , fail_cost_sched(ic, ClassName, id(), std::move(flow_right().fail_cost))
    , use_value_sched(ic, ClassName, id(), std::move(flow_right().use_value))
    , fmax_sched(ic, ClassName, id(), std::move(flow_right().fmax))
{
}

bool FlowRightLP::add_to_lp(const SystemContext& sc,
                            const ScenarioLP& scenario,
                            const StageLP& stage,
                            LinearProblem& lp)
{
  static constexpr auto ampl_name = ClassName.snake_case();

  if (!is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();
  const auto& options = sc.options();

  BIndexHolder<ColIndex> fcols;
  BIndexHolder<ColIndex> ffails;
  map_reserve(fcols, blocks.size());
  map_reserve(ffails, blocks.size());

  // Evaluate initial bound rule if present.  The axis dispatch is
  // delegated to `resolve_bound_rule_axis_value` so this call site does
  // not have to special-case each axis kind; the reservoir lookup is
  // skipped entirely on axes that don't consume reservoir state.
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

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    const auto block_discharge =
        discharge.at(scenario.uid(), stage.uid(), block.uid());
    const auto block_fmax = fmax_sched.at(stage.uid(), buid).value_or(0.0);

    // Resolve fail_cost (per-element schedule, falling back to global
    // hydro_fail_cost converted from $/m³ to $/(m³/s)).
    const auto block_fail_cost = resolve_block_fail_cost(
        fail_cost_sched.at(stage.uid(), buid).value_or(0.0),
        block.duration(),
        options);

    // Compute final flow column bounds.  Helper handles variable/fixed
    // mode selection, bound-rule cap, and deficit relaxation in one
    // place so update_lp() can apply identical logic on re-clamp.
    const auto [lowb, uppb] = compute_block_bounds(
        block_discharge, block_fmax, block_fail_cost, initial_rule_bound);

    const bool has_deficit = block_fail_cost > 0.0 && block_discharge > 0.0;

    // Use value: negative objective coefficient on the flow variable
    // (benefit — positive use_value incentivizes flow).
    // Per-element use_value takes precedence; falls back to global
    // hydro_use_value ($/m³) × duration × 3600 to convert to $/(m³/s).
    auto block_use_value = use_value_sched.at(stage.uid(), buid).value_or(0.0);
    if (block_use_value == 0.0) {
      const auto global_uv = options.hydro_use_value().value_or(0.0);
      if (global_uv > 0.0) {
        block_use_value = global_uv * block.duration() * 3600.0;
      }
    }

    const auto block_ctx =
        make_block_context(scenario.uid(), stage.uid(), block.uid());
    const auto fcol = lp.add_col({
        .lowb = lowb,
        .uppb = uppb,
        .cost = -block_use_value,
        .class_name = ClassName.full_name(),
        .variable_name = FlowName,
        .variable_uid = uid(),
        .context = block_ctx,
    });
    fcols[buid] = fcol;

    if (block_fail_cost > 0.0) {
      const auto fail_col = lp.add_col({
          .cost = block_fail_cost,
          .class_name = ClassName.full_name(),
          .variable_name = FailName,
          .variable_uid = uid(),
          .context = block_ctx,
      });
      ffails[buid] = fail_col;

      // Deficit coupling: flow + fail >= discharge.
      // Without this constraint the fail variable is uncoupled
      // (dead) and the optimizer always sets it to 0.
      if (has_deficit) {
        auto demand_row =
            SparseRow {
                .class_name = ClassName.full_name(),
                .constraint_name = DemandName,
                .variable_uid = uid(),
                .context = make_block_context(
                    scenario.uid(), stage.uid(), block.uid()),
            }
                .greater_equal(block_discharge);
        demand_row[fcol] = 1.0;
        demand_row[fail_col] = 1.0;
        [[maybe_unused]] const auto drow = lp.add_row(std::move(demand_row));
      }
    }
  }

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  flow_cols[st_key] = std::move(fcols);
  if (!ffails.empty()) {
    fail_cols[st_key] = std::move(ffails);
  }

  // Register PAMPL-visible columns.
  if (!flow_cols.at(st_key).empty()) {
    sc.add_ampl_variable(
        ampl_name, uid(), FlowName, scenario, stage, flow_cols.at(st_key));
  }
  if (const auto it = fail_cols.find(st_key);
      it != fail_cols.end() && !it->second.empty())
  {
    sc.add_ampl_variable(
        ampl_name, uid(), FailName, scenario, stage, it->second);
  }

  // Store bound rule state for update_lp
  if (opt_rule.has_value()) {
    m_bound_states_[st_key] = BoundState {
        .current_bound = initial_rule_bound,
    };
  }

  // Create stage-average hourly flow variable (qeh) if requested.
  // qeh = Σ_b [ flow(b) × dur(b) / dur_stage ]
  // This mirrors PLP's "H"-suffix variables (IQDRH, IQDEH, etc.)
  if (flow_right().use_average.value_or(false)) {
    const auto stage_dur = stage.duration();

    const auto qeh_col = lp.add_col(SparseCol {
        .class_name = ClassName.full_name(),
        .variable_name = QehName,
        .variable_uid = uid(),
        .context = make_stage_context(scenario.uid(), stage.uid()),
    });
    qeh_cols[st_key] = qeh_col;

    auto avg_row =
        SparseRow {
            .class_name = ClassName.full_name(),
            .constraint_name = QavgName,
            .variable_uid = uid(),
            .context = make_stage_context(scenario.uid(), stage.uid()),
        }
            .equal(0.0);

    avg_row[qeh_col] = 1.0;
    const auto& my_fcols = flow_cols[st_key];
    for (auto&& block : blocks) {
      const auto buid = block.uid();
      const auto dur_ratio = block.duration() / stage_dur;
      avg_row[my_fcols.at(buid)] = -dur_ratio;
    }
    avg_rows[st_key] = lp.add_row(std::move(avg_row));
  }

  // Consumptive coupling: subtract flow from the physical Junction balance.
  // Rights are always consumptive — when a junction is set, the flow
  // variable is subtracted from the junction's balance row.
  if (const auto& j_ref = flow_right().junction; j_ref.has_value()) {
    const JunctionLPSId j_sid(*j_ref);
    const auto& j_lp = sc.element(j_sid);
    const auto& j_brows = j_lp.balance_rows_at(scenario, stage);
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

  return true;
}

bool FlowRightLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();

  out.add_col_sol(cname, FlowName, id(), flow_cols);
  out.add_col_cost(cname, FlowName, id(), flow_cols);

  if (!fail_cols.empty()) {
    out.add_col_sol(cname, FailName, id(), fail_cols);
    out.add_col_cost(cname, FailName, id(), fail_cols);
  }

  if (!qeh_cols.empty()) {
    out.add_col_sol(cname, QehName, id(), qeh_cols);
    out.add_row_dual(cname, QavgName, id(), avg_rows);
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

  // Resolve the rule's input value via the axis dispatcher.  Reservoir
  // lookups are skipped entirely when the axis is not reservoir-driven,
  // so a stage-month rule is allowed to leave `reservoir` unset.
  const auto axis_value = resolve_bound_rule_axis_value(
      *opt_rule,
      stage.month(),
      [&]() -> Real
      {
        const auto rsv_sid = ReservoirLPSId(opt_rule->reservoir);
        const auto& rsv = sys.element<ReservoirLP>(rsv_sid);
        const auto default_volume = rsv.reservoir().eini.value_or(0.0);
        const auto vini =
            rsv.physical_eini(sys, scenario, stage, default_volume, rsv_sid);
        const auto vfin =
            rsv.physical_efin(sys, scenario, stage, default_volume);
        return (vini + vfin) / 2.0;
      });

  const auto new_bound = evaluate_bound_rule(*opt_rule, axis_value);

  if (new_bound == state.current_bound) {
    return 0;
  }

  int total = 0;
  const auto& my_fcols = flow_cols.at(st_key);

  // Re-clamp every block via the same helper used in add_to_lp so that
  // both column bounds (not just upper) are updated consistently when
  // the rule output shrinks below the original discharge value.
  for (auto&& block : stage.blocks()) {
    const auto buid = block.uid();
    const auto fcol_it = my_fcols.find(buid);
    if (fcol_it == my_fcols.end()) {
      continue;
    }

    const auto block_discharge =
        discharge.at(scenario.uid(), stage.uid(), buid);
    const auto block_fmax = fmax_sched.at(stage.uid(), buid).value_or(0.0);
    const auto block_fail_cost = resolve_block_fail_cost(
        fail_cost_sched.at(stage.uid(), buid).value_or(0.0),
        block.duration(),
        options);

    const auto [lowb, uppb] = compute_block_bounds(
        block_discharge, block_fmax, block_fail_cost, new_bound);

    li.set_col_low(fcol_it->second, lowb);
    li.set_col_upp(fcol_it->second, uppb);
    ++total;
  }

  SPDLOG_TRACE(
      "FlowRightLP uid={}: updated bounds "
      "(axis_value={:.1f}, bound={:.1f})",
      uid(),
      axis_value,
      new_bound);

  state.current_bound = new_bound;

  return total;
}

}  // namespace gtopt
