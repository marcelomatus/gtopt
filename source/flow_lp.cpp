/**
 * @file      flow_lp.cpp
 * @brief     Implementation of FlowLP methods
 * @date      Wed Jul 30 15:56:46 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Contains implementation of:
 * - FlowLP construction and initialization
 * - Adding flow variables to LP problems
 * - Managing flow constraints in junctions
 * - AR(1) inflow-model rows and the lagged-inflow state variable
 *   (opt-in via ``Flow.inflow_model`` — see
 *   docs/formulation/sddp-ar-inflows.md)
 * - Output generation for flow solutions
 */

#include <algorithm>
#include <format>
#include <functional>
#include <optional>
#include <stdexcept>

#include <gtopt/cost_helper.hpp>
#include <gtopt/flow_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

FlowLP::FlowLP(const Flow& pflow, const InputContext& ic)
    : ObjectLP<Flow>(pflow)
    , discharge(ic, Element::class_name, id(), std::move(flow().discharge))
    , fcost(ic, Element::class_name, id(), std::move(flow().fcost))
{
  // Fail fast on an unsupported inflow-model type: silently ignoring a
  // typo'd `"type": "par3"` would run the deterministic model while the
  // user believes the AR structure is active.
  if (const auto& im = flow().inflow_model; im.has_value()) {
    const auto type = im->type.value_or(Name {"ar1"});
    if (type != "ar1") {
      throw std::invalid_argument(
          std::format("Flow {}: unsupported inflow_model.type '{}' "
                      "(only \"ar1\" is implemented)",
                      uid(),
                      type));
    }
  }
}

