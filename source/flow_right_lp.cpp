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
  static constexpr std::string_view cname = ClassName.short_name();

  if (!is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();
  const auto& options = sc.options();
  const auto scale_objective = options.scale_objective();

  BIndexHolder<ColIndex> fcols;
  BIndexHolder<ColIndex> ffails;
  map_reserve(fcols, blocks.size());
  map_reserve(ffails, blocks.size());

  // Evaluate initial bound rule if present
  const auto& opt_rule = flow_right().bound_rule;
  auto initial_rule_bound = std::numeric_limits<Real>::max();
  if (opt_rule.has_value()) {
    const auto rsv_sid = ReservoirLPSId(opt_rule->reservoir);
    const auto& rsv = sc.element<ReservoirLP>(rsv_sid);
    const auto initial_volume = rsv.reservoir().eini.value_or(0.0);
    initial_rule_bound = evaluate_bound_rule(*opt_rule, initial_volume);
  }

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    const auto block_discharge =
        discharge.at(scenario.uid(), stage.uid(), block.uid());
    const auto block_fmax = fmax_sched.at(stage.uid(), buid).value_or(0.0);

    // Determine flow column bounds:
    // - Variable mode: fmax > 0 and discharge == 0 -> [0, fmax]
    // - Fixed mode (default): [discharge, discharge]
    auto lowb =
        (block_fmax > 0.0 && block_discharge == 0.0) ? 0.0 : block_discharge;
    auto uppb = (block_fmax > 0.0 && block_discharge == 0.0) ? block_fmax
                                                             : block_discharge;

    // Apply bound rule cap to upper bound
    if (opt_rule.has_value()) {
      uppb = std::min(uppb, initial_rule_bound);
      lowb = std::min(lowb, uppb);
    }

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

    auto flow_col_name =
        sc.lp_col_label(scenario, stage, block, cname, "flow", uid());
    const auto fcol = lp.add_col({
        .name = std::move(flow_col_name),
        .lowb = lowb,
        .uppb = uppb,
        .cost = -block_use_value / scale_objective,
    });
    fcols[buid] = fcol;

    // Deficit variable: penalized in objective when demand unmet.
    // Per-element fail_cost (already in $/flow-unit) takes precedence.
    // Falls back to global hydro_fail_cost ($/m³) × duration × 3600
    // to convert from $/m³ to $/(m³/s) for the block.
    auto block_fail_cost = fail_cost_sched.at(stage.uid(), buid).value_or(0.0);
    if (block_fail_cost == 0.0) {
      const auto global_fc = options.hydro_fail_cost().value_or(0.0);
      if (global_fc > 0.0) {
        block_fail_cost = global_fc * block.duration() * 3600.0;
      }
    }
    if (block_fail_cost > 0.0) {
      auto fail_col_name =
          sc.lp_col_label(scenario, stage, block, cname, "fail", uid());
      const auto fail_col = lp.add_col({
          .name = std::move(fail_col_name),
          .cost = block_fail_cost / scale_objective,
      });
      ffails[buid] = fail_col;
    }
  }

  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  flow_cols[st_key] = std::move(fcols);
  if (!ffails.empty()) {
    fail_cols[st_key] = std::move(ffails);
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
        .name = sc.lp_col_label(scenario, stage, cname, "qeh", uid()),
    });
    qeh_cols[st_key] = qeh_col;

    auto avg_row =
        SparseRow {
            .name = sc.lp_row_label(scenario, stage, cname, "qavg", uid()),
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

  out.add_col_sol(cname, "flow", id(), flow_cols);
  out.add_col_cost(cname, "flow", id(), flow_cols);

  if (!fail_cols.empty()) {
    out.add_col_sol(cname, "fail", id(), fail_cols);
    out.add_col_cost(cname, "fail", id(), fail_cols);
  }

  if (!qeh_cols.empty()) {
    out.add_col_sol(cname, "qeh", id(), qeh_cols);
    out.add_row_dual(cname, "qavg", id(), avg_rows);
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
  const auto rsv_sid = ReservoirLPSId(opt_rule->reservoir);
  const auto& rsv = sys.element<ReservoirLP>(rsv_sid);
  const auto default_volume = rsv.reservoir().eini.value_or(0.0);

  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  auto& state = m_bound_states_.at(st_key);

  const auto vini =
      rsv.physical_eini(sys, scenario, stage, default_volume, rsv_sid);
  const auto vfin = rsv.physical_efin(sys, scenario, stage, default_volume);
  const Real volume = (vini + vfin) / 2.0;

  const auto new_bound = evaluate_bound_rule(*opt_rule, volume);

  if (new_bound == state.current_bound) {
    return 0;
  }

  int total = 0;
  const auto& my_fcols = flow_cols.at(st_key);

  for (const auto& [buid, col] : my_fcols) {
    const auto block_fmax =
        fmax_sched.at(stage.uid(), buid).value_or(new_bound);
    li.set_col_upp(col, std::min(block_fmax, new_bound));
    ++total;
  }

  SPDLOG_TRACE(
      "FlowRightLP uid={}: updated bounds "
      "(volume={:.1f}, bound={:.1f})",
      uid(),
      volume,
      new_bound);

  state.current_bound = new_bound;

  return total;
}

}  // namespace gtopt
