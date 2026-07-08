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
#include <format>

#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/stage_month_guard.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/volume_right_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// Read a VolumeRight's INCOMING volume at `stage` — the state its
/// balance would start from before any reset provisioning:
///
///   * in-phase predecessor (shared LP): the previous stage's efin
///     column solution (post-solve; config `eini` before the first
///     solve),
///   * cross-phase boundary: the predecessor phase's `efin`
///     StateVariable (`col_sol_physical()`, updated after every
///     forward solve — the same channel `physical_eini_from_cache`
///     uses for reservoirs),
///   * horizon start / no information: the config `eini`.
///
/// Numeric (not an LP expression) — mirrors PLP's FijaMaule, which
/// evaluates the compensation recompute from `VarMaulePrev` each
/// stage/sim (genpdmaule.f:942-957).
template<typename SystemLPT>
[[nodiscard]] Real volume_right_incoming_state(SystemLPT& sys,
                                               const ScenarioLP& scenario,
                                               const StageLP& stage,
                                               const VolumeRightLP& vr)
{
  const Real def = vr.volume_right().eini.value_or(0.0);
  if (stage.index() > 0) {
    const auto& stages = sys.phase().stages();
    const auto& li = sys.linear_interface();
    if (li.is_optimal()) {
      return li
          .get_col_sol()[vr.efin_col_at(scenario, stages[stage.index() - 1])];
    }
    return def;
  }
  if (const auto* prev_sys = sys.prev_phase_sys()) {
    const auto& prev_stages = prev_sys->phase().stages();
    if (!prev_stages.empty()) {
      auto svar_opt = sys.system_context().simulation().state_variable(
          StateVariable::key(scenario,
                             prev_stages.back(),
                             VolumeRightLP::Element::class_name,
                             vr.uid(),
                             "efin"));
      if (svar_opt.has_value()) {
        return svar_opt->get().col_sol_physical();
      }
    }
  }
  return def;
}

}  // namespace

VolumeRightLP::VolumeRightLP(const VolumeRight& pvol, const InputContext& ic)
    : StorageBase(pvol, ic, Element::class_name)
    , demand(ic, Element::class_name, id(), std::move(volume_right().demand))
    , fmax(ic, Element::class_name, id(), std::move(volume_right().fmax))
    , saving_rate(
          ic, Element::class_name, id(), std::move(volume_right().saving_rate))
    , fail_cost(volume_right().fail_cost.value_or(0.0))
{
}