bool FlowLP::add_to_lp(SystemContext& sc,
                       const ScenarioLP& scenario,
                       const StageLP& stage,
                       LinearProblem& lp)
{
  static constexpr auto ampl_name = Element::class_name.snake_case();

  if (!is_active(stage)) {
    return true;
  }

  // Junction is optional: flow-turbine mode has no junction.
  const JunctionLP* junction_ptr = nullptr;
  if (has_junction()) {
    junction_ptr = &sc.element<JunctionLP>(junction_sid());
    if (!junction_ptr->is_active(stage)) {
      return true;
    }
  }

  const auto& blocks = stage.blocks();

  BIndexHolder<ColIndex> fcols;
  map_reserve(fcols, blocks.size());

  const bool has_inflow_model = flow().inflow_model.has_value();

  // ``--lp-reduction``: fold FIXED-discharge flows (constants) into the
  // junction-balance RHS instead of emitting a fixed singleton column.
  // Disabled under the AR inflow model: the model needs live columns —
  // both as AR-row members and as the state-variable source the next
  // stage/phase chains from.
  const bool reduce_fixed_flow =
      sc.options().lp_reduction() && !has_inflow_model;

  // ── AR(1) inflow model: resolve this stage's lag ─────────────────────
  //
  // The lag is the previous stage's last-block inflow (reference).  At a
  // cross-phase boundary it becomes a dependent ``inflow_lag`` column
  // linked to the previous phase's ``inflow`` StateVariable (the exact
  // `efin → eini` pattern); within a phase it is the previous stage's
  // in-LP flow column.  The first stage of the horizon has no lag: its
  // columns stay bound-pinned to the schedule (initial inflow
  // condition).  See docs/formulation/sddp-ar-inflows.md §2.
  const auto stage_has_fcost = [this](const StageLP& st)
  {
    return std::ranges::any_of(
        st.blocks(),
        [&](const auto& b)
        { return fcost.optval(st.uid(), b.uid()).has_value(); });
  };

  double ar_phi = 0.0;
  double ar_mu_prev = 0.0;
  std::optional<ColIndex> ar_lag_col;
  StageUid ar_prev_stage_uid {};
  BlockUid ar_prev_ref_block_uid {};
  bool ar_cross_phase = false;
  bool ar_state_source = false;  // register the outgoing inflow state?

  if (has_inflow_model && junction_ptr != nullptr) {
    if (stage_has_fcost(stage)) {
      // The soft-band `fcost` slack semantics conflict with the AR
      // equality row (design doc §3): the flow would be both a costed
      // decision and an AR-determined quantity.  Unsupported in v1.
      SPDLOG_WARN(
          "Flow {}: inflow_model is ignored at stage {} because fcost "
          "is set (unsupported combination, see "
          "docs/formulation/sddp-ar-inflows.md §3)",
          uid(),
          stage.uid());
    } else {
      ar_state_source = true;
      ar_phi = flow().inflow_model.value_or(InflowModel {}).phi.value_or(0.0);
      const auto [prev_stage, prev_phase] = sc.prev_stage(stage);
      if (prev_stage != nullptr && is_active(*prev_stage)
          && !stage_has_fcost(*prev_stage))
      {
        const auto prev_ref_buid = prev_stage->blocks().back().uid();
        const auto mu_prev =
            discharge.at(scenario.uid(), prev_stage->uid(), prev_ref_buid);
        if (mu_prev.has_value()) {
          if (prev_phase != nullptr) {
            // Cross-phase boundary: dependent lag column, pinned at
            // build time to the schedule reference so every non-SDDP
            // path solves the identical LP.  The SDDP forward pass
            // re-pins it to the realized trial value via
            // `propagate_trial_values` (same mechanism as `sini`).
            const auto lag = lp.add_col({
                .lowb = *mu_prev,
                .uppb = *mu_prev,
                .class_name = Element::class_name.full_name(),
                .variable_name = InflowLagName,
                .variable_uid = uid(),
                .context = make_stage_context(scenario.uid(), stage.uid()),
            });
            sc.defer_state_link(
                // NOLINTNEXTLINE(readability-suspicious-call-argument)
                StateVariable::key(scenario,
                                   *prev_stage,
                                   Element::class_name,
                                   uid(),
                                   InflowName),
                lag);
            ar_lag_col = lag;
            ar_cross_phase = true;
          } else {
            // Same phase (shared LP): chain directly off the previous
            // stage's reference column — no state variable needed.
            ar_lag_col = lookup_flow_col(scenario, *prev_stage, prev_ref_buid);
          }
          if (ar_lag_col.has_value()) {
            ar_mu_prev = *mu_prev;
            ar_prev_stage_uid = prev_stage->uid();
            ar_prev_ref_block_uid = prev_ref_buid;
          }
        }
      }
    }
  }

  BIndexHolder<RowIndex> ar_rows;
  if (ar_lag_col.has_value()) {
    map_reserve(ar_rows, blocks.size());
  }
  // Last emitted flow column (blocks iterate in ascending uid order) —
  // the AR state registration below uses it as the outgoing inflow
  // state.  Tracked here, storage_lp-style, instead of via
  // `std::prev(fcols.end())`: std::flat_map iterators advertise a
  // legacy input_iterator category, so std::prev is not applicable.
  std::optional<ColIndex> ar_last_flow_col;

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    // ``discharge`` is OPTIONAL — when set, the column is FORCED to
    // exactly ``discharge`` (legacy hard equality, regardless of
    // ``fcost``).  When unset, the column is a free slack ``[0,
    // +inf)`` priced at ``fcost`` so the LP only activates it when
    // the junction balance demands it.  ``fcost`` itself is also
    // optional — when neither field is set, no LP column is emitted.
    const auto block_discharge_opt =
        discharge.at(scenario.uid(), stage.uid(), block.uid());
    const auto block_fcost = fcost.optval(stage.uid(), buid);
    if (!block_discharge_opt.has_value() && !block_fcost.has_value()) {
      continue;
    }

    // lp_reduction: a FIXED-discharge flow (discharge set, no fcost) is a
    // constant.  Fold ±discharge into the junction-balance RHS rather than
    // emit a fixed [d, d] singleton column (PaPILO-style singleton
    // substitution).  Dual-transparent: the balance row's dual (water value /
    // LMP) is unchanged.  Skipped when fcost is set (a soft slack stays a
    // column) or there is no junction (flow-turbine mode keeps the column).
    if (reduce_fixed_flow && junction_ptr != nullptr
        && block_discharge_opt.has_value() && !block_fcost.has_value())
    {
      const double d = *block_discharge_opt;
      const auto& balance_rows = junction_ptr->balance_rows_at(scenario, stage);
      auto& brow = lp.row_at(balance_rows.at(buid));
      // The balance LHS would receive (is_input ? +1 : -1)·d; moving that
      // constant to the RHS gives (is_input ? -d : +d).
      const double delta = is_input() ? -d : d;
      brow.lowb += delta;
      brow.uppb += delta;
      continue;  // no LP column emitted for this fixed flow
    }

    // Under the AR model the column is FREE — the AR equality row below
    // pins it through the lag instead of the bounds, so backward-pass
    // duals expose ∂V/∂inflow on the lag column.  Unbounded rather than
    // [0, ∞): the AR extrapolation μ + φ·Δ may cross zero and v1 does
    // not truncate (design doc §2).
    const bool ar_block =
        ar_lag_col.has_value() && block_discharge_opt.has_value();

    const double block_lowb =
        ar_block ? -DblMax : block_discharge_opt.value_or(0.0);
    const double block_uppb =
        ar_block ? DblMax : block_discharge_opt.value_or(DblMax);
    const double block_cost = block_fcost.has_value()
        ? CostHelper::block_ecost(scenario, stage, block, *block_fcost)
        : 0.0;

    const auto fcol = lp.add_col({
        .lowb = block_lowb,
        .uppb = block_uppb,
        .cost = block_cost,
        .class_name = Element::class_name.full_name(),
        .variable_name = FlowName,
        .variable_uid = uid(),
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });
    fcols[buid] = fcol;
    ar_last_flow_col = fcol;

    // adding flow to the junction balances (only when junction exists)
    if (junction_ptr != nullptr) {
      const auto& balance_rows = junction_ptr->balance_rows_at(scenario, stage);
      auto& brow = lp.row_at(balance_rows.at(buid));
      brow[fcol] = is_input() ? 1 : -1;
    }

    // AR(1) recursion row:  q_b − φ·lag = μ_b − φ·μ_ref  (ε = 0 on the
    // forward path; apertures rewrite the RHS via `update_aperture`).
    // With the lag at its build/trial pin μ_ref this yields q_b = μ_b —
    // LP values identical to the pinned-bounds formulation.
    if (ar_block) {
      auto ar_row = SparseRow {
          .class_name = Element::class_name.full_name(),
          .constraint_name = ArBalanceName,
          .variable_uid = uid(),
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      };
      ar_row.equal(*block_discharge_opt - (ar_phi * ar_mu_prev));
      ar_row[fcol] = 1.0;
      ar_row[*ar_lag_col] = -ar_phi;
      ar_rows[buid] = lp.add_row(std::move(ar_row));
    }
  }

  // ── AR(1): register the outgoing inflow state ────────────────────────
  //
  // The last block's flow column is the stage's lagged-inflow state:
  // the next stage (same phase) chains off it directly, and the next
  // phase's `inflow_lag` dependent column links to it through the
  // standard `PendingStateLink` machinery, so backward Benders cuts
  // automatically gain ∂V/∂inflow coefficients
  // (docs/formulation/sddp-cut-validity.md §3 applies unchanged).
  if (ar_state_source && ar_last_flow_col.has_value()) {
    const auto last_col = ar_last_flow_col.value_or(ColIndex {unknown_index});
    sc.add_state_col(
        lp,
        // NOLINTNEXTLINE(readability-suspicious-call-argument)
        StateVariable::key(
            scenario, stage, Element::class_name, uid(), InflowName),
        last_col,
        /*scost=*/0.0,
        /*var_scale=*/lp.get_col_scale(last_col),
        make_stage_context(scenario.uid(), stage.uid()));
  }

  // storing the indices for this scenario and stage
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  flow_cols[st_key] = std::move(fcols);

  if (!ar_rows.empty()) {
    // ar_rows is only populated under `ar_block`, which requires
    // `ar_lag_col`; value_or keeps the optional access checked anyway.
    ar_info[st_key] = ArStageInfo {
        .rows = std::move(ar_rows),
        .lag_col = ar_lag_col.value_or(ColIndex {unknown_index}),
        .phi = ar_phi,
        .mu_prev_ref = ar_mu_prev,
        .prev_stage_uid = ar_prev_stage_uid,
        .prev_ref_block_uid = ar_prev_ref_block_uid,
        .cross_phase = ar_cross_phase,
    };
  }

  // Register PAMPL-visible columns.
  if (!flow_cols.at(st_key).empty()) {
    sc.add_ampl_variable(
        ampl_name, uid(), FlowName, scenario, stage, flow_cols.at(st_key));
  }

  return true;
}

