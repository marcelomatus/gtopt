#include <algorithm>
#include <numbers>

#include <gtopt/line_losses.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

LineLP::LineLP(const Line& pline, const InputContext& ic)
    : CapacityBase(pline, ic, ClassName)
    , tmax_ba(ic, ClassName, id(), std::move(line().tmax_ba))
    , tmax_ab(ic, ClassName, id(), std::move(line().tmax_ab))
    , tcost(ic, ClassName, id(), std::move(line().tcost))
    , lossfactor(ic, ClassName, id(), std::move(line().lossfactor))
    , reactance(ic, ClassName, id(), std::move(line().reactance))
    , voltage(ic, ClassName, id(), std::move(line().voltage))
    , resistance(ic, ClassName, id(), std::move(line().resistance))
    , tap_ratio(ic, ClassName, id(), std::move(line().tap_ratio))
    , phase_shift_deg(ic, ClassName, id(), std::move(line().phase_shift_deg))
{
  SPDLOG_DEBUG("LineLP created: uid={} name='{}'", id().first, id().second);
}

// ── add_kirchhoff_rows ──────────────────────────────────────────────

void LineLP::add_kirchhoff_rows(
    SystemContext& sc,
    const ScenarioLP& scenario,
    const StageLP& stage,
    LinearProblem& lp,
    const BusLP& bus_a_lp,
    const BusLP& bus_b_lp,
    const BIndexHolder<ColIndex>& fpcols,
    const BIndexHolder<ColIndex>& fncols,
    const BIndexHolder<std::vector<ColIndex>>& fpsegcols,
    const BIndexHolder<std::vector<ColIndex>>& fnsegcols)
{
  const auto& stage_reactance = sc.stage_reactance(stage, reactance);
  // Skip Kirchhoff for lines without reactance (DC/HVDC lines).
  // A zero-reactance line would create a degenerate constraint
  // (θ_a = θ_b) that doesn't model DC power flow correctly.
  if (!stage_reactance || stage_reactance.value() == 0.0) {
    return;
  }

  const auto& blocks = stage.blocks();
  const auto& theta_a_cols =
      bus_a_lp.theta_cols_at(sc, scenario, stage, lp, blocks);
  const auto& theta_b_cols =
      bus_b_lp.theta_cols_at(sc, scenario, stage, lp, blocks);

  if (theta_a_cols.empty() || theta_b_cols.empty()) {
    return;
  }

  const double X = stage_reactance.value();
  // V defaults to 1.0 (per-unit mode).  When V is in kV, X must be in Ω
  // so that B = V²/X yields consistent susceptance units.
  const double V = voltage.at(stage.uid()).value_or(1);
  // Physical susceptance term x = X / V² (reactance per V²).
  const double x = X / (V * V);

  // Off-nominal tap ratio: scales effective susceptance by τ.
  const double tau = tap_ratio.at(stage.uid()).value_or(1.0);
  const double x_tau = tau * x;
  if (x_tau == 0.0) {
    return;
  }

  // Phase-shift angle in radians; shifts the equality constraint RHS.
  const double phi_deg = phase_shift_deg.at(stage.uid()).value_or(0.0);
  const double phi_rad = phi_deg * std::numbers::pi / 180.0;

  // Natural Kirchhoff form (no manual row pre-scaling):
  //
  //   -θ_a + θ_b + x_tau·f_p − x_tau·f_n = −φ_rad
  //
  // Prior versions divided the entire row by |x_tau| so that flow
  // coefficients became ±1, but this required a dual back-scale factor
  // (`theta_row_scale`) to recover physical units on output, and it
  // defeated the LP layer's row-max equilibration (which already handles
  // row norms internally).
  //
  // Writing the row in its natural form hands row scaling back to
  // `linear_problem.cpp` row-max equilibration, which auto-unscales
  // duals. The theta column is separately col-scaled by `scale_theta`
  // (chosen as median(|x_tau|) in planning_lp.cpp:auto_scale_theta),
  // so after equilibration the median line's row has near-unit
  // coefficients both in the theta and flow directions.
  const double kirchhoff_rhs = -phi_rad;

  BIndexHolder<RowIndex> trows;
  map_reserve(trows, blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    auto trow =
        SparseRow {
            .class_name = ClassName.full_name(),
            .constraint_name = ThetaName,
            .variable_uid = uid(),
            .context =
                make_block_context(scenario.uid(), stage.uid(), block.uid()),
        }
            .equal(kirchhoff_rhs);

    // piecewise_direct mode stamps each segment column directly with
    // ±x_τ (PLP genpdlin.f); other modes stamp the aggregator. Per
    // block, exactly one of {segs, aggregator} is populated per
    // direction.  Pre-reserve roughly: 2 thetas + segs + aggregator.
    const auto fp_seg_it = fpsegcols.find(buid);
    const auto fn_seg_it = fnsegcols.find(buid);
    const auto fp_seg_n =
        (fp_seg_it != fpsegcols.end()) ? fp_seg_it->second.size() : 0;
    const auto fn_seg_n =
        (fn_seg_it != fnsegcols.end()) ? fn_seg_it->second.size() : 0;
    trow.reserve(2 + fp_seg_n + fn_seg_n + 2);

    trow[theta_a_cols.at(buid)] = -1.0;
    trow[theta_b_cols.at(buid)] = +1.0;

    if (fp_seg_n != 0) {
      for (const auto& col : fp_seg_it->second) {
        trow[col] = +x_tau;
      }
    } else if (auto fit = fpcols.find(buid); fit != fpcols.end()) {
      trow[fit->second] = +x_tau;
    }
    if (fn_seg_n != 0) {
      for (const auto& col : fn_seg_it->second) {
        trow[col] = -x_tau;
      }
    } else if (auto fit = fncols.find(buid); fit != fncols.end()) {
      trow[fit->second] = -x_tau;
    }

    trows[buid] = lp.add_row(std::move(trow));
  }

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  theta_rows[st_key] = std::move(trows);
}

