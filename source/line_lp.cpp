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
  return line_losses::make_config(loss_mode,
                                  raw_line,
                                  allocation,
                                  lf,
                                  R,
                                  V,
                                  nseg,
                                  fmax,
                                  sc.options().scale_loss_link());
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
  // Reserve every per-block holder up-front to avoid rehash churn as
  // the block loop populates each map.  All eight grow at most to
  // `blocks.size()` (one entry per block) so a single reservation
  // each gets us to steady-state capacity in the common case.  The
  // seg holders carry an outer entry per block and an inner segment
  // vector — the inner vector is reserved by `add_direct_direction`
  // via `seg_cols.reserve(nseg)`, so we only need the outer-map
  // reservation here.
  map_reserve(fpcols, blocks.size());
  map_reserve(fncols, blocks.size());
  map_reserve(cprows, blocks.size());
  map_reserve(cnrows, blocks.size());
  map_reserve(lpcols, blocks.size());
  map_reserve(lncols, blocks.size());
  map_reserve(fpsegcols, blocks.size());
  map_reserve(fnsegcols, blocks.size());

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
  //
  // Outer-map assignment is **conditional on a non-empty inner**
  // (2026-05-14).  Pre-fix every (s, t) key was inserted even when
  // the per-block map was empty (e.g., `flowp_seg_cols` for non-
  // direct modes, `flowp_cols` for `piecewise_direct`), making the
  // `STBIndexHolder::empty()` gate in `add_to_output` useless — the
  // outer map always had entries.  The four flow-col accessors in
  // `line_lp.hpp` now return a static empty BIndexHolder when the
  // key is missing, so callers (the kirchhoff dispatcher, the
  // resolver) stay unchanged.
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  const auto cond_assign = [&](auto& dst, auto&& src)
  {
    if (!src.empty()) {
      dst[st_key] = std::forward<decltype(src)>(src);
    }
  };
  cond_assign(capacityp_rows, std::move(cprows));
  cond_assign(capacityn_rows, std::move(cnrows));
  cond_assign(flowp_cols, std::move(fpcols));
  cond_assign(flown_cols, std::move(fncols));
  cond_assign(lossp_cols, std::move(lpcols));
  cond_assign(lossn_cols, std::move(lncols));
  cond_assign(flowp_seg_cols, std::move(fpsegcols));
  cond_assign(flown_seg_cols, std::move(fnsegcols));

  // ── Kirchhoff KVL rows ────────────────────────────────────────────
  // Dispatch by `kirchhoff_mode`:
  //   * node_angle: emits one KVL row per block here, returns a
  //     per-block `RowIndex` map.
  //   * cycle_basis: returns empty; KVL rows are emitted at the
  //     system level by `kirchhoff::cycle_basis::add_kvl_rows`
  //     after every line has finished creating its flow vars.
  //
  // Reads via the `*_cols_at` accessors so the conditional-insert
  // pattern above is transparent — the accessors return a static
  // empty `BIndexHolder<...>` when the outer key was skipped (e.g.,
  // `flowp_seg_cols` for non-direct-mode lines, `flowp_cols` for
  // piecewise_direct lines).  `.at(st_key)` would throw on those
  // missing keys.
  auto trows = kirchhoff::add_line_kvl_rows(sc,
                                            scenario,
                                            stage,
                                            lp,
                                            *this,
                                            bus_a_lp,
                                            bus_b_lp,
                                            flowp_cols_at(scenario, stage),
                                            flown_cols_at(scenario, stage),
                                            flowp_seg_cols_at(scenario, stage),
                                            flown_seg_cols_at(scenario, stage));
  if (!trows.empty()) {
    theta_rows[st_key] = std::move(trows);
  }

  // ── Register PAMPL-visible columns ────────────────────────────────
  // `capainst` is registered centrally by CapacityBase::add_to_lp.
  // The `line.flow` compound (+1·flowp − 1·flown) is registered once
  // per SimulationLP by `system_lp.cpp::register_all_ampl_element_names`
  // (called via std::call_once from the SystemLP constructor).
  //
  // Under `piecewise_direct` line-loss mode there is NO aggregator
  // LP column for `flowp` / `flown` — both decompose into K segment
  // cols stamped directly into the bus rows.  To keep `line.flow`
  // AMPL-resolvable without paying the cost of materialising
  // aggregator cols, the segment-col holders are registered as
  // **sum-of-cols** AMPL attributes (see
  // `AmplVariable::block_cols_sum`).  The resolver then expands
  // `flow = +flowp − flown` to `+Σ flowp_seg_k − Σ flown_seg_k`
  // by stamping each segment col with the leg coefficient.
  // Per-direction registration is independent so a half-direction
  // line (e.g., tmax_ba = 0) registers only its non-empty side.
  // The conditional-insert pattern above means `cols.at(st_key)`
  // would throw on the (s,t) keys we skipped.  Look up via
  // `.find()` and silently no-op when the outer key is absent —
  // semantically equivalent to "no LP cols for this (s,t)".
  // Generic registration: picks the matching `add_ampl_variable`
  // overload by ADL on `it->second`'s value type.
  //   * `BIndexHolder<ColIndex>`               → single-col path
  //     (`flowp_cols`, `flown_cols`, `lossp_cols`, `lossn_cols`).
  //   * `BIndexHolder<std::vector<ColIndex>>`  → sum-of-cols path
  //     used by `piecewise_direct`'s virtual aggregator.
  // Aggregator-mode lines leave `flowp_seg_cols` / `flown_seg_cols`
  // empty, and `piecewise_direct` lines leave the single-col
  // `flowp_cols` / `flown_cols` empty — so each line registers
  // exactly one shape, never both.
  const auto register_if_present =
      [&](std::string_view attribute, const auto& cols)
  {
    const auto it = cols.find(st_key);
    if (it == cols.end() || it->second.empty()) {
      return;
    }
    sc.add_ampl_variable(
        ampl_name, uid(), attribute, scenario, stage, it->second);
  };
  register_if_present(FlowpName, flowp_cols);
  register_if_present(FlownName, flown_cols);
  register_if_present(LosspName, lossp_cols);
  register_if_present(LossnName, lossn_cols);
  register_if_present(FlowpName, flowp_seg_cols);
  register_if_present(FlownName, flown_seg_cols);

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

  // Output policy (2026-05-14):
  //
  //   * Flow solutions and loss solutions are kept — they are the
  //     primary post-solve quantities consumers (analysis notebooks,
  //     PLP-comparison scripts, dashboards) read off lines.
  //   * Flow reduced costs are NOT emitted.  In aggregator modes the
  //     reduced cost of `flowp` / `flown` is recoverable from the
  //     bus-balance duals; in `piecewise_direct` the segment-sum
  //     interpretation of "reduced cost of an implicit aggregator"
  //     is ambiguous (Σ rc(seg_k) ≠ marginal cost of a +ε increase
  //     in |f|).  Better to omit than to emit a misleading scalar.
  //   * Theta-row duals and capacity-row duals are also dropped.
  //     The KVL theta dual is locational-marginal-price adjacent
  //     and easier to read off `Bus.balance:dual`; capacity duals
  //     are recoverable from CapacityBase's `capainst` reduced cost
  //     when expansion is active.  Skipping them saves I/O.
  //
  // Aggregator-mode emission (none / linear / piecewise / bidirectional).
  // No-op for piecewise_direct, where the holders are empty.
  out.add_col_sol(cname, FlowpName, pid, flowp_cols);
  out.add_col_sol(cname, FlownName, pid, flown_cols);

  // Direct-mode emission: under `piecewise_direct` the seg holders
  // carry the K per-segment cols and the sum-of-cols overload writes
  // Σ col_sol[seg_k] under the same `flowp:sol` / `flown:sol` field
  // names.  Outer-level gate avoids paying the flat() walk on the
  // empty per-block maps in non-direct modes (where the outer key
  // exists but every inner BIndexHolder is empty).
  if (!flowp_seg_cols.empty()) {
    out.add_col_sol(cname, FlowpName, pid, flowp_seg_cols);
  }
  if (!flown_seg_cols.empty()) {
    out.add_col_sol(cname, FlownName, pid, flown_seg_cols);
  }

  // Loss solutions: piecewise / bidirectional only.  none / linear /
  // piecewise_direct don't create loss vars so these are no-ops.
  out.add_col_sol(cname, LosspName, pid, lossp_cols);
  out.add_col_sol(cname, LossnName, pid, lossn_cols);

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