bool FlowLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();

  out.add_col_sol(cname, FlowName, id(), flow_cols);
  out.add_col_cost(cname, FlowName, id(), flow_cols);

  return true;
}

bool FlowLP::update_aperture(
    LinearInterface& li,
    const ScenarioLP& base_scenario,
    const std::function<std::optional<double>(StageUid, BlockUid)>& value_fn,
    const StageLP& stage) const
{
  if (!is_active(stage)) {
    return true;
  }

  const auto st_key = std::tuple {base_scenario.uid(), stage.uid()};

  // ── AR(1) mode: the flow columns are FREE; the aperture hydrology
  // enters through the AR row RHS instead of the column bounds, so the
  // per-aperture cut keeps its ∂V/∂inflow slope on the lag column
  // (design doc §2, "Apertures").  RHS = a_b − φ·lag_ref, where the lag
  // reference is the aperture's own previous-stage value for an in-LP
  // (same-phase) lag, and the current trial pin of the dependent
  // `inflow_lag` column at a cross-phase lag — so the aperture realizes
  // q = a_b exactly at the trial point, matching the legacy
  // bound-pinning semantics.
  //
  // NOT an early return: a block with no discharge value at build time
  // has no AR row (`ar_block` requires the schedule value), so its
  // column kept the legacy bounds — the loop below still applies the
  // aperture pin to those blocks.
  const auto ar_it = ar_info.find(st_key);
  const ArStageInfo* ai = ar_it != ar_info.end() ? &ar_it->second : nullptr;
  if (ai != nullptr) {
    for (const auto& [block_uid, row] : ai->rows) {
      const auto new_val = value_fn(stage.uid(), block_uid);
      if (!new_val.has_value()) {
        continue;  // keep the forward-pass RHS (no change)
      }
      // Cross-phase lag: the clone carries the forward pass's trial pin
      // (lo == up).  Same-phase lag: the aperture's own previous-stage
      // value, falling back to the schedule reference.
      const double lag_ref = ai->cross_phase
          ? li.get_col_low()[ai->lag_col]
          : value_fn(ai->prev_stage_uid, ai->prev_ref_block_uid)
                .value_or(ai->mu_prev_ref);
      const double rhs = *new_val - (ai->phi * lag_ref);
      li.set_row_bounds(row, rhs, rhs);
    }
  }

  const auto it = flow_cols.find(st_key);
  if (it == flow_cols.end()) {
    return true;  // no columns registered for this (scenario, stage)
  }

  const auto& fcols = it->second;
  for (const auto& [block_uid, col] : fcols) {
    if (ai != nullptr && ai->rows.contains(block_uid)) {
      continue;  // AR-row block: hydrology entered through the RHS above
    }
    const auto new_val = value_fn(stage.uid(), block_uid);
    if (new_val.has_value()) {
      li.set_col_low(col, *new_val);
      li.set_col_upp(col, *new_val);
    }
    // If nullopt, keep the forward-pass value (no change)
  }

  return true;
}

}  // namespace gtopt
