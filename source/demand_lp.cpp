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
    , lmin(ic, Element::class_name, id(), std::move(object().lmin))
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

  const auto& bus_balance_rows = bus_lp.balance_rows_at(scenario, stage);
  const auto& blocks = stage.blocks();

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  // adding the minimum energy constraint
  const auto stage_emin = emin.optval(stage.uid());
  auto stage_ecost = ecost.optval(stage.uid());
  // ecost fallback: when `Demand.ecost` is unset, reuse `fcost` sampled
  // at the first block of the stage.  `fcost` is now per-(stage, block);
  // the stage-level emin/ecost feature is itself per-stage, so we sample
  // a single representative value (matches legacy semantics for scalar
  // fcost; for true per-block fcost only the first block's value drives
  // ecost — the per-block cost still applies to the load column).
  if (!stage_ecost && fcost.has_value() && !blocks.empty()) {
    stage_ecost = fcost.optval(stage.uid(), blocks.front().uid())
                      .or_else([&] { return sc.options().demand_fail_cost(); });
  } else if (!stage_ecost && sc.options().demand_fail_cost().has_value()) {
    stage_ecost = sc.options().demand_fail_cost();
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
  BIndexHolder<double> block_offsets;  // Option C: AMPL offset map
                                       // = block_lmax so user constraints
                                       // resolve `demand.load` -> col + lmax
  const bool is_forced = demand().forced.value_or(false);
  // Option C (experimental, opt-in via
  // `model_options.demand_fail_rhs_shift`; legacy
  // `demand_option_c` accepted as alias via naming-dialects).
  // When false (default): Option A — col represents `load`, cost
  // `−fail_cost × ecost`, and the `+fail_cost × ecost × lmax`
  // baseline rides on `lp.add_obj_constant(...)`.
  //
  // When true: col represents `neg_fail = load − lmax ∈ [−lmax, 0]`,
  // `obj_constant` stays at 0, and the baseline is absorbed by RHS
  // shifts on the bus-balance row (`+(1+loss)·lmax`) and capacity
  // row (`+lmax`).  The AMPL resolver receives a per-block offset
  // so user constraints referencing `demand.load` resolve to
  // `(col + lmax)` physically (via `param_shift`).
  //
  // **Caveat**: LP-side consumers that reference the demand column
  // directly (Converter, Battery interactions) assume the col
  // carries `load`, not `neg_fail`.  Enabling Option C with a
  // converter-tied demand produces a mathematically wrong row.
  // Audit before enabling on a model with Converter elements.
  const bool option_c = sc.options().demand_fail_rhs_shift();
  // `fcost` is now per-(stage, block); substitution is enabled when the
  // field is set anywhere on the schedule grid OR the global
  // `model_options.demand_fail_cost` fallback is set.  Per-block
  // resolution happens inside the block loop via
  // `sc.demand_fail_cost(stage, block, fcost)`.
  const bool fail_substituted =
      ((fcost.has_value() || sc.options().demand_fail_cost().has_value())
       && !is_forced);
  const bool use_option_c = fail_substituted && option_c;

  // Reserve only the holders that will actually be populated in this
  // call.  `block_lmaxs` is always populated for blocks with non-zero
  // demand (one entry per block in the load path), so reserve it
  // up-front; the others stay guarded.
  map_reserve(lcols, blocks.size());
  map_reserve(block_lmaxs, blocks.size());
  if (use_option_c) {
    map_reserve(block_offsets, blocks.size());
  }
  if (stage_emin) {
    map_reserve(mcols, blocks.size());
  }
  if (capacity_col) {
    map_reserve(crows, blocks.size());
  }

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto block_lmax = sc.block_max_at(stage, block, lmax, stage_capacity);
    const auto block_lossfactor =
        lossfactor.optval(stage.uid(), buid).value_or(0.0);
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
    // `lcol + fcol = lmax`.
    //
    // Option A (default): substitute `fail = lmax − load`:
    //   * `lcol.cost = −fail_cost × ecost`,
    //   * `lp.add_obj_constant(+fail_cost × ecost × lmax)`,
    //   * bus_brow / capacity row RHS = 0 (default).
    //
    // Option C (`model_options.demand_fail_rhs_shift = true`): substitute
    // `neg_fail = load − lmax`:
    //   * `lcol.cost = −fail_cost × ecost` (same),
    //   * `lcol.bounds = [-lmax, 0]`,
    //   * `obj_constant = 0`,
    //   * bus_brow RHS shifts `+(1+loss)·lmax`,
    //   * capacity row RHS shifts `+lmax`,
    //   * AMPL registers offset = lmax so user constraints
    //     referencing `demand.load` resolve to (col + lmax).
    //
    // Both options preserve the algebraic obj value to within FP.
    // Option C eliminates ~$105 B obj_constant cancellation on
    // juan-scale runs but is currently incompatible with LP-side
    // consumers that reference the demand column directly
    // (Converter, Battery) — they expect `load` semantics.
    //
    // When block_lmax == 0 every entry below is a no-op (zero
    // coefficients, zero RHS shift, zero cost contribution).
    if (has_load) {
      // ``lmin`` provides a HARD floor on served load (per-(stage, block));
      // used to propagate ``Battery.pmin_charge`` onto the synthetic
      // charge demand so the LP must charge at least that rate every
      // block.  Floor at 0 (no negative floors).
      const auto block_lmin =
          std::max(0.0, lmin.optval(stage.uid(), buid).value_or(0.0));
      double col_lowb = std::max(block_lmin, is_forced ? block_lmax : 0.0);
      double col_uppb = block_lmax;
      double lcol_cost = 0.0;
      if (fail_substituted) {
        const auto block_fcost = sc.demand_fail_cost(stage, block, fcost);
        const auto block_ecost = CostHelper::block_ecost(
            scenario, stage, block, block_fcost.value_or(0.0));
        lcol_cost = -block_ecost;
        if (block_ecost > 0.0) {
          fail_penalized_ = true;
        }
        if (use_option_c) {
          // Option C: col represents neg_fail ∈ [-lmax, 0].
          col_lowb = -block_lmax;
          col_uppb = 0.0;
        } else {
          // Option A: col represents load ∈ [0, lmax].
          // obj_constant carries the +fail_cost × ecost × lmax
          // baseline so `get_obj_value()` matches the
          // algebraically-original pre-substitution objective.
          lp.add_obj_constant(block_ecost * block_lmax);
        }
      }
      const auto lcol = lp.add_col({
          .lowb = col_lowb,
          .uppb = col_uppb,
          .cost = lcol_cost,
          .class_name = Element::class_name.full_name(),
          .variable_name = LoadName,
          .variable_uid = uid(),
          .context = block_ctx,
      });

      lcols[buid] = lcol;
      // Cache the post-capacity-clamp `lmax` so `add_to_output` can
      // reconstruct `load` / `fail` without re-walking `block_max_at`.
      block_lmaxs[buid] = block_lmax;
      bus_brow[lcol] = -(1.0 + block_lossfactor);

      if (use_option_c) {
        // AMPL offset = lmax so `demand.load = col + lmax` for
        // user constraints (see ResolvedCol::offset).
        block_offsets[buid] = block_lmax;
        // Bus balance RHS shift: +(1+loss)·lmax per elastic-demand
        // block.  Bus balance is an equality row (default
        // lowb == uppb == 0), so we shift both bounds equally.
        const double bus_rhs_shift = (1.0 + block_lossfactor) * block_lmax;
        bus_brow.lowb += bus_rhs_shift;
        bus_brow.uppb += bus_rhs_shift;
      }

      // adding the capacity constraint
      if (capacity_col) {
        // Option C: `cap_col − neg_fail ≥ lmax` (RHS shift +lmax).
        // Option A / non-substituted: `cap_col − load ≥ 0`.
        const double crow_rhs = use_option_c ? block_lmax : 0.0;
        auto crow =
            SparseRow {
                .class_name = Element::class_name.full_name(),
                .constraint_name = CapacityName,
                .variable_uid = uid(),
                .context = block_ctx,
            }
                .greater_equal(crow_rhs);

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

      bus_brow[mcol] = -(1.0 + block_lossfactor);
    }
  }

  if (emin_row) {
    emin_rows[st_key] = lp.add_row(std::move(*emin_row));
    lman_cols[st_key] = std::move(mcols);
  }

  if (!lcols.empty()) {
    load_cols[st_key] = std::move(lcols);
    if (!block_offsets.empty()) {
      // Option C: col represents `neg_fail = load − lmax`; register
      // the per-block offset so the AMPL resolver reports
      // `demand.load = col + lmax` for user constraints.
      sc.add_ampl_variable(ampl_name,
                           uid(),
                           LoadName,
                           scenario,
                           stage,
                           load_cols.at(st_key),
                           block_offsets);
    } else {
      // Option A (forced demand, or no fcost): col represents
      // `load` directly; no offset registered.
      sc.add_ampl_variable(
          ampl_name, uid(), LoadName, scenario, stage, load_cols.at(st_key));
    }
  }
  if (!crows.empty()) {
    capacity_rows[st_key] = std::move(crows);
  }
  if (!block_lmaxs.empty()) {
    block_lmax_values_[st_key] = std::move(block_lmaxs);
  }
  if (!block_offsets.empty()) {
    block_offset_values_[st_key] = std::move(block_offsets);
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

  // When Option C is OFF for every (scene, phase): col directly
  // carries `load`, so emit it via the standard add_col_sol /
  // add_col_cost paths.  When Option C is ON for at least one cell,
  // we reconstruct both load and fail via add_col_sol_values below
  // and skip the load_cost emission (its physical interpretation
  // under Option C is the reduced cost of `neg_fail`, sign-flipped,
  // with no downstream consumer).
  const bool option_c_active = !block_offset_values_.empty();
  if (!option_c_active) {
    out.add_col_sol(cname, LoadName, pid, load_cols);
    out.add_col_cost(cname, LoadName, pid, load_cols);
  }

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

  // ── Reconstructed `Demand/fail_sol.csv` (always) and
  //    `Demand/load_sol.csv` (Option C only) ───────────────────────
  //
  // fail_sol is reconstructed in every mode because the `fail` LP
  // column was substituted away (both Option A and C share that).
  // load_sol is reconstructed only under Option C, where the LP
  // column carries `neg_fail = load − lmax` (offset!=0).  Otherwise
  // (Option A / forced / non-substituted) load_sol was already
  // emitted above via the direct add_col_sol path.
  STBIndexHolder<double> load_sol_values;
  STBIndexHolder<double> fail_sol_values;
  for (const auto& [st_key, lmax_block] : block_lmax_values_) {
    const auto lc_it = load_cols.find(st_key);
    if (lc_it == load_cols.end()) {
      continue;
    }
    const auto off_it = block_offset_values_.find(st_key);
    const bool has_offset = (off_it != block_offset_values_.end());

    BIndexHolder<double> load_block;
    BIndexHolder<double> fail_block;
    map_reserve(fail_block, lmax_block.size());
    if (has_offset) {
      map_reserve(load_block, lmax_block.size());
    }
    for (const auto& [buid, block_lmax] : lmax_block) {
      const auto lc_block_it = lc_it->second.find(buid);
      if (lc_block_it == lc_it->second.end()) {
        continue;
      }
      const double col_primal = out.primal(lc_block_it->second);
      double offset = 0.0;
      if (has_offset) {
        const auto ob_it = off_it->second.find(buid);
        if (ob_it != off_it->second.end()) {
          offset = ob_it->second;
        }
      }
      // Option C (offset != 0): col_primal == neg_fail ∈ [-lmax, 0].
      //   load = col_primal + offset
      //   fail = -col_primal
      // Option A / forced (offset == 0): col_primal == load.
      //   fail = lmax - col_primal
      // Clamp at 0 to absorb FP noise of order 1e-14.
      if (has_offset) {
        load_block[buid] = std::max(0.0, col_primal + offset);
        fail_block[buid] = std::max(0.0, -col_primal);
      } else {
        fail_block[buid] = std::max(0.0, block_lmax - col_primal);
      }
    }
    if (has_offset && !load_block.empty()) {
      load_sol_values[st_key] = std::move(load_block);
    }
    if (!fail_block.empty()) {
      fail_sol_values[st_key] = std::move(fail_block);
    }
  }
  if (option_c_active) {
    out.add_col_sol_values(cname, LoadName, pid, load_sol_values);
  }
  // Only emit fail_sol when failure is actually penalized.  An unpenalized
  // demand (fcost == 0, e.g. a battery-charge `<name>_dem`) has
  // `fail = lmax − load` equal to unused dispatchable load capacity, not
  // curtailment — emitting it pollutes Demand/fail_sol.
  if (fail_penalized_) {
    out.add_col_sol_values(cname, FailName, pid, fail_sol_values);
  }

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
