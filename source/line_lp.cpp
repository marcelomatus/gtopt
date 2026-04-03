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

void LineLP::add_kirchhoff_rows(SystemContext& sc,
                                const ScenarioLP& scenario,
                                const StageLP& stage,
                                LinearProblem& lp,
                                const BusLP& bus_a_lp,
                                const BusLP& bus_b_lp,
                                const BIndexHolder<ColIndex>& fpcols,
                                const BIndexHolder<ColIndex>& fncols)
{
  static constexpr std::string_view cname = ClassName.short_name();

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

  const double scale_theta = sc.options().scale_theta();
  const double X = stage_reactance.value();
  // V defaults to 1.0 (per-unit mode).  When V is in kV, X must be in Ω
  // so that B = V²/X yields consistent susceptance units.
  const double V = voltage.at(stage.uid()).value_or(1);
  // Scaled susceptance: χ_l = X / (V² × scale_theta).
  // Since scale_theta is small (e.g. 1e-4), dividing makes chi large,
  // matching the scaled-up theta variables (theta_LP = theta_phys /
  // scale_theta).
  const double x = X / (V * V * scale_theta);

  // Off-nominal tap ratio: scales effective susceptance by τ.
  // Kirchhoff: -θ'_a + θ'_b + τ·χ·f_p − τ·χ·f_n = −φ/scale_theta
  const double tau = tap_ratio.at(stage.uid()).value_or(1.0);
  const double x_tau = tau * x;

  // Phase-shift angle in radians; shifts the equality constraint RHS.
  const double phi_deg = phase_shift_deg.at(stage.uid()).value_or(0.0);
  const double phi_rad = phi_deg * std::numbers::pi / 180.0;

  // Row normalization: divide the entire Kirchhoff row by |x_tau| so that
  // flow coefficients become ±1 and theta coefficients become ±1/|x_tau|.
  // This reduces the coefficient ratio within each row from |x_tau| to 1,
  // improving numerical conditioning.
  const double abs_x_tau = std::abs(x_tau);
  const double row_norm = (abs_x_tau > 0.0) ? abs_x_tau : 1.0;
  const double inv_norm = 1.0 / row_norm;

  // Normalized RHS = -(phi_rad / scale_theta) / row_norm
  const double kirchhoff_rhs = -(phi_rad / scale_theta) * inv_norm;

  // Normalized coefficients: theta terms = ±1/row_norm, flow terms = ±sign
  const double theta_coeff = inv_norm;
  const double flow_sign = (x_tau >= 0.0) ? 1.0 : -1.0;

  BIndexHolder<RowIndex> trows;
  map_reserve(trows, blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    auto trow =
        SparseRow {
            .name =
                sc.lp_row_label(scenario, stage, block, cname, "theta", uid()),
        }
            .equal(kirchhoff_rhs);

    trow.reserve(4);

    trow[theta_a_cols.at(buid)] = -theta_coeff;
    trow[theta_b_cols.at(buid)] = +theta_coeff;
    if (!fpcols.empty()) {
      trow[fpcols.at(buid)] = +flow_sign;
    }
    if (!fncols.empty()) {
      trow[fncols.at(buid)] = -flow_sign;
    }

    trows[buid] = lp.add_row(std::move(trow));
  }

  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  theta_rows[st_key] = std::move(trows);
  // Store normalization factor for dual unscaling in add_to_output.
  theta_row_scale[st_key] = row_norm;
}

// ── add_to_lp ───────────────────────────────────────────────────────

bool LineLP::add_to_lp(SystemContext& sc,
                       const ScenarioLP& scenario,
                       const StageLP& stage,
                       LinearProblem& lp)
{
  static constexpr std::string_view cname = ClassName.short_name();

  if (is_loop()) {
    return true;
  }

  if (!CapacityBase::add_to_lp(sc, scenario, stage, lp)) {
    return false;
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
        sc.block_ecost(scenario, stage, block, stage_tcost);

    auto result = line_losses::add_block(loss_config,
                                         sc,
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
                                         id(),
                                         cname);

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
  }

  // ── Kirchhoff (DC OPF) constraints ────────────────────────────────
  add_kirchhoff_rows(
      sc, scenario, stage, lp, bus_a_lp, bus_b_lp, fpcols, fncols);

  // Store all indices for this (scenario, stage)
  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  capacityp_rows[st_key] = std::move(cprows);
  capacityn_rows[st_key] = std::move(cnrows);
  flowp_cols[st_key] = std::move(fpcols);
  flown_cols[st_key] = std::move(fncols);
  lossp_cols[st_key] = std::move(lpcols);
  lossn_cols[st_key] = std::move(lncols);

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

  out.add_col_sol(cname, "flowp", pid, flowp_cols);
  out.add_col_cost(cname, "flowp", pid, flowp_cols);

  out.add_col_sol(cname, "flown", pid, flown_cols);
  out.add_col_cost(cname, "flown", pid, flown_cols);

  out.add_col_sol(cname, "lossp", pid, lossp_cols);
  out.add_col_sol(cname, "lossn", pid, lossn_cols);

  out.add_row_dual(cname, "capacityp", pid, capacityp_rows);
  out.add_row_dual(cname, "capacityn", pid, capacityn_rows);

  // Kirchhoff duals must be multiplied by row_norm to undo the
  // normalization applied in add_kirchhoff_rows().
  out.add_row_dual(cname, "theta", pid, theta_rows, theta_row_scale);

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
