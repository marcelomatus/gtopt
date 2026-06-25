#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

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
    , in_service(ic, Element::class_name, id(), std::move(line().in_service))
    , tmax_normal_ba(
          ic, Element::class_name, id(), std::move(line().tmax_normal_ba))
    , tmax_normal_ab(
          ic, Element::class_name, id(), std::move(line().tmax_normal_ab))
    , tcost(ic, Element::class_name, id(), std::move(line().tcost))
    , overload_penalty(
          ic, Element::class_name, id(), std::move(line().overload_penalty))
    , lossfactor(ic, Element::class_name, id(), std::move(line().lossfactor))
    , reactance(ic, Element::class_name, id(), std::move(line().reactance))
    , voltage(ic, Element::class_name, id(), std::move(line().voltage))
    , resistance(ic, Element::class_name, id(), std::move(line().resistance))
    , loss_envelope(
          ic, Element::class_name, id(), std::move(line().loss_envelope))
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

  // `lossfactor` is now per-(stage, block) (PR-B).  The `LossConfig`
  // structure is built once per (line, stage) and reused across
  // blocks for performance — sample at the FIRST block of the stage
  // as the representative value.  Scalar schedules broadcast
  // uniformly so this matches the previous semantics exactly.  True
  // per-block lossfactor schedules see only the first block's value
  // on the loss-link rows; full per-block lossfactor on those rows
  // is a follow-up requiring `line_losses::add_block` to accept a
  // block-overridable `lf` parameter.
  const auto first_buid = blocks.empty() ? BlockUid {} : blocks.front().uid();
  const auto lf = self.param_lossfactor(stage.uid(), first_buid).value_or(0.0);
  const auto R = self.param_resistance(stage.uid()).value_or(0.0);
  const auto V = self.param_voltage(stage.uid()).value_or(0.0);
  // Pass ``loss_segments`` through verbatim (no ``max(1, …)`` clamp).
  // ``nseg = 0`` is a legitimate "no PWL" input; ``line_losses::
  // make_config`` rewrites the mode to ``none`` so the dispatch
  // produces a lossless LP (matches the per-line invariant tested in
  // ``test_line_losses_decoupled_envelope.cpp``).  ``nseg = 1`` keeps
  // its existing fallback-to-linear behaviour inside ``make_config``.
  const int nseg =
      raw_line.loss_segments.value_or(sc.options().loss_segments());

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

  // Loss-PWL envelope (decoupled from the flow cap).  When the line
  // carries an explicit ``loss_envelope`` (e.g. the ORIGINAL rating of a
  // soft-cap / ``enforce_level``-lifted line), the K loss segments are
  // spread over THAT range instead of the (possibly inflated) ``fmax``
  // flow cap — concentrating resolution in the realistic loading band.
  // Falls back to ``fmax`` when unset (backward compatible).  Flow past
  // the envelope extrapolates on the last segment's slope.
  // Pass the EXPLICIT per-line envelope only (0 when unset) so
  // ``make_config`` falls back to ``fmax`` internally — passing the
  // fmax-fallback here would mark every line as "decoupled" and release
  // the per-segment caps, breaking the legacy flow-cap-anchored model.
  const auto loss_env = self.param_loss_envelope(stage.uid());
  const double explicit_envelope =
      (loss_env && *loss_env > 0.0) ? *loss_env : 0.0;

  const auto allocation = raw_line.loss_allocation_mode_enum();
  // Resolve the per-MWh loss-column ε cost: per-line override (when set)
  // beats the global ``model_options.loss_cost_eps`` default.  ``0.0``
  // preserves legacy behaviour — no extra cost on loss columns.
  const double loss_cost_eps =
      raw_line.loss_cost_eps.value_or(sc.options().loss_cost_eps());

  // L-secant chord controls (issue #504): per-line override wins,
  // falling back to the global default.  ``loss_use_sos2 = true``
  // with ``loss_secant_segments <= 1`` is sanitized inside
  // ``make_config`` (collapses to single-secant, no SOS2) so we
  // forward the raw inputs here.
  const int nseg_secant = raw_line.loss_secant_segments.value_or(
      sc.options().loss_secant_segments());
  const bool use_sos2 =
      raw_line.loss_use_sos2.value_or(sc.options().loss_use_sos2());

  return line_losses::make_config(loss_mode,
                                  raw_line,
                                  allocation,
                                  lf,
                                  R,
                                  V,
                                  nseg,
                                  fmax,
                                  sc.options().scale_loss_link(),
                                  explicit_envelope,
                                  loss_cost_eps,
                                  nseg_secant,
                                  use_sos2);
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
  // ``tcost`` is per-(stage, block) since 2026-05-18 — read inside the
  // block loop below.  Kept here only as the per-stage-availability
  // sentinel (loss_config still wants a stage-scoped number; resolve
  // at the first block for that path).
  const auto stage_tcost =
      tcost.at(stage.uid(), blocks.front().uid()).value_or(0.0);
  // Soft-cap "feature enabled" gate: when the schedule is unset
  // anywhere, the whole soft-cap path is skipped to preserve the
  // pure hard-cap LP shape.  Per-block resolution happens inside
  // the block loop below — ``overload_penalty`` is per-(stage,
  // block) so time-varying penalties bind correctly.
  const bool soft_cap_enabled = overload_penalty.has_value();

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
  // Per-direction linear loss factors, parallel to `fpcols` / `fncols`.
  // Populated only under `LineLossesMode::linear` (non-zero
  // `result.fp_loss` / `fn_loss`); stay empty for every other mode so
  // the linear branch in `add_to_output` is a no-op there.
  BIndexHolder<double> fplossfac;
  BIndexHolder<double> fnlossfac;
  BIndexHolder<std::vector<ColIndex>> fpsegcols;
  BIndexHolder<std::vector<ColIndex>> fnsegcols;
  // Per-segment physical loss factors `lf_k`, parallel to the
  // `fpsegcols` / `fnsegcols` column vectors (piecewise_direct only).
  BIndexHolder<std::vector<double>> fpsegloss;
  BIndexHolder<std::vector<double>> fnsegloss;
  /// Per-block signed-flow column (populated only under
  /// ``LineLossesMode::tangent_signed_flow``).  When populated, the
  /// directional ``fpcols`` / ``fncols`` maps are empty for this
  /// block — KVL will stamp ``+x_τ`` directly on the signed column.
  BIndexHolder<ColIndex> fscols;
  // Soft-cap (overload) slack columns + rows.  Only populated when
  // the feature is enabled AND the per-block threshold is strictly
  // below the per-block hard cap.  Kept empty otherwise — the
  // `cond_assign` pattern below skips the outer (s,t) insert so
  // `add_to_output` stays a no-op for hard-cap-only lines.
  BIndexHolder<ColIndex> opcols;
  BIndexHolder<ColIndex> oncols;
  BIndexHolder<RowIndex> oprows;
  BIndexHolder<RowIndex> onrows;
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
  map_reserve(fplossfac, blocks.size());
  map_reserve(fnlossfac, blocks.size());
  map_reserve(fpsegcols, blocks.size());
  map_reserve(fnsegcols, blocks.size());
  map_reserve(fpsegloss, blocks.size());
  map_reserve(fnsegloss, blocks.size());
  map_reserve(fscols, blocks.size());
  if (soft_cap_enabled) {
    map_reserve(opcols, blocks.size());
    map_reserve(oncols, blocks.size());
    map_reserve(oprows, blocks.size());
    map_reserve(onrows, blocks.size());
  }

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    // Per-block in-service flag (PLEXOS ``Line.Units`` / ``Lin_Units.csv``).
    // A block resolving to ``0`` (False) means the line is out for that
    // block (maintenance / forced outage): emit nothing — no flow column,
    // capacity row, loss segment, balance contribution, or (downstream)
    // KVL row.  The kirchhoff pass skips blocks with no flow column, so
    // the two buses are genuinely decoupled — a true open circuit, not a
    // ``tmax=0`` short (which would instead pin θ_a = θ_b).  Absent
    // schedule ⇒ in service.
    if (in_service.at(stage.uid(), buid).value_or(True) == False) {
      continue;
    }
    auto& brow_a = lp.row_at(balance_rows_a.at(buid));
    auto& brow_b = lp.row_at(balance_rows_b.at(buid));

    // Both ``tmax_ab`` and ``tmax_ba`` are MAGNITUDES of the forward
    // and reverse maximum flow respectively (both ≥ 0 by convention).
    // Reading them via ``block_maxmin_at`` was a misuse — that helper
    // treats its 4th argument as a ``min`` slot whose default also
    // serves as a floor (``max(capacity_min, lmin_at)``), with the
    // pre-existing call passing ``-stage_capacity``.  That made
    // ``block_tmax_ba`` default to ``-DblMax`` for any line without
    // explicit reverse capacity, which then propagated into
    // ``line_losses.cpp:370`` as ``flowp.lowb = -block_tmax_ba = +DblMax``,
    // a positive lower bound on a non-negative flow variable.  After
    // Ruiz equilibration that landed at ``flowp >= 1.07e+20`` and made
    // every multi-bus run with at least one unconstrained line
    // (``enforce_limits=0`` in PLEXOS, ``tmax_ab/tmax_ba`` unset in
    // the JSON) trip CPLEX presolve infeasibility.  Discovered while
    // wiring the CEN PCP daily case (DATOS20260422) end-to-end.
    const auto block_tmax_ab =
        tmax_ab.at(stage.uid(), block.uid()).value_or(stage_capacity);
    const auto block_tmax_ba =
        tmax_ba.at(stage.uid(), block.uid()).value_or(stage_capacity);
    // ``tcost`` is per-(stage, block); read its block-specific value
    // and let CostHelper apply the per-block duration scaling.
    const auto block_tcost_phys =
        tcost.at(stage.uid(), block.uid()).value_or(stage_tcost);
    const auto block_tcost =
        CostHelper::block_ecost(scenario, stage, block, block_tcost_phys);

    // The flow cap is ALWAYS enforced (``enforce_capacity = true``).
    //
    // ``Line.enforce_level`` (added 2026-05-22, ``feat(line): Line.
    // enforce_level``) was a short-lived attempt to mirror PLEXOS
    // ``Enforce Limits`` (0 = never, 1 = post-solve, 2 = always) by
    // passing ``enforce_capacity = false`` for level 0, which freed the
    // directional flow columns AND relaxed the per-segment upper bounds to
    // ``DblMax`` while the loss coefficients kept using the REAL
    // ``seg_width``.  That half-relaxed state is ill-posed for the loss
    // model (a free flow column with finite loss-row coefficients) and
    // leaves a genuinely FREE LP column — the dominant source of free
    // variables, which defeats GPU first-order / heuristic solvers (cuOpt
    // / PDLP cannot project an unbounded column).
    //
    // It is also obsolete: plexos2gtopt no longer relies on a gtopt-side
    // ``enforce_level`` toggle — it expresses every limit directly as
    // ``tmax`` (hard cap) + ``tmax_normal`` (soft band).  Passing
    // ``enforce_capacity = true`` here simply RESTORES the original
    // pre-2026-05-22 behaviour: every flow column is bound by
    // ``block_tmax`` (= ``tmax`` if set, else the capacity ceiling), and a
    // flow column is NEVER left free.  Two representations, both covered:
    //   - signed flow (``add_none``'s ``flowp``, ``add_tangent_signed_flow``):
    //     a single column over ``[−tmax_ba, +tmax_ab]`` — free on BOTH
    //     sides unless bounded, so both bounds are set here.
    //   - ``flowp`` / ``flown`` (linear / piecewise / bidirectional): two
    //     columns already pinned ``lowb = 0``, so only the ``tmax`` upper
    //     bound is added.
    // The overload columns still apply the soft ``tmax_normal → tmax``
    // penalty, and capacity-*expansion* lines additionally carry the
    // installed-capacity row.  ``Line.enforce_level`` is retained in the
    // schema for backward compatibility but is now a no-op.
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
                                         uid(),
                                         /*enforce_capacity=*/true);

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
    // Linear-mode loss factors (non-zero only under `add_linear`).
    // Stored alongside the aggregator flow cols so `add_to_output` can
    // reconstruct `loss = lossfactor · primal(flow)`.  Gated on a
    // non-zero factor so non-linear modes (which leave `fp_loss` /
    // `fn_loss` at 0) never populate these maps.
    if (result.fp_col && result.fp_loss != 0.0) {
      fplossfac[buid] = result.fp_loss;
    }
    if (result.fn_col && result.fn_loss != 0.0) {
      fnlossfac[buid] = result.fn_loss;
    }
    store_opt(lpcols, result.lossp_col);
    store_opt(lncols, result.lossn_col);
    store_opt(cprows, result.capp_row);
    store_opt(cnrows, result.capn_row);
    store_vec(fpsegcols, result.seg_p_cols);
    store_vec(fnsegcols, result.seg_n_cols);
    store_vec(fpsegloss, result.seg_p_loss);
    store_vec(fnsegloss, result.seg_n_loss);
    store_opt(fscols, result.flow_col);

    // ── Soft cap / overload penalty ────────────────────────────────
    // For each direction with a flow aggregator (fp_col / fn_col),
    // when a per-block soft threshold `tmax_normal_*` is set strictly
    // below the corresponding hard cap, emit a non-negative slack
    // column `overload{p,n}` bounded by `(hard − soft)` and a row
    //   `flow{p,n} − overload{p,n} ≤ tmax_normal_*`
    // so the LP only pays the penalty for MW above the soft cap and
    // can never exceed the hard cap (the existing flow-col upper
    // bound still binds).  Disabled for `piecewise_direct` mode
    // because there is no aggregator column to constrain.
    if (soft_cap_enabled) {
      const auto block_ctx =
          make_block_context(scenario.uid(), stage.uid(), block.uid());
      // Per-(stage, block) penalty resolution.  When the schedule does
      // not yield a value at this (s, b) — e.g. file-backed loader
      // produced an empty cell — fall back to 0 and skip emission.
      const auto block_overload_penalty =
          overload_penalty.at(stage.uid(), block.uid()).value_or(0.0);
      if (block_overload_penalty <= 0.0) {
        continue;
      }
      const auto block_overload_cost = CostHelper::block_ecost(
          scenario, stage, block, block_overload_penalty);

      const auto add_soft_cap_direction =
          [&](std::optional<ColIndex> flow_col,
              const OptTBRealSched& tmax_normal_sched,
              double block_tmax_hard,
              std::string_view ov_name,
              auto& ov_cols_dst,
              auto& ov_rows_dst,
              std::string_view row_constraint_name)
      {
        if (!flow_col) {
          return;
        }
        const auto soft_at = tmax_normal_sched.at(stage.uid(), buid);
        if (!soft_at) {
          return;
        }
        const double block_tmax_soft = *soft_at;
        // Skip when soft cap is not strict relative to hard cap —
        // an equal or larger threshold makes the slack inert.
        if (!(block_tmax_soft < block_tmax_hard)) {
          return;
        }
        const double slack_uppb = block_tmax_hard - block_tmax_soft;

        const auto ov_col = lp.add_col({
            .lowb = 0.0,
            .uppb = slack_uppb,
            .cost = block_overload_cost,
            .class_name = Element::class_name.full_name(),
            .variable_name = ov_name,
            .variable_uid = uid(),
            .context = block_ctx,
        });
        ov_cols_dst[buid] = ov_col;

        auto ov_row = SparseRow {
            .class_name = Element::class_name.full_name(),
            .constraint_name = row_constraint_name,
            .variable_uid = uid(),
            .context = block_ctx,
        };
        ov_row[*flow_col] = 1.0;
        ov_row[ov_col] = -1.0;
        ov_rows_dst[buid] =
            lp.add_row(std::move(ov_row.less_equal(block_tmax_soft)));
      };

      add_soft_cap_direction(result.fp_col,
                             tmax_normal_ab,
                             block_tmax_ab,
                             OverloadpName,
                             opcols,
                             oprows,
                             OverloadpName);
      add_soft_cap_direction(result.fn_col,
                             tmax_normal_ba,
                             block_tmax_ba,
                             OverloadnName,
                             oncols,
                             onrows,
                             OverloadnName);
    }
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
  cond_assign(flowp_lossfactor, std::move(fplossfac));
  cond_assign(flown_lossfactor, std::move(fnlossfac));
  cond_assign(flows_cols, std::move(fscols));
  cond_assign(lossp_cols, std::move(lpcols));
  cond_assign(lossn_cols, std::move(lncols));
  cond_assign(flowp_seg_cols, std::move(fpsegcols));
  cond_assign(flown_seg_cols, std::move(fnsegcols));
  cond_assign(flowp_seg_loss, std::move(fpsegloss));
  cond_assign(flown_seg_loss, std::move(fnsegloss));
  cond_assign(overloadp_cols, std::move(opcols));
  cond_assign(overloadn_cols, std::move(oncols));
  cond_assign(overloadp_rows, std::move(oprows));
  cond_assign(overloadn_rows, std::move(onrows));

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
                                            flown_seg_cols_at(scenario, stage),
                                            flows_cols_at(scenario, stage));
  if (!trows.empty()) {
    theta_rows[st_key] = std::move(trows);
  }

  // ── Register PAMPL-visible columns ────────────────────────────────
  // `capainst` is registered centrally by CapacityBase::add_to_lp.
  //
  // Only `line.flow`, `line.loss` and the overload slacks are exposed to
  // user constraints.  The per-direction `flowp` / `flown` / `flows`
  // columns are internal LP decompositions (two-variable flow split,
  // piecewise-direct segments, or the tangent-signed single column) and
  // are NOT registered as AMPL attributes — `flow` below folds them into
  // one signed weighted sum.
  //
  // `register_if_present` registers a single-col / sum-of-cols holder
  // under `attribute`, ADL-picking the `add_ampl_variable` overload by
  // `it->second`'s value type.  Look up via `.find()` and silently no-op
  // when the outer (s,t) key is absent — equivalent to "no LP cols here".
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
  // ── line.flow — the ONLY AMPL-exposed flow attribute ──────────────
  // The signed physical flow, registered DIRECTLY as a weighted sum of
  // the underlying direction columns instead of as a resolve-time
  // compound: +flowp − flown (single-col), +Σflowp_seg − Σflown_seg
  // (piecewise_direct), or +flows (tangent_signed_flow).  flowp / flown
  // / flows are internal LP decompositions and are NOT exposed to user
  // constraints — `line("X").flow` is the only addressable flow.  This
  // single weighted-sum entry replaces the former three per-direction
  // registrations plus the class-level `flow = +flowp − flown` compound.
  BIndexHolder<std::vector<std::pair<ColIndex, double>>> flow_weighted;
  const auto accumulate_flow = [&](const auto& cols, double sign)
  {
    const auto it = cols.find(st_key);
    if (it == cols.end()) {
      return;
    }
    for (const auto& [buid, held] : it->second) {
      if constexpr (std::is_same_v<std::decay_t<decltype(held)>, ColIndex>) {
        flow_weighted[buid].emplace_back(held, sign);
      } else {  // BIndexHolder<std::vector<ColIndex>> — piecewise_direct
        for (const auto& col : held) {
          flow_weighted[buid].emplace_back(col, sign);
        }
      }
    }
  };
  accumulate_flow(flowp_cols, +1.0);
  accumulate_flow(flown_cols, -1.0);
  accumulate_flow(flowp_seg_cols, +1.0);
  accumulate_flow(flown_seg_cols, -1.0);
  accumulate_flow(flows_cols, +1.0);
  if (!flow_weighted.empty()) {
    sc.add_ampl_variable(
        ampl_name, uid(), FlowName, scenario, stage, flow_weighted);
  }

  register_if_present(LosspName, lossp_cols);
  register_if_present(LossnName, lossn_cols);
  register_if_present(OverloadpName, overloadp_cols);
  register_if_present(OverloadnName, overloadn_cols);

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
  // Unified line-flow emission — ONE signed primal + ONE signed rc.
  //
  // Output stems: ``Line/flow_sol.parquet`` and ``Line/flow_cost.parquet``.
  // Convention (industry-aligned, PLEXOS / SDDP / GenX):
  //   * primal: ``flow = primal(flowp) − primal(flown)`` for two-column
  //     modes (bidirectional / linear / piecewise / piecewise_direct),
  //     or ``flow = primal(flows)`` for ``tangent_signed_flow`` mode.
  //     Signed scalar: positive ⇒ A→B direction, negative ⇒ B→A.
  //   * reduced cost: sign-based pick of the active direction's rc:
  //         flow > 0  ⇒ flow_cost = +rc[flowp]   (A→B active)
  //         flow < 0  ⇒ flow_cost = −rc[flown]   (B→A active)
  //         flow = 0  ⇒ flow_cost = 0            (line idle)
  //     For the single-column ``tangent_signed_flow`` mode, the LP
  //     rc on the signed flows column already encodes both bounds
  //     in one signed scalar — pass through verbatim.
  //
  // Drops the previously-emitted per-direction
  // ``flowp_sol`` / ``flown_sol`` stems (and their cost counterparts)
  // in favour of one signed ``flow`` stem — matches PLEXOS' "Net Flow",
  // SDDP's intercâmbio, and the convention every other gtopt
  // consumer (gtopt_check_output, gtopt_results_summary,
  // gtopt_marginal_units, plexos2gtopt/{compare_with_plexos,
  // auto_lift_lines}) wants once they're updated to read the new
  // shape: a single signed column instead of the two non-negative
  // pieces the LP happens to use internally.
  //
  // ──  ``extras:Line`` opt-in side-stream ────────────────────────────
  //
  // When the writer opts into ``--write-out ...,extras:Line``, two
  // additional stems are emitted alongside ``flow_*``:
  //   * ``flown_sol``   = ``primal(flown)`` (always ≥ 0) — the raw
  //     negative-direction primal; consumers that want the directional
  //     split can recover ``flowp = flow + flown``.
  //   * ``flown_cost``  = the reduced cost the SIGN-BASED rule did NOT
  //     pick for ``flow_cost``.  When ``flow > 0`` we picked
  //     ``rc[flowp]`` → ``flown_cost = rc[flown]``.  When ``flow < 0``
  //     we picked ``−rc[flown]`` → ``flown_cost = rc[flowp]``.  When
  //     ``flow = 0`` the LP is idle so both rcs are emitted as zero in
  //     the main stream; ``flown_cost`` carries ``rc[flown]``.
  STBIndexHolder<double> flow_signed_sol;
  STBIndexHolder<double> flow_signed_cost;
  STBIndexHolder<double> flown_extras_sol;
  STBIndexHolder<double> flown_extras_cost;

  const auto fill_two_column = [&](const STBIndexHolder<ColIndex>& fp_holder,
                                   const STBIndexHolder<ColIndex>& fn_holder)
  {
    for (const auto& [st_key, p_blocks] : fp_holder) {
      auto& dst_p = flow_signed_sol[st_key];
      auto& dst_c = flow_signed_cost[st_key];
      const auto it_n = fn_holder.find(st_key);
      const bool has_flown_st = (it_n != fn_holder.end());
      const auto* n_blocks = has_flown_st ? &it_n->second : nullptr;
      // Only allocate the flown extras buckets when there is an actual
      // flown column at this (scenario, stage) — the LP doesn't always
      // emit one (e.g. ``tangent_signed_flow`` skips two-column setup
      // entirely; ``add_none`` has a single signed flowp).  Empty extras
      // STBIndexHolders short-circuit the emit call at the bottom.
      auto* dst_fn_p = has_flown_st ? &flown_extras_sol[st_key] : nullptr;
      auto* dst_fn_c = has_flown_st ? &flown_extras_cost[st_key] : nullptr;
      for (const auto& [buid, pcol] : p_blocks) {
        const double fp = out.primal(pcol);
        const double rcp = out.cost(pcol);
        double fn = 0.0;
        double rcn = 0.0;
        bool has_flown_cell = false;
        if (n_blocks != nullptr) {
          const auto it_b = n_blocks->find(buid);
          if (it_b != n_blocks->end()) {
            fn = out.primal(it_b->second);
            rcn = out.cost(it_b->second);
            has_flown_cell = true;
          }
        }
        const double signed_flow = fp - fn;
        dst_p[buid] = signed_flow;
        // Sign-based combined rc.  Equivalent to "rc of the active
        // LP column"; the inactive direction contributes 0 because
        // its primal is 0 and its rc tells us only about its OWN
        // lower-bound dual (irrelevant for the saturation question
        // on the SIGNED flow).
        if (signed_flow > 0.0) {
          dst_c[buid] = rcp;
        } else if (signed_flow < 0.0) {
          dst_c[buid] = -rcn;
        } else {
          dst_c[buid] = 0.0;
        }
        // Extras: only when this (line, cell) actually has a flown
        // column.  ``flown_extras_cost`` carries the NOT-selected rc.
        if (has_flown_cell && dst_fn_p != nullptr) {
          (*dst_fn_p)[buid] = fn;
          // The NOT-selected direction's rc: reverse flow active
          // (``signed_flow < 0``) selects flown, so the unselected is
          // flowp → ``rcp``; otherwise (forward active OR zero flow) the
          // unselected is flown → ``rcn``.
          (*dst_fn_c)[buid] = signed_flow < 0.0 ? rcp : rcn;
        }
      }
    }
  };

  // Aggregator-mode (none / linear / piecewise / bidirectional):
  // single column per direction in ``flowp_cols`` / ``flown_cols``.
  fill_two_column(flowp_cols, flown_cols);

  // ``tangent_signed_flow``: one signed LP column in ``flows_cols``
  // (``lowb = -tmax_ba``, ``uppb = +tmax_ab``).  Primal and rc pass
  // straight through; ``rc[flows] < 0`` ⇔ at the positive cap,
  // ``rc[flows] > 0`` ⇔ at the negative cap.
  for (const auto& [st_key, s_blocks] : flows_cols) {
    auto& dst_p = flow_signed_sol[st_key];
    auto& dst_c = flow_signed_cost[st_key];
    for (const auto& [buid, scol] : s_blocks) {
      dst_p[buid] = out.primal(scol);
      dst_c[buid] = out.cost(scol);
    }
  }

  // ``piecewise_direct`` mode: K per-segment columns per direction,
  // signed flow is ``Σ seg_p − Σ seg_n``.  The same sign-based combined
  // rc rule applies — but reduced cost on a per-segment kink is the
  // segment slope's gap and not a single scalar per (line, block);
  // for the saturation signal we use the smallest |rc| across the
  // segments in the active direction.
  const auto fill_segments_signed_flow = [&]()
  {
    if (flowp_seg_cols.empty() && flown_seg_cols.empty()) {
      return;
    }
    auto sum_segs = [&](const STBIndexHolder<std::vector<ColIndex>>& holder,
                        std::tuple<ScenarioUid, StageUid> st_key,
                        BlockUid buid) -> std::pair<double, double>
    {
      double f = 0.0;
      double rc_min_abs = std::numeric_limits<double>::infinity();
      const auto it = holder.find(st_key);
      if (it == holder.end()) {
        return {0.0, 0.0};
      }
      const auto& blocks = it->second;
      const auto it_b = blocks.find(buid);
      if (it_b == blocks.end()) {
        return {0.0, 0.0};
      }
      for (const auto& col : it_b->second) {
        f += out.primal(col);
        rc_min_abs = std::min(rc_min_abs, std::abs(out.cost(col)));
      }
      if (!std::isfinite(rc_min_abs)) {
        rc_min_abs = 0.0;
      }
      return {f, rc_min_abs};
    };

    std::set<std::tuple<ScenarioUid, StageUid>> st_keys;
    for (const auto& [k, _] : flowp_seg_cols) {
      st_keys.insert(k);
    }
    for (const auto& [k, _] : flown_seg_cols) {
      st_keys.insert(k);
    }
    for (const auto& st_key : st_keys) {
      std::set<BlockUid> buids;
      if (auto it = flowp_seg_cols.find(st_key); it != flowp_seg_cols.end()) {
        for (const auto& [b, _] : it->second) {
          buids.insert(b);
        }
      }
      if (auto it = flown_seg_cols.find(st_key); it != flown_seg_cols.end()) {
        for (const auto& [b, _] : it->second) {
          buids.insert(b);
        }
      }
      auto& dst_p = flow_signed_sol[st_key];
      auto& dst_c = flow_signed_cost[st_key];
      for (const auto buid : buids) {
        const auto [fp, rcp_abs] = sum_segs(flowp_seg_cols, st_key, buid);
        const auto [fn, rcn_abs] = sum_segs(flown_seg_cols, st_key, buid);
        const double signed_flow = fp - fn;
        dst_p[buid] = signed_flow;
        if (signed_flow > 0.0) {
          dst_c[buid] = -rcp_abs;
        } else if (signed_flow < 0.0) {
          dst_c[buid] = +rcn_abs;
        } else {
          dst_c[buid] = 0.0;
        }
      }
    }
  };
  fill_segments_signed_flow();

  out.add_col_sol_values(cname, FlowName, pid, flow_signed_sol);
  out.add_col_cost_values(cname, FlowName, pid, flow_signed_cost);
  // Extras (opt-in via ``--write-out extras`` or ``--write-out all``).
  // The unified `Line/flow_sol.parquet` (signed: + = A→B, − = B→A)
  // carries the full direction information; these directional raw
  // streams are useful only for LP-debug / arbitrage diagnostics, so
  // they ride the `extras` gate (off in the default `sol,dual,rc`).
  //   * ``flown_sol``  = raw flown primal (always ≥ 0); ``flowp = flow +
  //                       flown``
  //   * ``flowe_cost`` = EXCLUDED reduced cost (the rc that the sign-based
  //                      ``flow_cost`` rule did NOT pick).  Named ``flowe``
  //                      not ``flown`` so the suffix reads as "excluded",
  //                      not "negative direction".
  out.add_col_sol_values_extras(cname, FlownName, pid, flown_extras_sol);
  out.add_col_cost_values_extras(cname, FloweName, pid, flown_extras_cost);

  // Consolidated loss output: piecewise / bidirectional only.  none /
  // linear / piecewise_direct don't create loss vars so this is a no-op.
  // The LP holds losses in two per-direction column sets (``lossp_cols``
  // for A→B, ``lossn_cols`` for B→A); on any given block at most one is
  // populated under the arbitrage-free PWL modes (``adaptive`` /
  // ``piecewise`` / ``bidirectional``).  Merge them per cell and emit
  // a single ``Line/loss_sol.parquet`` whose per-(line, block) value
  // is ``LP(lossp) + LP(lossn)`` — total dissipated energy regardless
  // of direction.  Emitted under the same ``sol`` gate as ``flow_sol``
  // so any default ``write_out`` that requests Line solutions also
  // gets the loss stream — required for post-solve loss-vs-flow audits
  // in ``gtopt_check_output``.
  //
  // The directional split is OPT-IN via the `extras` gate:
  // `Line/lossn_sol.parquet` carries the B→A leg.  Useful for
  // arbitrage diagnostics; the A→B leg is implicitly `loss − lossn`.
  // PAMPL UCs that want the per-block total still reference the
  // class-level compound `line.loss = +lossp + lossn`.
  if (!lossp_cols.empty() || !lossn_cols.empty()) {
    STBIndexHolder<std::vector<ColIndex>> loss_combined;
    auto merge_in = [&loss_combined](const auto& src)
    {
      for (const auto& [st_key, blocks] : src) {
        auto& dst_blocks = loss_combined[st_key];
        for (const auto& [buid, col] : blocks) {
          dst_blocks[buid].push_back(col);
        }
      }
    };
    merge_in(lossp_cols);
    merge_in(lossn_cols);
    out.add_col_sol(cname, LossName, pid, loss_combined);
  } else if (!flowp_seg_cols.empty() || !flown_seg_cols.empty()) {
    // ``piecewise_direct`` mode: no explicit loss LP column.  The loss
    // is stamped per-segment into the bus-balance rows via the factors
    // ``lf_k`` (saved in ``flowp_seg_loss`` / ``flown_seg_loss``,
    // parallel to ``flowp_seg_cols`` / ``flown_seg_cols``).  Reconstruct
    // the EXACT LP-consistent loss
    //   ``Σ_k lf_k · primal(seg_col_k)``
    // summed over BOTH directional segment sets.  Both directions are
    // summed (not netted) so the value captures loss-arbitrage cases
    // where positive- AND negative-direction segments are simultaneously
    // non-zero — a ``R·f²/V²``-at-net-flow approximation would hide that
    // and under-report pathological losses.  Emitted on the same
    // ``solution`` gate as the PWL-mode ``loss_sol`` (``add_col_sol_values``
    // applies the identity ``sol`` factor pipeline, so the on-disk shape
    // is bit-for-bit identical to the explicit-column form).  The
    // ``else`` guarantees we never double-emit: explicit loss columns and
    // direct segments are mutually exclusive per line.
    STBIndexHolder<double> loss_values;
    const auto accumulate_dir =
        [&](const STBIndexHolder<std::vector<ColIndex>>& cols_holder,
            const STBIndexHolder<std::vector<double>>& loss_holder)
    {
      for (const auto& [st_key, blocks] : cols_holder) {
        const auto lit = loss_holder.find(st_key);
        if (lit == loss_holder.end()) {
          continue;
        }
        auto& dst_blocks = loss_values[st_key];
        for (const auto& [buid, seg_cols] : blocks) {
          const auto lbit = lit->second.find(buid);
          if (lbit == lit->second.end()) {
            continue;
          }
          const auto& lf = lbit->second;
          double cell_loss = 0.0;
          const auto n = std::min(seg_cols.size(), lf.size());
          for (std::size_t k = 0; k < n; ++k) {
            cell_loss += lf[k] * out.primal(seg_cols[k]);
          }
          dst_blocks[buid] += cell_loss;
        }
      }
    };
    accumulate_dir(flowp_seg_cols, flowp_seg_loss);
    accumulate_dir(flown_seg_cols, flown_seg_loss);
    out.add_col_sol_values(cname, LossName, pid, loss_values);
  } else if (!flowp_lossfactor.empty() || !flown_lossfactor.empty()) {
    // ``linear`` mode: no explicit loss LP column and no per-segment
    // cols.  The line's loss is the linear factor ``config.lossfactor``
    // applied to the directional aggregator flow cols (stamped into the
    // bus balance via ``apply_linear_allocation``), captured per cell in
    // ``flowp_lossfactor`` / ``flown_lossfactor`` parallel to
    // ``flowp_cols`` / ``flown_cols``.  Reconstruct the LP-consistent
    // per-(line, block) loss
    //   ``flowp_lossfactor · primal(flowp) + flown_lossfactor · primal(flown)``
    // summed over BOTH directions (not netted) so simultaneous bi-
    // directional flow is captured rather than hidden.  Emitted on the
    // same ``solution`` gate as the other modes' ``loss_sol`` via
    // ``add_col_sol_values`` (identity ``sol`` factor pipeline → bit-
    // identical on-disk shape).  Reached only when branches 1 (explicit
    // loss cols) and 2 (piecewise_direct seg cols) did NOT fire — the
    // ``else if`` chain guarantees no double-emit; the non-empty
    // lossfactor-holder guard additionally makes this a no-op for any
    // other mode that happens to populate ``flowp_cols`` but not the
    // lossfactor maps (those modes leave ``fp_loss`` / ``fn_loss`` at 0).
    STBIndexHolder<double> loss_values;
    const auto accumulate_linear_dir =
        [&](const STBIndexHolder<ColIndex>& cols_holder,
            const STBIndexHolder<double>& loss_holder)
    {
      for (const auto& [st_key, blocks] : cols_holder) {
        const auto lit = loss_holder.find(st_key);
        if (lit == loss_holder.end()) {
          continue;
        }
        auto& dst_blocks = loss_values[st_key];
        for (const auto& [buid, col] : blocks) {
          const auto lbit = lit->second.find(buid);
          if (lbit == lit->second.end()) {
            continue;
          }
          dst_blocks[buid] += lbit->second * out.primal(col);
        }
      }
    };
    accumulate_linear_dir(flowp_cols, flowp_lossfactor);
    accumulate_linear_dir(flown_cols, flown_lossfactor);
    out.add_col_sol_values(cname, LossName, pid, loss_values);
  }
  // Extras-gated per-direction loss (B→A leg).  `lossn_cols` is empty
  // for ``tangent_signed_flow`` / ``none`` / ``linear`` /
  // ``piecewise_direct`` modes; the helper is a no-op in those cases.
  if (!lossn_cols.empty()) {
    out.add_col_sol_extras(cname, LossnName, pid, lossn_cols);
  }

  // Overload-slack solutions and costs: only populated when the
  // soft-cap feature is active for this line (see `add_to_lp`).
  // No-op for lines without `tmax_normal_*` + `overload_penalty`.
  // Used today only by audits that look for "did the LP violate the
  // soft tmax_normal cap?" — `extras`-gated so the default footprint
  // doesn't pay for them on cases where the feature is unused.
  if (!overloadp_cols.empty()) {
    out.add_col_sol_extras(cname, OverloadpName, pid, overloadp_cols);
    out.add_col_cost_extras(cname, OverloadpName, pid, overloadp_cols);
  }
  if (!overloadn_cols.empty()) {
    out.add_col_sol_extras(cname, OverloadnName, pid, overloadn_cols);
    out.add_col_cost_extras(cname, OverloadnName, pid, overloadn_cols);
  }

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
