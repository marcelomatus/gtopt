#include <algorithm>

#include <gtopt/kirchhoff_node_angle.hpp>
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
    : CapacityBase(pline, ic, Element::class_name)
    , tmax_ba(ic, Element::class_name, id(), std::move(line().tmax_ba))
    , tmax_ab(ic, Element::class_name, id(), std::move(line().tmax_ab))
    , tcost(ic, Element::class_name, id(), std::move(line().tcost))
    , lossfactor(ic, Element::class_name, id(), std::move(line().lossfactor))
    , reactance(ic, Element::class_name, id(), std::move(line().reactance))
    , voltage(ic, Element::class_name, id(), std::move(line().voltage))
    , resistance(ic, Element::class_name, id(), std::move(line().resistance))
    , tap_ratio(ic, Element::class_name, id(), std::move(line().tap_ratio))
    , phase_shift_deg(
          ic, Element::class_name, id(), std::move(line().phase_shift_deg))
{
  SPDLOG_DEBUG("LineLP created: uid={} name='{}'", id().first, id().second);
}

namespace
{

// ── Helpers ─────────────────────────────────────────────────────────
//
// Pulled out of `LineLP::add_to_lp` to keep that function focused on
// orchestration.  Tested implicitly via the IEEE benchmark + losses
// equivalence tests in `test_kirchhoff_modes_ieee.cpp`.

/// Resolve `LossConfig` for a (scenario, stage).  Bundles loss-mode
/// resolution + per-stage scalar extraction (lossfactor, R, V, nseg,
/// allocation) so the caller can hand a single value to
/// `line_losses::add_block` per block.  When `opt_capacity` is unset
/// (no expansion) `fmax` is taken from the per-block `tmax_{ab,ba}`
/// schedule maximum so the loss curve covers the actual flow range.
[[nodiscard]] line_losses::LossConfig resolve_loss_config(
    const LineLP& self,
    const Line& raw_line,
    const SystemContext& sc,
    const StageLP& stage,
    std::span<const BlockLP> blocks,
    const OptTBRealSched& tmax_ab,
    const OptTBRealSched& tmax_ba,
    const std::optional<double>& opt_capacity,
    bool has_expansion)
{
  const auto loss_mode =
      line_losses::resolve_mode(raw_line, sc.options(), has_expansion);

  const auto lf = self.param_lossfactor(stage.uid()).value_or(0.0);
  const auto R = self.param_resistance(stage.uid()).value_or(0.0);
  const auto V = self.param_voltage(stage.uid()).value_or(0.0);
  const int nseg = std::max(
      1, raw_line.loss_segments.value_or(sc.options().loss_segments()));

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

  const auto allocation = raw_line.loss_allocation_mode_enum();
  return line_losses::make_config(
      loss_mode, raw_line, allocation, lf, R, V, nseg, fmax);
}

}  // namespace

// ── add_to_lp ───────────────────────────────────────────────────────

bool LineLP::add_to_lp(SystemContext& sc,
                       const ScenarioLP& scenario,
                       const StageLP& stage,
                       LinearProblem& lp)
{
  static constexpr auto ampl_name = Element::class_name.snake_case();

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

  const auto loss_config =
      resolve_loss_config(*this,
                          line(),
                          sc,
                          stage,
                          blocks,
                          tmax_ab,
                          tmax_ba,
                          opt_capacity,
                          /*has_expansion=*/capacity_col.has_value());

  // Per-block flow / capacity / loss / segment col maps assembled
  // here; moved into the persistent `STBIndexHolder` members at the
  // end so the cycle_basis post-pass and the AMPL resolver can read
  // them across scenarios and stages.
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

    // Compress the eight near-identical "if present, store" clauses.
    const auto store_opt = [&](auto& dst, const auto& src)
    {
      if (src) {
        dst[buid] = *src;
      }
    };
    const auto store_vec = [&](auto& dst, auto& src)
    {
      if (!src.empty()) {
        dst[buid] = std::move(src);
      }
    };
    store_opt(fpcols, result.fp_col);
    store_opt(fncols, result.fn_col);
    store_opt(lpcols, result.lossp_col);
    store_opt(lncols, result.lossn_col);
    store_opt(cprows, result.capp_row);
    store_opt(cnrows, result.capn_row);
    store_vec(fpsegcols, result.seg_p_cols);
    store_vec(fnsegcols, result.seg_n_cols);
  }

  // Store all indices for this (scenario, stage) BEFORE emitting KVL
  // rows — the cycle_basis dispatcher reads `flowp_*` / `flown_*` /
  // `*_seg_cols` via the LineLP accessors when it runs at the system
  // level, so the persistent state must be in place by then.
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  capacityp_rows[st_key] = std::move(cprows);
  capacityn_rows[st_key] = std::move(cnrows);
  flowp_cols[st_key] = std::move(fpcols);
  flown_cols[st_key] = std::move(fncols);
  lossp_cols[st_key] = std::move(lpcols);
  lossn_cols[st_key] = std::move(lncols);
  flowp_seg_cols[st_key] = std::move(fpsegcols);
  flown_seg_cols[st_key] = std::move(fnsegcols);

  // ── Kirchhoff KVL rows ────────────────────────────────────────────
  // Dispatch by `kirchhoff_mode`:
  //   * node_angle: emits one KVL row per block here, returns a
  //     per-block `RowIndex` map.
  //   * cycle_basis: returns empty; KVL rows are emitted at the
  //     system level by `kirchhoff::cycle_basis::add_kvl_rows`
  //     after every line has finished creating its flow vars.
  auto trows = kirchhoff::add_line_kvl_rows(sc,
                                            scenario,
                                            stage,
                                            lp,
                                            *this,
                                            bus_a_lp,
                                            bus_b_lp,
                                            flowp_cols.at(st_key),
                                            flown_cols.at(st_key),
                                            flowp_seg_cols.at(st_key),
                                            flown_seg_cols.at(st_key));
  if (!trows.empty()) {
    theta_rows[st_key] = std::move(trows);
  }

  // ── Register PAMPL-visible columns ────────────────────────────────
  // `capainst` is registered centrally by CapacityBase::add_to_lp.
  // The `line.flow` compound (+1·flowp − 1·flown) is registered once
  // per SimulationLP by `system_lp.cpp::register_all_ampl_element_names`
  // (called via std::call_once from the SystemLP constructor).
  const auto register_if_present =
      [&](std::string_view attribute, const auto& cols)
  {
    const auto& m = cols.at(st_key);
    if (!m.empty()) {
      sc.add_ampl_variable(ampl_name, uid(), attribute, scenario, stage, m);
    }
  };
  register_if_present(FlowpName, flowp_cols);
  register_if_present(FlownName, flown_cols);
  register_if_present(LosspName, lossp_cols);
  register_if_present(LossnName, lossn_cols);

  return true;
}

// ── add_to_output ───────────────────────────────────────────────────

bool LineLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
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

  // Kirchhoff rows are emitted in their natural form (no manual pre-
  // scaling), so duals come out in physical units directly and no
  // post-hoc back-scale is needed.  Row-max equilibration is handled
  // by the LP layer, which auto-unscales duals.  In `cycle_basis`
  // mode `theta_rows` is empty (no per-line KVL), so this is a no-op.
  out.add_row_dual(cname, ThetaName, pid, theta_rows);

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
