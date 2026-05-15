/**
 * @file      demand_lp.cpp
 * @brief     Implementation of demand LP formulation
 * @date      Thu Mar 27 22:31:31 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements DemandLP construction and add_to_lp, which builds
 * LP variables and constraints for load, curtailment, and energy limits.
 */
#include <gtopt/demand_lp.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

DemandLP::DemandLP(const Demand& pdemand, const InputContext& ic)
    : CapacityBase(pdemand, ic, Element::class_name)
    , lmax(ic, Element::class_name, id(), std::move(object().lmax))
    , lossfactor(ic, Element::class_name, id(), std::move(object().lossfactor))
    , fcost(ic, Element::class_name, id(), std::move(object().fcost))
    , emin(ic, Element::class_name, id(), std::move(object().emin))
    , ecost(ic, Element::class_name, id(), std::move(object().ecost))
{
  SPDLOG_DEBUG("DemandLP created: uid={} name='{}'", id().first, id().second);
}

double DemandLP::fail_sol_at(const ScenarioLP& scenario,
                             const StageLP& stage,
                             const BlockLP& block,
                             const ScaledView& col_sol) const noexcept
{
  // Reconstruct `fail_sol = max(0, lmax − load_sol)` from cached
  // `block_lmax_values_` and the surviving `load_cols` primal value.
  // Pre-P0 callers used `lp.col_sol[fail_col]` for the same value;
  // this method returns the algebraically-equivalent quantity from
  // the post-substitution LP.  See `add_to_output` for the
  // companion bulk-emit site that produces `Demand/fail_sol.csv`.
  const std::tuple st_key {scenario.uid(), stage.uid()};
  const auto lmax_it = block_lmax_values_.find(st_key);
  if (lmax_it == block_lmax_values_.end()) {
    return 0.0;
  }
  const auto lmax_block_it = lmax_it->second.find(block.uid());
  if (lmax_block_it == lmax_it->second.end()) {
    return 0.0;
  }
  const auto lc_it = load_cols.find(st_key);
  if (lc_it == load_cols.end()) {
    return 0.0;
  }
  const auto lc_block_it = lc_it->second.find(block.uid());
  if (lc_block_it == lc_it->second.end()) {
    return 0.0;
  }
  const double load_sol = col_sol[lc_block_it->second];
  return std::max(0.0, lmax_block_it->second - load_sol);
}

