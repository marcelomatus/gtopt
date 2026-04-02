/**
 * @file      volume_right_lp.cpp
 * @brief     Implementation of VolumeRightLP methods
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements LP formulation for volume-based water rights.
 * Uses StorageLP base class for the rights-volume balance and
 * SDDP state variable coupling (Tilmant's "dummy reservoir").
 *
 * The volume right is NOT part of the hydrological topology —
 * it tracks accumulated right volumes in a separate ledger.
 */

#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reservoir_enums.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/volume_right_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

VolumeRightLP::VolumeRightLP(const VolumeRight& pvol, const InputContext& ic)
    : StorageBase(pvol, ic, ClassName)
    , demand(ic, ClassName, id(), std::move(volume_right().demand))
    , fmax(ic, ClassName, id(), std::move(volume_right().fmax))
    , fail_cost(volume_right().fail_cost.value_or(0.0))
{
}

bool VolumeRightLP::add_to_lp(SystemContext& sc,
                              const ScenarioLP& scenario,
                              const StageLP& stage,
                              LinearProblem& lp)
{
  static constexpr std::string_view cname = ClassName.short_name();

  if (!is_active(stage)) {
    return true;
  }

  auto&& blocks = stage.blocks();
  const auto& options = sc.options();
  const auto scale_objective = options.scale_objective();

  // Resolve energy_scale: use explicit value, or inherit from
  // the source reservoir (keeps "colchon" volumes in the same
  // LP scaling as the physical reservoir), else default 1.0.
  const double energy_scale = [&]
  {
    if (volume_right().energy_scale.has_value()) {
      return *volume_right().energy_scale;
    }
    if (const auto& r_ref = volume_right().reservoir; r_ref.has_value()) {
      const ReservoirLPSId r_sid(*r_ref);
      return sc.element<ReservoirLP>(r_sid).energy_scale();
    }
    return VolumeRight::default_energy_scale;
  }();

  // Evaluate initial bound rule if present
  const auto& opt_rule = volume_right().bound_rule;
  auto initial_rule_bound = std::numeric_limits<Real>::max();
  if (opt_rule.has_value()) {
    const auto rsv_sid = ReservoirLPSId(opt_rule->reservoir);
    const auto& rsv = sc.element<ReservoirLP>(rsv_sid);
    const auto initial_volume = rsv.reservoir().eini.value_or(0.0);
    initial_rule_bound = evaluate_bound_rule(*opt_rule, initial_volume);
  }

  BIndexHolder<ColIndex> finp_cols;
  map_reserve(finp_cols, blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    auto uppb = fmax.at(stage.uid(), buid).value_or(LinearProblem::DblMax);

    // Apply bound rule cap to upper bound
    if (opt_rule.has_value()) {
      uppb = std::min(uppb, initial_rule_bound);
    }

    auto col_name =
        sc.lp_col_label(scenario, stage, block, cname, "finp", uid());
    const auto fcol = lp.add_col(SparseCol {
        .name = std::move(col_name),
        .uppb = uppb,
    });

    finp_cols[buid] = fcol;
  }

  // The storage balance tracks accumulated rights volume.
  // Input flow = rights delivered (finp_cols).
  // No output flow — rights only accumulate (fout_cols = finp_cols with
  // zero efficiency to satisfy the signature, but we pass empty).
  // No drain/spillway for rights.
  const StorageOptions opts {
      .use_state_variable = volume_right().use_state_variable.value_or(true),
      .daily_cycle = false,
      .energy_scale = energy_scale,
  };

  if (!StorageBase::add_to_lp(cname,
                              sc,
                              scenario,
                              stage,
                              lp,
                              flow_conversion_rate(),
                              finp_cols,
                              1.0,
                              finp_cols,
                              0.0,
                              LinearProblem::DblMax,
                              std::nullopt,
                              std::nullopt,
                              std::nullopt,
                              opts))
  {
    SPDLOG_CRITICAL("Failed to add storage constraints for volume right {}",
                    uid());
    return false;
  }

  // Reset accumulated volume at the designated month boundary.
  // When the stage's month matches reset_month, fix eini to 0
  // (e.g., Maule irrigation rights reset each April).
  if (const auto& rm = volume_right().reset_month;
      rm.has_value() && stage.month() == rm)
  {
    auto& eini = lp.col_at(eini_col_at(scenario, stage));
    eini.lowb = 0.0;
    eini.uppb = 0.0;
  }

  // Store finp_cols for external coupling access
  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  finp_cols_map[st_key] = finp_cols;

  // Store bound rule state for update_lp
  if (opt_rule.has_value()) {
    m_bound_states_[st_key] = BoundState {
        .current_bound = initial_rule_bound,
    };
  }

  // Couple to parent VolumeRight's energy balance if linked
  if (const auto& rr_ref = volume_right().right_reservoir; rr_ref.has_value()) {
    const VolumeRightLPSId vr_sid(*rr_ref);
    const auto& vr_lp = sc.element(vr_sid);
    const auto& vr_erows = vr_lp.energy_rows_at(scenario, stage);
    const auto dir = static_cast<double>(volume_right().direction.value_or(-1));

    for (auto&& block : blocks) {
      const auto buid = block.uid();
      const auto coeff =
          flow_conversion_rate() * block.duration() * dir / energy_scale;
      lp.row_at(vr_erows.at(buid))[finp_cols.at(buid)] = coeff;
    }
  }

  // Consumptive coupling: subtract extraction from physical Reservoir
  if (const auto& r_ref = volume_right().reservoir;
      r_ref.has_value() && volume_right().consumptive.value_or(false))
  {
    const ReservoirLPSId r_sid(*r_ref);
    const auto& r_lp = sc.element(r_sid);
    const auto& r_erows = r_lp.energy_rows_at(scenario, stage);
    const auto r_energy_scale = r_lp.energy_scale();

    for (auto&& block : blocks) {
      const auto buid = block.uid();
      const auto coeff =
          -flow_conversion_rate() * block.duration() / r_energy_scale;
      lp.row_at(r_erows.at(buid))[finp_cols.at(buid)] = coeff;
    }
  }

  // Add demand satisfaction constraint with deficit penalty.
  // sum_b [duration_b * flow_conversion_rate * finp(b)] + fail >= demand
  // Implemented per-stage: if demand is specified, create a fail variable
  // and a constraint row.
  const auto stage_demand = demand.at(stage.uid());
  if (stage_demand.has_value() && *stage_demand > 0.0) {
    const auto inv_energy_scale = 1.0 / energy_scale;

    // Deficit variable (in hm³ / energy_scale)
    auto fail_col_name = sc.lp_col_label(scenario, stage, cname, "fail", uid());
    const auto fail_col = lp.add_col(SparseCol {
        .name = std::move(fail_col_name),
        .cost = fail_cost * energy_scale / scale_objective,
        .scale = energy_scale,
    });

    // Demand satisfaction constraint:
    // sum_b [coeff_b * finp(b)] + fail >= demand / energy_scale
    auto demand_row_name =
        sc.lp_row_label(scenario, stage, cname, "demand", uid());
    SparseRow drow {
        .name = std::move(demand_row_name),
    };

    for (auto&& block : blocks) {
      const auto buid = block.uid();
      const auto duration = block.duration();
      const auto coeff = flow_conversion_rate() * duration * inv_energy_scale;
      drow[finp_cols.at(buid)] = coeff;
    }
    drow[fail_col] = 1.0;

    [[maybe_unused]] const auto row_idx = lp.add_row(
        std::move(drow.greater_equal(*stage_demand * inv_energy_scale)));
  }

  return true;
}

bool VolumeRightLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();

  StorageBase::add_to_output(out, cname);

  return true;
}

int VolumeRightLP::update_lp(SystemLP& sys,
                             const ScenarioLP& scenario,
                             const StageLP& stage)
{
  const auto& opt_rule = volume_right().bound_rule;
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
  const auto& my_finp_cols = finp_cols_map.at(st_key);

  for (const auto& [buid, col] : my_finp_cols) {
    li.set_col_upp(col, new_bound);
    ++total;
  }

  SPDLOG_TRACE(
      "VolumeRightLP uid={}: updated bounds "
      "(volume={:.1f}, bound={:.1f})",
      uid(),
      volume,
      new_bound);

  state.current_bound = new_bound;

  return total;
}

}  // namespace gtopt
