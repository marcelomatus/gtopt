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
 *
 * ## Balance equation
 *
 *   efin(b) = efin(b-1) + fcr × dur(b) × saving(b) / escale
 *                        - fcr × dur(b) × extraction(b) / escale
 *
 * The volume starts at emax (full rights entitlement) and decrements
 * as extraction occurs (depletion model).  Economy VolumeRights
 * (purpose="economy") use the saving variable to receive deposits
 * of unused rights (PLP: IVESN/IVERN/IVAPN).
 *
 * At reset_month, eini is re-provisioned:
 *   - With bound_rule: eini = evaluate_bound_rule(reservoir_volume)
 *     (PLP: DerRiego = Base + Σ(Factor_i × Zone_Volume_i))
 *   - Without bound_rule: eini = emax (simple full reprovision)
 *
 * Efficiency is 1.0 — rights are consumed 1:1 without loss.
 */
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
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
    , saving_rate(ic, ClassName, id(), std::move(volume_right().saving_rate))
    , fail_cost(volume_right().fail_cost.value_or(0.0))
{
}

bool VolumeRightLP::add_to_lp(SystemContext& sc,
                              const ScenarioLP& scenario,
                              const StageLP& stage,
                              LinearProblem& lp)
{
  static constexpr std::string_view cname = ClassName.full_name();
  static const auto ampl_name = std::string {ClassName.snake_case()};

  sc.register_ampl_element(ampl_name, id().second, uid());

  if (!is_active(stage)) {
    return true;
  }

  auto&& blocks = stage.blocks();

  // Resolve energy_scale from VariableScaleMap. When not set explicitly,
  // inherit from source reservoir or parent VolumeRight.
  const double energy_scale = [&]
  {
    const auto vs = sc.options().variable_scale_map().lookup(
        "VolumeRight", "energy", uid());
    if (vs != 1.0) {
      return vs;
    }
    if (const auto& r_ref = volume_right().reservoir; r_ref.has_value()) {
      const ReservoirLPSId r_sid(*r_ref);
      return sc.element<ReservoirLP>(r_sid).energy_scale();
    }
    if (const auto& rr_ref = volume_right().right_reservoir; rr_ref.has_value())
    {
      const VolumeRightLPSId vr_sid(*rr_ref);
      return sc.element(vr_sid).energy_scale();
    }
    return 1.0;
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

  BIndexHolder<ColIndex> extraction_cols;
  map_reserve(extraction_cols, blocks.size());

  // flow_scale is resolved by add_col from VariableScaleMap metadata.
  double flow_scale = 1.0;

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    auto uppb = fmax.at(stage.uid(), buid).value_or(LinearProblem::DblMax);

    // Apply bound rule cap to upper bound
    if (opt_rule.has_value()) {
      uppb = std::min(uppb, initial_rule_bound);
    }

    const auto fcol = lp.add_col(SparseCol {
        .uppb = uppb,
        .class_name = ClassName.full_name(),
        .variable_name = "flow",
        .variable_uid = uid(),
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });

    extraction_cols[buid] = fcol;
    flow_scale = lp.get_col_scale(fcol);
  }

  // Create saving (inflow) columns for economy VolumeRights.
  // When saving_rate is set, a saving variable is created per block
  // representing deposits of unused rights into this economy.
  BIndexHolder<ColIndex> saving_cols;
  if (volume_right().saving_rate.has_value()) {
    map_reserve(saving_cols, blocks.size());
    for (auto&& block : blocks) {
      const auto buid = block.uid();
      const auto uppb =
          saving_rate.at(stage.uid(), buid).value_or(LinearProblem::DblMax);
      const auto fcol = lp.add_col(SparseCol {
          .uppb = uppb,
          .class_name = ClassName.full_name(),
          .variable_name = "flow",
          .variable_uid = uid(),
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      });
      saving_cols[buid] = fcol;
    }
  }

  // The storage balance tracks remaining rights volume.
  //
  //   vrt_vol = vrt_prev + fcr × dur × saving / escale
  //                       - fcr × dur × extraction / escale
  //
  // Efficiency = 1.0 (rights consumed 1:1, no loss).
  //
  // When reset_month fires at a cross-phase boundary, skip state variable
  // linking: the sini value is independently provisioned, so backward
  // duals and forward trial values from the previous phase are meaningless.
  const auto& rm = volume_right().reset_month;
  const bool is_reset_stage = rm.has_value() && stage.month() == rm;
  const auto [prev_stg, prev_ph] = sc.prev_stage(stage);
  const bool is_cross_phase = (prev_stg != nullptr && prev_ph != nullptr);

  const StorageOptions opts {
      .use_state_variable = volume_right().use_state_variable.value_or(true),
      .daily_cycle = false,
      .skip_state_link = is_reset_stage && is_cross_phase,
      .class_name = ClassName.full_name(),
      .variable_uid = uid(),
      .energy_scale = energy_scale,
      .flow_scale = flow_scale,
  };

  if (!StorageBase::add_to_lp(cname,
                              ampl_name,
                              sc,
                              scenario,
                              stage,
                              lp,
                              flow_conversion_rate(),
                              saving_cols,
                              1.0,
                              extraction_cols,
                              1.0,
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

  // Dynamic provisioning at the designated month boundary.
  // When the stage's month matches reset_month, re-provision the
  // rights volume for the new period (PLP: FijaLajaMBloA / FijaMaule).
  //
  // With bound_rule: eini = evaluate_bound_rule(reservoir_volume)
  //   → dynamic provisioning based on current reservoir level.
  //   This matches PLP's DerRiego = Base + Σ(Factor_i × Zone_Volume_i).
  //
  // Without bound_rule: eini = emax (simple full reprovision).
  //
  // Note: when this fires at a cross-phase boundary, skip_state_link
  // ensures that the SDDP forward pass does not overwrite this value
  // with the previous phase's efin, and backward duals are not
  // propagated through the reset.
  if (is_reset_stage) {
    const auto provision = opt_rule.has_value()
        ? initial_rule_bound
        : param_emax(stage.uid()).value_or(0.0);
    auto& eini_col = lp.col_at(eini_col_at(scenario, stage));
    eini_col.lowb = provision;
    eini_col.uppb = provision;
  }

  // Store columns for external coupling access
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  extraction_cols_[st_key] = extraction_cols;
  if (!saving_cols.empty()) {
    saving_cols_[st_key] = saving_cols;
  }

  // Register volume_right-specific PAMPL columns.  The canonical name is
  // `extraction`; `flow` is kept as a convenience spelling that matches
  // waterway/flow_right.  Storage-generic variables (energy/eini/efin/
  // soft_emin) are registered centrally by StorageBase::add_to_lp.
  if (!extraction_cols_.at(st_key).empty()) {
    sc.add_ampl_variable(ampl_name,
                         uid(),
                         ExtractionName,
                         scenario,
                         stage,
                         extraction_cols_.at(st_key));
    sc.add_ampl_variable(ampl_name,
                         uid(),
                         FlowName,
                         scenario,
                         stage,
                         extraction_cols_.at(st_key));
  }
  if (const auto it = saving_cols_.find(st_key);
      it != saving_cols_.end() && !it->second.empty())
  {
    sc.add_ampl_variable(
        ampl_name, uid(), SavingName, scenario, stage, it->second);
  }

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
      // Physical coefficient: flow → energy conversion factor.
      // flatten() applies col_scale to both flow and energy columns.
      const auto coeff = flow_conversion_rate() * block.duration() * dir;
      lp.row_at(vr_erows.at(buid))[extraction_cols.at(buid)] = coeff;
    }
  }

  // Consumptive coupling: extraction removes water from physical Reservoir.
  // Physical coefficient: +fcr × duration.
  // flatten() applies col_scale to both flow and energy columns.
  if (const auto& r_ref = volume_right().reservoir; r_ref.has_value()) {
    const ReservoirLPSId r_sid(*r_ref);
    const auto& r_lp = sc.element(r_sid);
    const auto& r_erows = r_lp.energy_rows_at(scenario, stage);

    for (auto&& block : blocks) {
      const auto buid = block.uid();
      const auto coeff = +flow_conversion_rate() * block.duration();
      lp.row_at(r_erows.at(buid))[extraction_cols.at(buid)] = coeff;
    }
  }

  // Add demand satisfaction constraint with deficit penalty.
  // sum_b [duration_b * flow_conversion_rate * extraction(b)] + fail >= demand
  // Implemented per-stage: if demand is specified, create a fail variable
  // and a constraint row.
  const auto stage_demand = demand.at(stage.uid());
  if (stage_demand.has_value() && *stage_demand > 0.0) {
    // Deficit variable — physical cost, flatten() applies col_scale.
    const auto stage_ctx = make_stage_context(scenario.uid(), stage.uid());
    const auto fail_col = lp.add_col(SparseCol {
        .cost = fail_cost,
        .scale = energy_scale,
        .class_name = ClassName.full_name(),
        .variable_name = "fail",
        .variable_uid = uid(),
        .context = stage_ctx,
    });

    // Demand satisfaction constraint in physical units:
    // sum_b [fcr × duration × extraction(b)] + fail >= demand
    SparseRow drow {
        .class_name = ClassName.full_name(),
        .constraint_name = "demand",
        .variable_uid = uid(),
        .context = stage_ctx,
    };

    for (auto&& block : blocks) {
      const auto buid = block.uid();
      const auto duration = block.duration();
      // Physical coefficient: flow → demand conversion.
      const auto coeff = flow_conversion_rate() * duration;
      drow[extraction_cols.at(buid)] = coeff;
    }
    drow[fail_col] = 1.0;

    [[maybe_unused]] const auto row_idx =
        lp.add_row(std::move(drow.greater_equal(*stage_demand)));
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

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
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
  const auto& my_extraction_cols = extraction_cols_.at(st_key);

  for (const auto& [buid, col] : my_extraction_cols) {
    li.set_col_upp(col, new_bound);
    ++total;
  }

  // Dynamic provisioning: at reset_month, update eini to the new
  // bound_rule value (PLP: DerRiego recomputed from reservoir volume).
  if (const auto& rm = volume_right().reset_month;
      rm.has_value() && stage.month() == rm)
  {
    const auto eini_col = eini_col_at(scenario, stage);
    li.set_col_low(eini_col, new_bound);
    li.set_col_upp(eini_col, new_bound);
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