bool DemandLP::add_to_lp(SystemContext& sc,
                         const ScenarioLP& scenario,
                         const StageLP& stage,
                         LinearProblem& lp)
{
  static constexpr auto ampl_name = Element::class_name.snake_case();

  if (!CapacityBase::add_to_lp(sc, ampl_name, scenario, stage, lp)) {
    return false;
  }

  // F9: register filter metadata so sum(...) predicates work.
  {
    AmplElementMetadata metadata;
    metadata.reserve(2);
    if (const auto& t = demand().type) {
      metadata.emplace_back(TypeKey, *t);
    }
    // Resolve via `sc.element<BusLP>` (handles both Uid and Name forms
    // of the JSON-side `bus` SingleId variant — `std::get<Uid>` would
    // throw if the JSON used a string name).
    metadata.emplace_back(
        BusKey, static_cast<double>(sc.element<BusLP>(bus_sid()).uid()));
    sc.register_ampl_element_metadata(ampl_name, uid(), std::move(metadata));
  }

  if (!is_active(stage)) [[unlikely]] {
    return true;
  }

  const auto& bus_lp = sc.element<BusLP>(bus_sid());
  if (!bus_lp.is_active(stage)) [[unlikely]] {
    return true;
  }

  const auto [opt_capacity, capacity_col] = capacity_and_col(stage, lp);
  const double stage_capacity = opt_capacity.value_or(LinearProblem::DblMax);
  const auto stage_fcost = sc.demand_fail_cost(stage, fcost);
  const auto stage_lossfactor = lossfactor.optval(stage.uid()).value_or(0.0);

  const auto& bus_balance_rows = bus_lp.balance_rows_at(scenario, stage);
  const auto& blocks = stage.blocks();

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  // adding the minimum energy constraint
  const auto stage_emin = emin.optval(stage.uid());
  auto stage_ecost = ecost.optval(stage.uid());
  if (!stage_ecost && stage_fcost) {
    stage_ecost = stage_fcost;
  }

  // ── emin aggregator collapse ─────────────────────────────────────
  //
  // Pre-collapse: a stage-level `emin_col` (variable) + an equality
  // row `sum_b bdur*mcol[b] − emin_col = 0` glued the per-block
  // `mcol[b]` (lman) variables to one stage aggregator.
  // Post-collapse: the aggregator is gone — `emin_col` and the
  // gluing row are both deleted.  In their place:
  //
  //   * `mcol[b].cost = −c_per_unit × bdur` (soft, ecost set), where
  //     `c_per_unit = scenario_stage_ecost(s, t, stage_ecost /
  //     stage.duration())`. The negative coefficient rewards drawing more
  //     energy.
  //
  //   * A single stage-level row replaces the aggregator-gluing row:
  //         sum_b bdur*mcol[b] ≤ stage_emin   (soft, ecost set)
  //         sum_b bdur*mcol[b] = stage_emin   (hard, no ecost)
  //     Same semantic as the pre-collapse `emin_col ∈ [0, stage_emin]`
  //     (soft) / `emin_col = stage_emin` (hard) — the aggregator was
  //     redundant tautological glue.
  //
  // Saves 1 column per (scenario, stage) per demand with emin
  // active.  Algebraically identical: pre-collapse obj contribution
  // was `−c × emin_col_optimal = −c × stage_emin` (soft) / `0` (hard);
  // post-collapse it's `Σ_b −c × bdur × mcol[b] = −c × stage_emin`
  // (soft) / `0` (hard) — same.
  auto emin_row = [&](const auto& stage_emin,
                      const auto& stage_ecost) -> std::optional<SparseRow>
  {
    if (!stage_emin) [[unlikely]] {
      return std::nullopt;
    }
    auto row = SparseRow {
        .class_name = Element::class_name.full_name(),
        .constraint_name = EminName,
        .variable_uid = uid(),
        .context = make_stage_context(scenario.uid(), stage.uid()),
    };
    if (stage_ecost) {
      // Soft: total energy ≤ stage_emin (the per-mcol cost provides
      // the incentive to push the total up to the cap).
      row.less_equal(*stage_emin);
    } else {
      // Hard: total energy exactly stage_emin.  No incentive cost —
      // the LP must hit the floor.
      row.equal(*stage_emin);
    }
    return row;
  }(stage_emin, stage_ecost);

  // Per-block lman-cost coefficient applied directly to each mcol
  // (soft path).  Zero when `stage_ecost` is unset (hard path) —
  // the LP must hit `stage_emin` via the equality row instead.
  const double per_unit_neg_cost = (stage_ecost && stage_emin)
      ? -CostHelper::scenario_stage_ecost(
            scenario, stage, *stage_ecost / stage.duration())
      : 0.0;

  BIndexHolder<ColIndex> lcols;
  BIndexHolder<ColIndex> mcols;
  BIndexHolder<RowIndex> crows;
  BIndexHolder<double> block_lmaxs;  // P0 reconstruction cache:
                                     // post-capacity-clamp `lmax` per block
  const bool is_forced = demand().forced.value_or(false);
  // ── P0 demand-failure substitution gate ───────────────────────────
  // Pre-P0: `fcol` (cost = fail_cost × ecost) + `frow`
  // (`lcol + fcol = lmax`) co-exist with `lcol` (cost = 0).
  // Post-P0 substituting `fail = lmax − load` collapses both extra LP
  // entities: `lcol` takes cost `−fail_cost × ecost` and the constant
  // `+fail_cost × ecost × lmax` rides via `lp.add_obj_constant(...)`.
  // The substitution applies only when failure has a real cost and
  // the demand isn't forced; otherwise lcol keeps its zero cost and
  // no obj_constant is accumulated.
  const bool fail_substituted = (stage_fcost.has_value() && !is_forced);

  // Reserve only the holders that will actually be populated in this
  // call.  `block_lmaxs` is always populated for blocks with non-zero
  // demand (one entry per block in the load path), so reserve it
  // up-front; the others stay guarded.
  map_reserve(lcols, blocks.size());
  map_reserve(block_lmaxs, blocks.size());
  if (stage_emin) {
    map_reserve(mcols, blocks.size());
  }
  if (capacity_col) {
    map_reserve(crows, blocks.size());
  }

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto block_lmax = sc.block_max_at(stage, block, lmax, stage_capacity);
    const bool has_load = block_lmax > 0.0;
    const bool has_lman = stage_emin && emin_row;
    // P1 LP-size: when block_lmax == 0 AND no emin (lman) work is
    // needed for this block, the entire iteration is degenerate
    // (lcol, fcol, frow, crow all collapse).  Skip eagerly so we
    // don't even fetch the bus balance row.
    if (!has_load && !has_lman) [[unlikely]] {
      continue;
    }

    const auto bus_balance_row = bus_balance_rows.at(buid);
    // Hoisted once per block: every `lp.add_col` / `SparseRow{...}`
    // initialiser below shares the same (scenario, stage, block)
    // context.  Pre-2026-05-14 the inner blocks re-built it 5×.
    const auto block_ctx =
        make_block_context(scenario.uid(), stage.uid(), block.uid());
    auto& bus_brow = lp.row_at(bus_balance_row);

    // ── P0 demand-failure substitution ──────────────────────────
    //
    // Pre-substitution: `lcol` (cost 0, bounds [0, lmax]) + `fcol`
    // (cost = fail_cost × ecost, bounds [0, ∞]) + balance row
    // `lcol + fcol = lmax`.  Substituting `fail = lmax − load`
    // collapses both extra LP entities:
    //
    //   c·fail = c·(lmax − lcol) = c·lmax − c·lcol
    //                              \_____/   \____/
    //                              constant   coef on lcol
    //
    // So lcol takes cost `−c = −fail_cost × ecost`, and the constant
    // `+c · lmax` is carried by `lp.add_obj_constant(...)` so
    // `LinearInterface::get_obj_value{,_raw}()` keep the algebraic
    // objective.  Saves 1 col + 1 row per block per elastic demand —
    // the dominant LP-size win at Juan scale.
    //
    // When block_lmax == 0 the substituted cost contribution is
    // `−fail_cost × 0 = 0`, obj_constant gets `+fail_cost × 0 = 0`,
    // the capacity row is trivially satisfied, and the bus_brow
    // coefficient is `0 × −(...)` — every entry below is a no-op.
    if (has_load) {
      const auto load_lowb = is_forced ? block_lmax : 0.0;
      double lcol_cost = 0.0;
      if (fail_substituted) {
        const auto block_ecost =
            CostHelper::block_ecost(scenario, stage, block, *stage_fcost);
        lcol_cost = -block_ecost;
        // Constant from the substitution; carries the failure-cost
        // baseline into `get_obj_value{,_raw}()` so the obj reported
        // to downstream consumers (SDDP bounds, standalone reporting,
        // pre-substitution test assertions) matches the un-rewritten
        // formulation exactly.
        lp.add_obj_constant(block_ecost * block_lmax);
      }
      const auto lcol = lp.add_col({
          .lowb = load_lowb,
          .uppb = block_lmax,
          .cost = lcol_cost,
          .class_name = Element::class_name.full_name(),
          .variable_name = LoadName,
          .variable_uid = uid(),
          .context = block_ctx,
      });

      lcols[buid] = lcol;
      // Cache the post-capacity-clamp `lmax` so `fail_sol_at` and
      // `add_to_output` can reconstruct `fail = lmax − load_sol`
      // without re-walking `block_max_at`.
      block_lmaxs[buid] = block_lmax;
      bus_brow[lcol] = -(1.0 + stage_lossfactor);

      // adding the capacity constraint
      if (capacity_col) {
        auto crow =
            SparseRow {
                .class_name = Element::class_name.full_name(),
                .constraint_name = CapacityName,
                .variable_uid = uid(),
                .context = block_ctx,
            }
                .greater_equal(0.0);

        crow[*capacity_col] = 1.0;
        crow[lcol] = -1.0;

        crows[buid] = lp.add_row(std::move(crow));
      }
    }

    // adding the minimum energy constraint (lman is independent of
    // lmax — represents extra load drawn to satisfy the stage-level
    // emin floor, valid even when natural demand is zero).  Post-
    // collapse `mcol[b]` directly carries the soft-emin reward cost
    // `−c × bdur` (zero on the hard path); the stage-level emin row
    // built above caps `sum_b bdur*mcol[b]` at `stage_emin`.
    if (has_lman) {
      const auto bdur = block.duration();

      const auto mcol = lp.add_col({
          .uppb = *stage_emin / bdur,
          .cost = per_unit_neg_cost * bdur,
          .class_name = Element::class_name.full_name(),
          .variable_name = LmanName,
          .variable_uid = uid(),
          .context = block_ctx,
      });

      mcols[buid] = mcol;
      (*emin_row)[mcol] = bdur;

      bus_brow[mcol] = -(1.0 + stage_lossfactor);
    }
  }

  if (emin_row) {
    emin_rows[st_key] = lp.add_row(std::move(*emin_row));
    lman_cols[st_key] = std::move(mcols);
  }

  if (!lcols.empty()) {
    load_cols[st_key] = std::move(lcols);
    sc.add_ampl_variable(
        ampl_name, uid(), LoadName, scenario, stage, load_cols.at(st_key));
  }
  if (!crows.empty()) {
    capacity_rows[st_key] = std::move(crows);
  }
  if (!block_lmaxs.empty()) {
    block_lmax_values_[st_key] = std::move(block_lmaxs);
  }

  // P0: `fail_cols` / `balance_rows` are gone — substituted away via
  // `fail = lmax − load`.  The constant baseline is in
  // `lp.add_obj_constant(...)` (per block, accumulated above); the
  // `Demand/fail_sol.csv` output is reconstructed in `add_to_output`
  // from `block_lmax_values_` and `load_cols`'s primal solution.
  //
  // `fail` is no longer registered as an AMPL variable either —
  // user constraints that referenced `demand.fail` would need to be
  // rewritten in terms of `demand.load` (or `lmax − demand.load`)
  // post-substitution; no production user-constraint does this on
  // the current corpus (grep, 2026-05-15).

  // `capainst` is registered centrally by CapacityBase::add_to_lp.

  return true;
}