bool VolumeRightLP::add_to_lp(SystemContext& sc,
                              const ScenarioLP& scenario,
                              const StageLP& stage,
                              LinearProblem& lp)
{
  static constexpr const auto& cname = Element::class_name;
  static constexpr auto ampl_name = Element::class_name.snake_case();

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

  // Evaluate initial bound rule if present.  Axis dispatch lives in
  // `resolve_bound_rule_axis_value`; the reservoir lookup is wrapped in
  // a callback so it is skipped on non-reservoir axes.
  const auto& opt_rule = volume_right().bound_rule;
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
        .class_name = Element::class_name.full_name(),
        .variable_name = ExtractionName,
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
          .class_name = Element::class_name.full_name(),
          .variable_name = SavingName,
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
  const auto [prev_stg, prev_ph] = sc.prev_stage(stage);
  const bool is_cross_phase = (prev_stg != nullptr && prev_ph != nullptr);
  // A VR with reset_month requires the stage to carry a calendar month:
  // a silent `nullopt != rm` would skip the provisioning quietly and
  // produce a wrong but feasible LP.  Fail fast instead.
  bool is_reset_stage = rm.has_value()
      && require_stage_month(stage,
                             Element::class_name.full_name(),
                             std::format("uid={}", uid()),
                             "reset_month")
          == *rm;
  // Monthly reset (PLP TipoEtaDE != INTRAETA, genpdmaule.f:937):
  // re-provision at every stage that starts a new calendar month.
  // The horizon's first stage keeps its config eini (PLP resets only
  // for IEta > 1).
  if (volume_right().reset_monthly.value_or(false) && prev_stg != nullptr) {
    const auto cur_month = require_stage_month(stage,
                                               Element::class_name.full_name(),
                                               std::format("uid={}", uid()),
                                               "reset_monthly");
    const auto prev_month =
        require_stage_month(*prev_stg,
                            Element::class_name.full_name(),
                            std::format("uid={}", uid()),
                            "reset_monthly (previous stage)");
    is_reset_stage = is_reset_stage || (prev_month != cur_month);
  }

  const StorageOptions opts {
      .use_state_variable = volume_right().use_state_variable.value_or(true),
      .daily_cycle = false,
      .skip_state_link = is_reset_stage && is_cross_phase,
      // In-phase reset: decouple this stage's initial state from the
      // previous stage's efin column so the provisioning pin / debit
      // row cannot corrupt the prior balance (storage_lp.hpp shares
      // the column in the same-phase layout).
      .break_stage_chain = is_reset_stage && !is_cross_phase,
      .class_name = Element::class_name.full_name(),
      .variable_uid = uid(),
      .energy_scale = energy_scale,
      .flow_scale = flow_scale,
  };

  if (!StorageBase::add_to_lp(
          cname,
          ampl_name,
          sc,
          scenario,
          stage,
          lp,
          flow_conversion_rate(),
          saving_cols,
          [](BlockUid) { return 1.0; },
          extraction_cols,
          [](BlockUid) { return 1.0; },
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
    // Provision resolution order: explicit ``reset_value`` (PLP
    // up-counter reset, e.g. IVGAF -> 0 at INICIOANTIC), then the
    // bound_rule evaluation, then the first block's ``emax`` (the
    // reset is a per-stage event; block 0 holds the stage's nominal
    // cap).
    const auto stage_cap =
        param_emax(stage.uid(), stage.blocks().front().uid()).value_or(0.0);
    // Provision resolution: reset_value > credit recompute > bound_rule
    // > emax.  The credit path (PLP compensation at INICIOANO) uses
    // build-time config states here; update_lp refreshes it each
    // iteration from the solved/propagated states.
    Real provision = 0.0;
    if (volume_right().reset_value.has_value()) {
      provision = *volume_right().reset_value;
    } else if (const auto& credit_ref = volume_right().reset_credit_right;
               credit_ref.has_value())
    {
      const auto& credit_lp = sc.element(VolumeRightLPSId(*credit_ref));
      provision = std::min(stage_cap,
                           volume_right().eini.value_or(0.0)
                               + credit_lp.volume_right().eini.value_or(0.0));
    } else if (opt_rule.has_value()) {
      provision = initial_rule_bound;
    } else {
      provision = stage_cap;
    }
    if (const auto& debit_ref = volume_right().reset_debit_right;
        debit_ref.has_value())
    {
      // PLP INICIOTEMP debit (genpdlajam.f:234-239): the December
      // riego provision is reduced by the anticipado spending counter
      // — as a ROW, not a numeric subtraction, so the linkage stays
      // exact within the LP and across SDDP phases (the debit's
      // incoming volume is its state-linked ``eini`` column):
      //
      //     eini + debit_eini = provision,   eini >= 0
      //
      // The referenced VolumeRight must have been added to the LP
      // already (earlier in volume_right_array — same ordering
      // contract as ``right_reservoir``).
      const auto& debit_lp = sc.element(VolumeRightLPSId(*debit_ref));
      const auto debit_eini = debit_lp.eini_col_at(scenario, stage);
      const auto eini_idx = eini_col_at(scenario, stage);
      auto& eini_col = lp.col_at(eini_idx);
      eini_col.lowb = 0.0;
      eini_col.uppb = LinearProblem::DblMax;
      auto drow = SparseRow {
          .class_name = Element::class_name.full_name(),
          .constraint_name = ResetDebitName,
          .variable_uid = uid(),
          .context = make_stage_context(scenario.uid(), stage.uid()),
      };
      drow[eini_idx] = 1.0;
      drow[debit_eini] = 1.0;
      reset_debit_rows_[{scenario.uid(), stage.uid()}] =
          lp.add_row(std::move(drow.equal(provision)));
    } else {
      auto& eini_col = lp.col_at(eini_col_at(scenario, stage));
      eini_col.lowb = provision;
      eini_col.uppb = provision;
    }
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
  const auto& extr_cols = extraction_cols_.at(st_key);
  if (!extr_cols.empty()) {
    sc.add_ampl_variable(
        ampl_name, uid(), ExtractionName, scenario, stage, extr_cols);
    sc.add_ampl_variable(
        ampl_name, uid(), FlowName, scenario, stage, extr_cols);
  }
  if (const auto it = saving_cols_.find(st_key);
      it != saving_cols_.end() && !it->second.empty())
  {
    sc.add_ampl_variable(
        ampl_name, uid(), SavingName, scenario, stage, it->second);
  }

  // Store bound rule state for update_lp.  When the rule's axis consumes
  // reservoir state, capture the static reservoir refs here so update_lp
  // doesn't need `sys.element<ReservoirLP>(...)` on the current sys.
  if (opt_rule.has_value()) {
    BoundState bs {.current_bound = initial_rule_bound};
    if (axis_uses_reservoir(opt_rule->axis)) {
      const auto rsv_sid = ReservoirLPSId(opt_rule->reservoir);
      const auto& rsv = sc.element<ReservoirLP>(rsv_sid);
      bs.reservoir_cache = make_reservoir_ref_cache(rsv, scenario, stage);
    }
    m_bound_states_[st_key] = bs;
  }

  // Couple to parent VolumeRight's energy balance if linked
  if (const auto& rr_ref = volume_right().right_reservoir; rr_ref.has_value()) {
    const VolumeRightLPSId vr_sid(*rr_ref);
    const auto& vr_lp = sc.element(vr_sid);
    const auto& vr_erows = vr_lp.energy_rows_at(scenario, stage);
    const double dir = volume_right().direction.value_or(-1);

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
        .class_name = Element::class_name.full_name(),
        .variable_name = FailName,
        .variable_uid = uid(),
        .context = stage_ctx,
    });

    // Demand satisfaction constraint in physical units:
    // sum_b [fcr × duration × extraction(b)] + fail >= demand
    SparseRow drow {
        .class_name = Element::class_name.full_name(),
        .constraint_name = DemandName,
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
  static constexpr const auto& cname = Element::class_name;

  StorageBase::add_to_output(out, cname);

  return true;
}

int VolumeRightLP::update_lp(SystemLP& sys,
                             const ScenarioLP& scenario,
                             const StageLP& stage)
{
  const auto& opt_rule = volume_right().bound_rule;
  const auto& credit_ref = volume_right().reset_credit_right;
  if (!opt_rule.has_value() && !credit_ref.has_value()) {
    return 0;
  }

  auto& li = sys.linear_interface();

  // ── Credit recompute (PLP FijaMaule INICIOANO, genpdmaule.f:942-957):
  //    provision = min(emax, own_incoming + credit_incoming), read
  //    numerically from the solved/propagated states each iteration.
  //    Independent of the bound_rule machinery below.
  if (credit_ref.has_value()) {
    const auto& rm_credit = volume_right().reset_month;
    const bool is_credit_reset = rm_credit.has_value()
        && require_stage_month(stage,
                               Element::class_name.full_name(),
                               std::format("uid={}", uid()),
                               "reset_month (credit update)")
            == *rm_credit;
    if (is_credit_reset
        && (stage.index() > 0 || sys.prev_phase_sys() != nullptr))
    {
      const auto& credit_lp =
          sys.system_context().template element<VolumeRightLP>(
              VolumeRightLPSId(*credit_ref));
      const Real own_in =
          volume_right_incoming_state(sys, scenario, stage, *this);
      const Real credit_in =
          volume_right_incoming_state(sys, scenario, stage, credit_lp);
      const auto cap =
          param_emax(stage.uid(), stage.blocks().front().uid()).value_or(0.0);
      const Real provision = std::min(cap, own_in + credit_in);
      const auto eini_col = eini_col_at(scenario, stage);
      li.set_col_low(eini_col, provision);
      li.set_col_upp(eini_col, provision);
      if (!opt_rule.has_value()) {
        return 1;
      }
    }
    if (!opt_rule.has_value()) {
      return 0;
    }
  }

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  auto& state = m_bound_states_.at(st_key);

  // Resolve the rule's axis input via the dispatcher.  Reservoir axes
  // route through `state.reservoir_cache` (populated in add_to_lp);
  // axes that don't consume reservoir state never invoke the callback,
  // so the cache stays untouched.
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
  const auto& my_extraction_cols = extraction_cols_.at(st_key);

  for (const auto& [buid, col] : my_extraction_cols) {
    li.set_col_upp(col, new_bound);
    ++total;
  }

  // Dynamic provisioning: at reset_month, update eini to the new
  // bound_rule value (PLP: DerRiego recomputed from reservoir volume).
  const auto& rm = volume_right().reset_month;
  const bool is_reset_stage_update = rm.has_value()
      && require_stage_month(stage,
                             Element::class_name.full_name(),
                             std::format("uid={}", uid()),
                             "reset_month (update_lp)")
          == *rm;
  if (is_reset_stage_update) {
    // ``reset_value`` provisions are static — nothing to refresh.
    if (!volume_right().reset_value.has_value()) {
      if (const auto it = reset_debit_rows_.find({scenario.uid(), stage.uid()});
          it != reset_debit_rows_.end())
      {
        // Debit-row provisioning: refresh the row RHS
        // (eini + debit_eini = provision).
        li.set_row_equal_to(it->second, new_bound);
      } else {
        const auto eini_col = eini_col_at(scenario, stage);
        li.set_col_low(eini_col, new_bound);
        li.set_col_upp(eini_col, new_bound);
      }
      ++total;
    }
  }

  SPDLOG_TRACE(
      "VolumeRightLP uid={}: updated bounds "
      "(axis_value={:.1f}, bound={:.1f})",
      uid(),
      axis_value,
      new_bound);

  state.current_bound = new_bound;

  return total;
}

}  // namespace gtopt