// ── add_to_lp ───────────────────────────────────────────────────────

bool LineLP::add_to_lp(SystemContext& sc,
                       const ScenarioLP& scenario,
                       const StageLP& stage,
                       LinearProblem& lp)
{
  static constexpr auto ampl_name = ClassName.snake_case();

  if (is_loop()) {
    return true;
  }

  if (!CapacityBase::add_to_lp(sc, ampl_name, scenario, stage, lp)) {
    return false;
  }

  // F9: register filter metadata for sum(...) predicates.
  {
    AmplElementMetadata metadata;
    metadata.reserve(3);
    if (const auto& t = line().type) {
      metadata.emplace_back(TypeKey, *t);
    }
    // Resolve via `sc.element<BusLP>` (handles both Uid and Name forms
    // of the JSON-side `bus_a` / `bus_b` SingleId variant — `std::get<Uid>`
    // would throw if the JSON used a string name).
    metadata.emplace_back(
        BusAKey, static_cast<double>(sc.element<BusLP>(bus_a_sid()).uid()));
    metadata.emplace_back(
        BusBKey, static_cast<double>(sc.element<BusLP>(bus_b_sid()).uid()));
    sc.register_ampl_element_metadata(ampl_name, uid(), std::move(metadata));
  }

  if (!is_active(stage)) [[unlikely]] {
    return true;
  }

  const auto& bus_a_lp = sc.element<BusLP>(bus_a_sid());
  const auto& bus_b_lp = sc.element<BusLP>(bus_b_sid());
  if (!bus_a_lp.is_active(stage) || !bus_b_lp.is_active(stage)) {
    return true;
  }

  const auto& balance_rows_a = bus_a_lp.balance_rows_at(scenario, stage);
  const auto& balance_rows_b = bus_b_lp.balance_rows_at(scenario, stage);
  const auto& blocks = stage.blocks();

  const auto [opt_capacity, capacity_col] = capacity_and_col(stage, lp);
  const double stage_capacity = opt_capacity.value_or(LinearProblem::DblMax);
  const auto stage_tcost = tcost.at(stage.uid()).value_or(0.0);

  // ── Resolve loss mode via the modular engine ──────────────────────
  const bool has_expansion = capacity_col.has_value();
  const auto loss_mode =
      line_losses::resolve_mode(line(), sc.options(), has_expansion);

  const auto lf = lossfactor.at(stage.uid()).value_or(0.0);
  const auto R = resistance.at(stage.uid()).value_or(0.0);
  const auto V = voltage.at(stage.uid()).value_or(0.0);
  const int nseg =
      std::max(1, line().loss_segments.value_or(sc.options().loss_segments()));
  // Use finite opt_capacity for fmax; when no capacity is defined
  // (opt_capacity is nullopt), fall back to the scheduled tmax values
  // (actual flow limits).
  double fmax = 0.0;
  if (opt_capacity) {
    fmax = std::max(*opt_capacity, 0.0);
  } else {
    for (const auto& block : blocks) {
      const auto buid = block.uid();
      const double tab = tmax_ab.at(stage.uid(), buid).value_or(0.0);
      const double tba = tmax_ba.at(stage.uid(), buid).value_or(0.0);
      fmax = std::max({fmax, tab, tba});
    }
  }
  const auto allocation = line().loss_allocation_mode_enum();

  const auto loss_config = line_losses::make_config(
      loss_mode, line(), allocation, lf, R, V, nseg, fmax);

  BIndexHolder<ColIndex> fpcols;
  BIndexHolder<RowIndex> cprows;
  BIndexHolder<ColIndex> fncols;
  BIndexHolder<RowIndex> cnrows;
  BIndexHolder<ColIndex> lpcols;
  BIndexHolder<ColIndex> lncols;
  BIndexHolder<std::vector<ColIndex>> fpsegcols;
  BIndexHolder<std::vector<ColIndex>> fnsegcols;
  map_reserve(fpcols, blocks.size());
  map_reserve(cprows, blocks.size());
  map_reserve(fncols, blocks.size());
  map_reserve(cnrows, blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    auto& brow_a = lp.row_at(balance_rows_a.at(buid));
    auto& brow_b = lp.row_at(balance_rows_b.at(buid));

    const auto [block_tmax_ab, block_tmax_ba] = sc.block_maxmin_at(
        stage, block, tmax_ab, tmax_ba, stage_capacity, -stage_capacity);
    const auto block_tcost =
        CostHelper::block_ecost(scenario, stage, block, stage_tcost);

    auto result = line_losses::add_block(loss_config,
                                         scenario,
                                         stage,
                                         block,
                                         lp,
                                         brow_a,
                                         brow_b,
                                         block_tmax_ab,
                                         block_tmax_ba,
                                         block_tcost,
                                         capacity_col,
                                         uid());

    if (result.fp_col) {
      fpcols[buid] = *result.fp_col;
    }
    if (result.fn_col) {
      fncols[buid] = *result.fn_col;
    }
    if (result.lossp_col) {
      lpcols[buid] = *result.lossp_col;
    }
    if (result.lossn_col) {
      lncols[buid] = *result.lossn_col;
    }
    if (result.capp_row) {
      cprows[buid] = *result.capp_row;
    }
    if (result.capn_row) {
      cnrows[buid] = *result.capn_row;
    }
    if (!result.seg_p_cols.empty()) {
      fpsegcols[buid] = std::move(result.seg_p_cols);
    }
    if (!result.seg_n_cols.empty()) {
      fnsegcols[buid] = std::move(result.seg_n_cols);
    }
  }

  // ── Kirchhoff (DC OPF) constraints ────────────────────────────────
  add_kirchhoff_rows(sc,
                     scenario,
                     stage,
                     lp,
                     bus_a_lp,
                     bus_b_lp,
                     fpcols,
                     fncols,
                     fpsegcols,
                     fnsegcols);

  // Store all indices for this (scenario, stage)
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  capacityp_rows[st_key] = std::move(cprows);
  capacityn_rows[st_key] = std::move(cnrows);
  flowp_cols[st_key] = std::move(fpcols);
  flown_cols[st_key] = std::move(fncols);
  lossp_cols[st_key] = std::move(lpcols);
  lossn_cols[st_key] = std::move(lncols);

  // Register PAMPL-visible columns.
  if (!flowp_cols.at(st_key).empty()) {
    sc.add_ampl_variable(
        ampl_name, uid(), FlowpName, scenario, stage, flowp_cols.at(st_key));
  }
  if (!flown_cols.at(st_key).empty()) {
    sc.add_ampl_variable(
        ampl_name, uid(), FlownName, scenario, stage, flown_cols.at(st_key));
  }
  if (!lossp_cols.at(st_key).empty()) {
    sc.add_ampl_variable(
        ampl_name, uid(), LosspName, scenario, stage, lossp_cols.at(st_key));
  }
  if (!lossn_cols.at(st_key).empty()) {
    sc.add_ampl_variable(
        ampl_name, uid(), LossnName, scenario, stage, lossn_cols.at(st_key));
  }
  // `capainst` is registered centrally by CapacityBase::add_to_lp.
  // The `line.flow` compound (+1·flowp − 1·flown) is registered once
  // per SimulationLP by `system_lp.cpp::register_all_ampl_element_names`
  // (called via std::call_once from the SystemLP constructor).

  return true;
}

// ── add_to_output ───────────────────────────────────────────────────

bool LineLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();
  if (is_loop()) {
    return true;
  }

  const auto pid = id();

  out.add_col_sol(cname, FlowpName, pid, flowp_cols);
  out.add_col_cost(cname, FlowpName, pid, flowp_cols);

  out.add_col_sol(cname, FlownName, pid, flown_cols);
  out.add_col_cost(cname, FlownName, pid, flown_cols);

  out.add_col_sol(cname, LosspName, pid, lossp_cols);
  out.add_col_sol(cname, LossnName, pid, lossn_cols);

  out.add_row_dual(cname, CapacitypName, pid, capacityp_rows);
  out.add_row_dual(cname, CapacitynName, pid, capacityn_rows);

  // Kirchhoff rows are now written in their natural form (no manual
  // pre-scaling), so duals come out in physical units directly and no
  // post-hoc back-scale is needed. Row-max equilibration is handled by
  // the LP layer, which auto-unscales duals.
  out.add_row_dual(cname, ThetaName, pid, theta_rows);

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