bool DemandLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto pid = id();

  out.add_col_sol(cname, LoadName, pid, load_cols);
  out.add_col_cost(cname, LoadName, pid, load_cols);
  out.add_row_dual(cname, CapacityName, pid, capacity_rows);

  // Stage-level emin row dual (post-collapse the only emin entity
  // still in the LP).  `Demand/emin_dual.csv` captures the marginal
  // value of relaxing the stage-energy cap — useful for diagnostics
  // even though no production tooling reads it today.  The legacy
  // `Demand/emin_sol.csv` and `Demand/emin_cost.csv` outputs are
  // gone because the aggregator `emin_col` no longer exists; the
  // "energy delivered toward emin" value is `sum_b bdur × mcol[b]`,
  // i.e. the same information available via `lman_cols` if needed.
  out.add_row_dual(cname, EminName, pid, emin_rows);

  // `lman:cost` is **not** emitted — the load-management slack
  // column has no meaningful dispatch-cost interpretation and no
  // downstream tooling consumes `Demand/lman_cost.*` (verified by
  // grep across scripts/ / guiservice/ / integration_test/,
  // 2026-05-14).  The `:sol` value is retained so post-mortem
  // analysis can see how much emin slack each (s, t, b) used.
  out.add_col_sol(cname, LmanName, pid, lman_cols);

  // ── P0: reconstructed `Demand/fail_sol.csv` ─────────────────────
  //
  // Pre-substitution this came from `lp.col_sol[fail_col]`.  After
  // the substitution, the `fail` LP column is gone — the value is
  // reconstructed as `max(0, lmax − load_sol)` from cached
  // `block_lmax_values_` and the surviving `load_cols` primal
  // solution.  `OutputContext::primal(col)` reads from the same
  // span the index-based `add_col_sol(load_cols, ...)` above
  // consults, so the produced CSV matches the legacy LP-emitted
  // form byte-for-byte (up to the FP non-negativity clamp which a
  // perfectly-solved LP already satisfies).
  //
  // `Demand/fail_cost.csv` and `Demand/balance_dual.csv` are NOT
  // emitted: they were LP-internal artifacts (reduced cost on
  // `fcol` / shadow price on the deleted balance row) with no
  // downstream consumer (verified by grep across scripts/,
  // guiservice/, integration_test/ — only `Bus/balance_dual.csv` is
  // read).  Removing them is part of the LP-size win.
  STBIndexHolder<double> fail_sol_values;
  for (const auto& [st_key, lmax_block] : block_lmax_values_) {
    const auto lc_it = load_cols.find(st_key);
    if (lc_it == load_cols.end()) {
      continue;
    }
    BIndexHolder<double> fail_block;
    map_reserve(fail_block, lmax_block.size());
    for (const auto& [buid, block_lmax] : lmax_block) {
      const auto lc_block_it = lc_it->second.find(buid);
      if (lc_block_it == lc_it->second.end()) {
        continue;
      }
      const double load_sol = out.primal(lc_block_it->second);
      // Non-negativity clamp: numerically the LP keeps `load_sol ≤
      // block_lmax` exactly via the upper-bound constraint, but
      // post-solve floating-point noise can yield
      // `block_lmax − load_sol` of order 1e-14 < 0.  Match the
      // legacy `fcol ≥ 0` bound by clamping at 0.
      fail_block[buid] = std::max(0.0, block_lmax - load_sol);
    }
    if (!fail_block.empty()) {
      fail_sol_values[st_key] = std::move(fail_block);
    }
  }
  out.add_col_sol_values(cname, FailName, pid, fail_sol_values);

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
