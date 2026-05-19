/**
 * @file      commitment_lp.cpp
 * @brief     LP formulation for unit commitment (three-bin u/v/w model)
 * @date      Tue Apr  8 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements CommitmentLP: creates binary status/startup/shutdown variables,
 * adds the three core UC constraints (logic, gen limits, exclusion),
 * emission cost adder on generation variables, and emission cap constraint.
 */

#include <gtopt/commitment_lp.hpp>
#include <gtopt/fuel_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reserve_provision_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

CommitmentLP::CommitmentLP(const Commitment& commitment, const InputContext& ic)
    : Base(commitment, ic, Element::class_name)
    , generator_index_(ic.element_index(generator_sid()))
    , startup_cost_(
          ic, Element::class_name, id(), std::move(object().startup_cost))
    , shutdown_cost_(
          ic, Element::class_name, id(), std::move(object().shutdown_cost))
    , fuel_cost_(ic, Element::class_name, id(), std::move(object().fuel_cost))
    , fuel_emission_factor_(ic,
                            Element::class_name,
                            id(),
                            std::move(object().fuel_emission_factor))
    , fixed_status_(
          ic, Element::class_name, id(), std::move(object().fixed_status))
{
}

bool CommitmentLP::add_to_lp(SystemContext& sc,
                             const ScenarioLP& scenario,
                             const StageLP& stage,
                             LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  // UC constraints only apply to chronological stages
  if (!stage.is_chronological()) {
    return true;
  }

  auto&& generator_lp = sc.element(generator_index_);
  if (!generator_lp.is_active(stage)) {
    return true;
  }

  const auto& generation_cols =
      generator_lp.generation_cols_at(scenario, stage);
  const auto& blocks = stage.blocks();

  if (blocks.empty()) {
    return true;
  }

  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto cuid = uid();

  // Resolve commitment parameters
  const auto noload = commitment().noload_cost.value_or(0.0);
  const auto stage_startup_cost =
      startup_cost_.optval(stage.uid()).value_or(0.0);
  const auto stage_shutdown_cost =
      shutdown_cost_.optval(stage.uid()).value_or(0.0);
  const auto initial_u = commitment().initial_status.value_or(0.0);
  const auto is_relax = commitment().relax.value_or(false)
      || sc.simulation().phases()[stage.phase_index()].is_continuous();
  const auto is_must_run = commitment().must_run.value_or(false);
  const auto opt_ramp_up = commitment().ramp_up;
  const auto opt_ramp_down = commitment().ramp_down;
  const auto opt_startup_ramp = commitment().startup_ramp;
  const auto opt_shutdown_ramp = commitment().shutdown_ramp;

  // Resolve piecewise heat rate curve
  const auto& pmax_segs = commitment().pmax_segments;
  const auto& hr_segs = commitment().heat_rate_segments;
  const auto has_segments = !pmax_segs.empty() && !hr_segs.empty()
      && pmax_segs.size() == hr_segs.size();

  // Resolve fuel-related parameters.  When `Commitment.fuel` is set
  // (PLEXOS-style FK), the price + emission factors come from the
  // referenced Fuel element; the inline legacy schedules
  // (`fuel_cost`, `fuel_emission_factor`) are ignored with a warning
  // when both are present.  When `fuel` is unset, fall back to the
  // legacy inline schedules (pre-2026-05 behaviour).
  double stage_fuel_cost = 0.0;
  if (commitment().fuel.has_value()) {
    const auto& fuel_lp = sc.element<FuelLP>(FuelLPSId {*commitment().fuel});
    stage_fuel_cost = fuel_lp.param_price(stage.uid()).value_or(0.0);
    if (commitment().fuel_cost.has_value()
        || commitment().fuel_emission_factor.has_value())
    {
      SPDLOG_WARN(
          "Commitment uid={} has both `fuel` reference and legacy "
          "`fuel_cost`/`fuel_emission_factor` schedules set — the "
          "Fuel reference wins.  Remove the legacy schedules to "
          "silence this warning.",
          cuid);
    }
  } else if (has_segments) {
    stage_fuel_cost = fuel_cost_.optval(stage.uid()).value_or(0.0);
  }

  // Pre-size per-segment column holders
  if (has_segments && segment_cols_.empty()) {
    segment_cols_.resize(pmax_segs.size());
  }

  // Emission cost is no longer wired here — the per-pollutant tax
  // and cap live on `EmissionZone` (see `emission_zone_lp.cpp`),
  // which adds its `price · production` term to the objective via
  // a dedicated bridge column.  Generator dispatch + segment cost
  // accounting here is fuel-only.

  // Resolve startup cost tiers
  const auto opt_hot_cost = commitment().hot_start_cost;
  const auto opt_warm_cost = commitment().warm_start_cost;
  const auto opt_cold_cost = commitment().cold_start_cost;
  const auto opt_hot_time = commitment().hot_start_time;
  const auto opt_cold_time = commitment().cold_start_time;
  const bool has_startup_tiers = opt_hot_cost.has_value()
      && opt_warm_cost.has_value() && opt_cold_cost.has_value()
      && opt_hot_time.has_value() && opt_cold_time.has_value();

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};

  // ── Compute commitment periods ──
  // When commitment_period is set, binary variables (u/v/w) are created at
  // a coarser resolution.  Each period groups consecutive blocks whose
  // cumulative duration fits within commitment_period hours.
  // Default: one period per block (commitment_period not set).
  const auto opt_period = commitment().commitment_period;
  const auto period_hours = opt_period.value_or(0.0);  // 0 = per-block

  // Period grouping: each entry is the index of the first block in a period.
  // period_starts[k] = index into blocks[] where period k begins.
  // A final sentinel equal to blocks.size() marks the end.
  std::vector<size_t> period_starts;
  period_starts.push_back(0);
  if (period_hours > 0.0) {
    double accum = 0.0;
    for (size_t i = 0; i < blocks.size(); ++i) {
      accum += blocks[i].duration();
      if (accum >= period_hours - 1e-9 && i + 1 < blocks.size()) {
        period_starts.push_back(i + 1);
        accum = 0.0;
      }
    }
  } else {
    // One period per block
    for (size_t i = 1; i < blocks.size(); ++i) {
      period_starts.push_back(i);
    }
  }
  const auto nperiods = period_starts.size();

  // Map every block index → its period index
  std::vector<size_t> block_period(blocks.size());
  for (size_t p = 0; p < nperiods; ++p) {
    const auto start = period_starts[p];
    const auto end = (p + 1 < nperiods) ? period_starts[p + 1] : blocks.size();
    for (size_t i = start; i < end; ++i) {
      block_period[i] = p;
    }
  }

  // ── Phase A: Create u/v/w per commitment period, C1 and C3 ──
  BIndexHolder<ColIndex> ucols;  // period representative buid → u col
  BIndexHolder<ColIndex> vcols;
  BIndexHolder<ColIndex> wcols;
  BIndexHolder<RowIndex> lrows;
  BIndexHolder<RowIndex> erows;
  map_reserve(ucols, nperiods);
  map_reserve(vcols, nperiods);
  map_reserve(wcols, nperiods);
  map_reserve(lrows, nperiods);
  map_reserve(erows, nperiods);

  // Period-indexed v/w columns: avoid repeated map lookups per block in the
  // phase-B loop (each period is visited once per block in that period).
  std::vector<ColIndex> period_vcol(nperiods);
  std::vector<ColIndex> period_wcol(nperiods);

  // block_ucol maps EVERY block uid → its period's u column
  BIndexHolder<ColIndex> block_ucol;
  map_reserve(block_ucol, blocks.size());

  ColIndex prev_ucol {};
  bool first_period = true;

  for (size_t p = 0; p < nperiods; ++p) {
    const auto pstart = period_starts[p];
    const auto pend = (p + 1 < nperiods) ? period_starts[p + 1] : blocks.size();

    // Period representative block (first block in the period)
    const auto& rep_block = blocks[pstart];
    const auto rep_buid = rep_block.uid();
    const auto ctx = make_block_context(scenario.uid(), stage.uid(), rep_buid);

    // Accumulate period duration for noload cost
    double period_duration = 0.0;
    for (size_t i = pstart; i < pend; ++i) {
      period_duration += blocks[i].duration();
    }

    // ── Create u (status) variable ──
    // Noload cost is proportional to the period duration, not block duration.
    // Use the representative block for probability/discount factors.
    const auto u_cost =
        CostHelper::block_ecost(scenario, stage, rep_block, noload)
        * (period_duration / rep_block.duration());

    // Resolve commitment bounds.  Precedence (highest → lowest):
    //   1. ``fixed_status`` schedule entry for this (stage, block) — if
    //      set to a value in ``[0, 1]``, pins ``u`` to that value
    //      (rounded at 0.5 to 0 or 1).  Values *outside* ``[0, 1]``
    //      are reserved as the "no-pin" sentinel so the schedule can
    //      pin some blocks and leave others free without juggling a
    //      proper nullable cell type (UC.jl's ``Commitment status``
    //      arrays can contain ``null`` for un-pinned blocks; the
    //      converter emits ``-1.0`` there).
    //   2. ``must_run`` flag — forces ``u >= 1`` for the whole stage.
    //   3. Default — ``u`` in ``[0, 1]``.
    auto u_lowb = is_must_run ? 1.0 : 0.0;
    auto u_uppb = 1.0;
    if (const auto pin = fixed_status_.optval(stage.uid(), rep_buid)) {
      const auto val = pin.value();
      if (val >= 0.0 && val <= 1.0) {
        const auto pinned = (val >= 0.5) ? 1.0 : 0.0;
        u_lowb = pinned;
        u_uppb = pinned;
      }
    }

    auto ucol = lp.add_col({
        .lowb = u_lowb,
        .uppb = u_uppb,
        .cost = u_cost,
        .is_integer = !is_relax,
        .class_name = cname,
        .variable_name = StatusName,
        .variable_uid = cuid,
        .context = ctx,
    });
    ucols[rep_buid] = ucol;

    // Map all blocks in this period to the same u column
    for (size_t i = pstart; i < pend; ++i) {
      block_ucol[blocks[i].uid()] = ucol;
    }

    // ── Create v (startup) variable ──
    const auto v_cost = has_startup_tiers
        ? 0.0
        : CostHelper::block_ecost(
              scenario, stage, rep_block, stage_startup_cost);
    auto vcol = lp.add_col({
        .lowb = 0.0,
        .uppb = 1.0,
        .cost = v_cost,
        .is_integer = !is_relax,
        .class_name = cname,
        .variable_name = StartupName,
        .variable_uid = cuid,
        .context = ctx,
    });
    vcols[rep_buid] = vcol;
    period_vcol[p] = vcol;

    // ── Create w (shutdown) variable ──
    const auto w_cost = CostHelper::block_ecost(
        scenario, stage, rep_block, stage_shutdown_cost);
    auto wcol = lp.add_col({
        .lowb = 0.0,
        .uppb = 1.0,
        .cost = w_cost,
        .is_integer = !is_relax,
        .class_name = cname,
        .variable_name = ShutdownName,
        .variable_uid = cuid,
        .context = ctx,
    });
    wcols[rep_buid] = wcol;
    period_wcol[p] = wcol;

    // ── C1: Logic transition (per period) ──
    // u[p] - u[p-1] - v[p] + w[p] = 0
    // For first period: u[0] - v[0] + w[0] = initial_status
    {
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = LogicName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .equal(first_period ? initial_u : 0.0);
      row[ucol] = 1.0;
      if (!first_period) {
        row[prev_ucol] = -1.0;
      }
      row[vcol] = -1.0;
      row[wcol] = 1.0;
      lrows[rep_buid] = lp.add_row(std::move(row));
    }

    // ── C3: Exclusion (per period): v[p] + w[p] <= 1 ──
    {
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = ExclusionName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .less_equal(1.0);
      row[vcol] = 1.0;
      row[wcol] = 1.0;
      erows[rep_buid] = lp.add_row(std::move(row));
    }

    prev_ucol = ucol;
    first_period = false;
  }

  // ── Phase B: Per-block constraints (C2, ramp, emission, segments) ──
  // Each block references its period's u column via block_ucol.
  BIndexHolder<RowIndex> gurows;
  BIndexHolder<RowIndex> glrows;
  BIndexHolder<RowIndex> rurows;
  BIndexHolder<RowIndex> rdrows;
  map_reserve(gurows, blocks.size());
  map_reserve(glrows, blocks.size());
  map_reserve(rurows, blocks.size());
  map_reserve(rdrows, blocks.size());

  ColIndex prev_gcol {};
  ColIndex prev_block_ucol {};
  bool first_block = true;

  for (size_t bidx = 0; bidx < blocks.size(); ++bidx) {
    const auto& block = blocks[bidx];
    const auto buid = block.uid();
    const auto ctx = make_block_context(scenario.uid(), stage.uid(), buid);

    const auto gcol_it = generation_cols.find(buid);
    if (gcol_it == generation_cols.end()) {
      continue;
    }
    const auto gcol = gcol_it->second;
    const auto ucol = block_ucol.at(buid);

    // Look up period's v/w for ramp constraints via period-indexed cache
    // (avoids two map .at() lookups per block).
    const auto pidx = block_period[bidx];
    const auto vcol = period_vcol[pidx];
    const auto wcol = period_wcol[pidx];

    const auto gen_pmax = lp.get_col_uppb(gcol);
    const auto gen_pmin = lp.get_col_lowb(gcol);

    // ── C2: Generation upper bound: p - Pmax*u <= 0 ──
    auto& gcol_ref = lp.col_at(gcol);
    gcol_ref.lowb = 0.0;

    {
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = GenUpperName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .less_equal(0.0);
      row[gcol] = 1.0;
      row[ucol] = -gen_pmax;
      gurows[buid] = lp.add_row(std::move(row));
    }

    // ── C2: Generation lower bound: p - Pmin*u >= 0 ──
    {
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = GenLowerName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .greater_equal(0.0);
      row[gcol] = 1.0;
      row[ucol] = -gen_pmin;
      glrows[buid] = lp.add_row(std::move(row));
    }

    // ── C4: Ramp up: p[t] - p[t-1] ≤ RU·u[t-1] + SU·v[t] ──
    if (opt_ramp_up.has_value() || opt_startup_ramp.has_value()) {
      const auto ru = opt_ramp_up.value_or(gen_pmax) * block.duration();
      const auto su = opt_startup_ramp.value_or(gen_pmax);

      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = RampUpName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .less_equal(first_block
                              ? (ru * initial_u) + (su * (1.0 - initial_u))
                              : 0.0);
      row[gcol] = 1.0;
      if (first_block) {
        // p[0] ≤ RU·u_init + SU·(1-u_init) + p_prev
        // For simplicity, p_prev is not tracked across stages.
      } else {
        row[prev_gcol] = -1.0;
        row[prev_block_ucol] = -ru;
        row[vcol] = -su;
      }
      rurows[buid] = lp.add_row(std::move(row));
    }

    // ── C5: Ramp down: p[t-1] - p[t] ≤ RD·u[t] + SD·w[t] ──
    if (opt_ramp_down.has_value() || opt_shutdown_ramp.has_value()) {
      const auto rd = opt_ramp_down.value_or(gen_pmax) * block.duration();
      const auto sd = opt_shutdown_ramp.value_or(gen_pmax);

      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = RampDownName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .less_equal(first_block
                              ? (rd * initial_u) + (sd * (1.0 - initial_u))
                              : 0.0);
      row[gcol] = -1.0;
      if (first_block) {
        // RHS includes the initial contribution.
      } else {
        row[prev_gcol] = 1.0;
        row[ucol] = -rd;
        row[wcol] = -sd;
      }
      rdrows[buid] = lp.add_row(std::move(row));
    }

    // ── Piecewise heat rate curve ──
    //
    // Segment costs are FUEL-ONLY here.  The per-pollutant tax (if
    // any) is wired separately through `EmissionZoneLP` — its
    // `production` bridge column is summed over generator dispatch
    // segments via `EmissionSourceLP`-injected coefficients on
    // `Emission/balance` rows.
    if (has_segments && stage_fuel_cost > 0.0) {
      gcol_ref.cost = 0.0;

      // Build linking row: p - Pmin·u - Σ δ_k = 0
      auto link_row =
          SparseRow {
              .class_name = cname,
              .constraint_name = SegmentName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .equal(0.0);
      link_row[gcol] = 1.0;
      link_row[ucol] = -gen_pmin;

      double prev_breakpoint = gen_pmin;
      for (const auto& [k, pmax_k] : enumerate<int>(pmax_segs)) {
        const auto seg_width = pmax_k - prev_breakpoint;
        if (seg_width <= 0.0) {
          prev_breakpoint = pmax_k;
          continue;
        }

        const auto seg_marginal = stage_fuel_cost * hr_segs[k];
        const auto seg_cost =
            CostHelper::block_ecost(scenario, stage, block, seg_marginal);

        const auto seg_ctx =
            make_block_context(scenario.uid(), stage.uid(), buid, k);

        auto seg_col = lp.add_col({
            .lowb = 0.0,
            .uppb = seg_width,
            .cost = seg_cost,
            .class_name = cname,
            .variable_name = SegmentName,
            .variable_uid = cuid,
            .context = seg_ctx,
        });

        // Segment bound: δ_k ≤ w_k·u
        {
          auto seg_bound_row =
              SparseRow {
                  .class_name = cname,
                  .constraint_name = SegmentName,
                  .variable_uid = cuid,
                  .context = seg_ctx,
              }
                  .less_equal(0.0);
          seg_bound_row[seg_col] = 1.0;
          seg_bound_row[ucol] = -seg_width;
          std::ignore = lp.add_row(std::move(seg_bound_row));
        }

        link_row[seg_col] = -1.0;
        segment_cols_[k][st_key][buid] = seg_col;
        prev_breakpoint = pmax_k;
      }

      segment_link_rows_[st_key][buid] = lp.add_row(std::move(link_row));
    }

    prev_block_ucol = ucol;
    prev_gcol = gcol;
    first_block = false;
  }

  // ── Reserve-UC integration ──
  // Modify existing reserve provision headroom rows to be conditional on u.
  // Up-provision:   g + r_up ≤ Pmax      →  g + r_up - Pmax·u ≤ 0
  // Down-provision: g - r_dn ≥ Pmin      →  g - r_dn - Pmin·u ≥ 0
  if (!block_ucol.empty()) {
    for (const auto& rprov : sc.elements<ReserveProvisionLP>()) {
      if (rprov.generator_sid() != generator_sid()) {
        continue;
      }
      for (const auto& block : blocks) {
        const auto buid = block.uid();
        const auto ucol_it = block_ucol.find(buid);
        if (ucol_it == block_ucol.end()) {
          continue;
        }
        const auto ucol = ucol_it->second;

        // Modify up-provision row: add -Pmax on u, change RHS to 0
        if (const auto urow =
                rprov.lookup_up_provision_row(scenario, stage, buid))
        {
          auto& row = lp.row_at(*urow);
          const auto pmax = row.uppb;  // original Pmax was stored as uppb
          lp.set_coeff(*urow, ucol, -pmax);
          row.uppb = 0.0;
        }

        // Modify down-provision row: add -Pmin on u, change RHS to 0
        if (const auto drow =
                rprov.lookup_dn_provision_row(scenario, stage, buid))
        {
          auto& row = lp.row_at(*drow);
          const auto pmin = row.lowb;  // original Pmin was stored as lowb
          lp.set_coeff(*drow, ucol, -pmin);
          row.lowb = 0.0;
        }
      }
    }
  }

  // ── C6: Min up time (at period level) ──
  // Σ_{q=p}^{min(p+UT_periods-1,P)} u[q] ≥ UT_periods · v[p]
  // Accumulates period durations to find how many periods cover min_up_hours.
  const auto min_up_hours = commitment().min_up_time.value_or(0.0);
  if (min_up_hours > 0.0 && !ucols.empty() && !vcols.empty()) {
    BIndexHolder<RowIndex> mut_rows;
    map_reserve(mut_rows, nperiods);

    // Compute period durations
    std::vector<double> period_dur(nperiods);
    for (size_t p = 0; p < nperiods; ++p) {
      const auto pstart = period_starts[p];
      const auto pend =
          (p + 1 < nperiods) ? period_starts[p + 1] : blocks.size();
      double dur = 0.0;
      for (size_t i = pstart; i < pend; ++i) {
        dur += blocks[i].duration();
      }
      period_dur[p] = dur;
    }

    for (size_t p = 0; p < nperiods; ++p) {
      const auto rep_buid = blocks[period_starts[p]].uid();
      const auto vcol_it = vcols.find(rep_buid);
      if (vcol_it == vcols.end()) {
        continue;
      }

      // Count how many periods from p onward cover min_up_hours
      double accum_hours = 0.0;
      size_t ut_periods = 0;
      for (size_t q = p; q < nperiods && accum_hours < min_up_hours; ++q) {
        accum_hours += period_dur[q];
        ++ut_periods;
      }
      if (ut_periods <= 1) {
        continue;  // trivially satisfied
      }

      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = MinUpTimeName,
              .variable_uid = cuid,
              .context =
                  make_block_context(scenario.uid(), stage.uid(), rep_buid),
          }
              .greater_equal(0.0);
      // Σ u[q] - UT_periods · v[p] ≥ 0
      for (size_t q = p; q < p + ut_periods && q < nperiods; ++q) {
        const auto q_buid = blocks[period_starts[q]].uid();
        const auto ucol_it = ucols.find(q_buid);
        if (ucol_it != ucols.end()) {
          row[ucol_it->second] = 1.0;
        }
      }
      row[vcol_it->second] = -static_cast<double>(ut_periods);
      mut_rows[rep_buid] = lp.add_row(std::move(row));
    }
    if (!mut_rows.empty()) {
      min_up_time_rows_[st_key] = std::move(mut_rows);
    }
  }

  // ── C7: Min down time (at period level) ──
  // Σ_{q=p}^{min(p+DT_periods-1,P)} (1 - u[q]) ≥ DT_periods · w[p]
  // Rearranged: Σ u[q] + DT_periods · w[p] ≤ span
  const auto min_down_hours = commitment().min_down_time.value_or(0.0);
  if (min_down_hours > 0.0 && !ucols.empty() && !wcols.empty()) {
    BIndexHolder<RowIndex> mdt_rows;
    map_reserve(mdt_rows, nperiods);

    // Reuse or recompute period_dur if not already computed
    std::vector<double> pd(nperiods);
    for (size_t p = 0; p < nperiods; ++p) {
      const auto ps = period_starts[p];
      const auto pe = (p + 1 < nperiods) ? period_starts[p + 1] : blocks.size();
      double dur = 0.0;
      for (size_t i = ps; i < pe; ++i) {
        dur += blocks[i].duration();
      }
      pd[p] = dur;
    }

    for (size_t p = 0; p < nperiods; ++p) {
      const auto rep_buid = blocks[period_starts[p]].uid();
      const auto wcol_it = wcols.find(rep_buid);
      if (wcol_it == wcols.end()) {
        continue;
      }

      double accum_hours = 0.0;
      size_t dt_periods = 0;
      for (size_t q = p; q < nperiods && accum_hours < min_down_hours; ++q) {
        accum_hours += pd[q];
        ++dt_periods;
      }
      if (dt_periods <= 1) {
        continue;
      }

      const auto span = std::min(p + dt_periods, nperiods) - p;
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = MinDownTimeName,
              .variable_uid = cuid,
              .context =
                  make_block_context(scenario.uid(), stage.uid(), rep_buid),
          }
              .less_equal(static_cast<double>(span));
      // Σ u[q] + DT_periods · w[p] ≤ span
      for (size_t q = p; q < p + dt_periods && q < nperiods; ++q) {
        const auto q_buid = blocks[period_starts[q]].uid();
        const auto ucol_it = ucols.find(q_buid);
        if (ucol_it != ucols.end()) {
          row[ucol_it->second] = 1.0;
        }
      }
      row[wcol_it->second] = static_cast<double>(dt_periods);
      mdt_rows[rep_buid] = lp.add_row(std::move(row));
    }
    if (!mdt_rows.empty()) {
      min_down_time_rows_[st_key] = std::move(mdt_rows);
    }
  }

  // ── C8/C9/C10: Hot/warm/cold startup cost tiers (per period) ──
  // When all tier parameters are defined, create per-period tier variables
  // y_hot, y_warm, y_cold with constraints:
  //   C8: v[p] = y_hot[p] + y_warm[p] + y_cold[p]  (type selection)
  //   C9: y_hot[p] ≤ Σ w[q] for q in hot window    (recent shutdown)
  //   C10: y_warm[p] ≤ Σ w[q] for q in warm window  (medium offline)
  //   cold start is the residual via C8.
  if (has_startup_tiers && !vcols.empty() && !wcols.empty()) {
    const auto hot_cost = *opt_hot_cost;
    const auto warm_cost = *opt_warm_cost;
    const auto cold_cost = *opt_cold_cost;
    const auto hot_time = *opt_hot_time;
    const auto cold_time = *opt_cold_time;

    // Validate: cold_time must be >= hot_time (cold = longer offline)
    if (cold_time < hot_time) {
      spdlog::warn(
          "Commitment {}: cold_start_time ({}) < hot_start_time ({}), "
          "skipping startup tiers",
          commitment().name,
          cold_time,
          hot_time);
      return true;
    }
    const auto initial_hours_offline =
        (initial_u < 0.5) ? commitment().initial_hours.value_or(1e6) : 0.0;

    // Compute period durations for window counting
    std::vector<double> pdur(nperiods);
    for (size_t p = 0; p < nperiods; ++p) {
      const auto ps = period_starts[p];
      const auto pe = (p + 1 < nperiods) ? period_starts[p + 1] : blocks.size();
      double dur = 0.0;
      for (size_t i = ps; i < pe; ++i) {
        dur += blocks[i].duration();
      }
      pdur[p] = dur;
    }

    BIndexHolder<ColIndex> hcols;
    BIndexHolder<ColIndex> wmcols;
    BIndexHolder<ColIndex> ccols;
    BIndexHolder<RowIndex> st_rows;
    BIndexHolder<RowIndex> hr_rows;
    BIndexHolder<RowIndex> wr_rows;
    map_reserve(hcols, nperiods);
    map_reserve(wmcols, nperiods);
    map_reserve(ccols, nperiods);
    map_reserve(st_rows, nperiods);
    map_reserve(hr_rows, nperiods);
    map_reserve(wr_rows, nperiods);

    for (size_t p = 0; p < nperiods; ++p) {
      const auto rep_buid = blocks[period_starts[p]].uid();
      const auto vcol_it = vcols.find(rep_buid);
      if (vcol_it == vcols.end()) {
        continue;
      }

      const auto bctx =
          make_block_context(scenario.uid(), stage.uid(), rep_buid);
      const auto& rep_block = blocks[period_starts[p]];

      // Create tier variables with their respective costs
      const auto h_cost =
          CostHelper::block_ecost(scenario, stage, rep_block, hot_cost);
      auto hcol = lp.add_col({
          .lowb = 0.0,
          .uppb = 1.0,
          .cost = h_cost,
          .is_integer = !is_relax,
          .class_name = cname,
          .variable_name = HotStartName,
          .variable_uid = cuid,
          .context = bctx,
      });
      hcols[rep_buid] = hcol;

      const auto wm_cost =
          CostHelper::block_ecost(scenario, stage, rep_block, warm_cost);
      auto wmcol = lp.add_col({
          .lowb = 0.0,
          .uppb = 1.0,
          .cost = wm_cost,
          .is_integer = !is_relax,
          .class_name = cname,
          .variable_name = WarmStartName,
          .variable_uid = cuid,
          .context = bctx,
      });
      wmcols[rep_buid] = wmcol;

      const auto c_cost =
          CostHelper::block_ecost(scenario, stage, rep_block, cold_cost);
      auto ccol = lp.add_col({
          .lowb = 0.0,
          .uppb = 1.0,
          .cost = c_cost,
          .is_integer = !is_relax,
          .class_name = cname,
          .variable_name = ColdStartName,
          .variable_uid = cuid,
          .context = bctx,
      });
      ccols[rep_buid] = ccol;

      // C8: v[p] - y_hot[p] - y_warm[p] - y_cold[p] = 0
      {
        auto row =
            SparseRow {
                .class_name = cname,
                .constraint_name = StartupTypeName,
                .variable_uid = cuid,
                .context = bctx,
            }
                .equal(0.0);
        row[vcol_it->second] = 1.0;
        row[hcol] = -1.0;
        row[wmcol] = -1.0;
        row[ccol] = -1.0;
        st_rows[rep_buid] = lp.add_row(std::move(row));
      }

      // Count periods back from p that cover hot_time / cold_time hours
      double accum = 0.0;
      size_t hot_periods = 0;
      for (size_t q = p; q > 0 && accum < hot_time; --q) {
        accum += pdur[q - 1];
        ++hot_periods;
      }

      accum = 0.0;
      size_t cold_periods = 0;
      for (size_t q = p; q > 0 && accum < cold_time; --q) {
        accum += pdur[q - 1];
        ++cold_periods;
      }

      // C9: y_hot[p] ≤ Σ w[q] for q in [p-hot_periods, p-1]
      {
        auto row =
            SparseRow {
                .class_name = cname,
                .constraint_name = HotStartName,
                .variable_uid = cuid,
                .context = bctx,
            }
                .less_equal(0.0);
        row[hcol] = 1.0;
        for (size_t q = (p > hot_periods ? p - hot_periods : 0); q < p; ++q) {
          const auto q_buid = blocks[period_starts[q]].uid();
          const auto wcol_it = wcols.find(q_buid);
          if (wcol_it != wcols.end()) {
            row[wcol_it->second] = -1.0;
          }
        }
        if (p < hot_periods && initial_hours_offline < hot_time) {
          row.uppb = 1.0;
        }
        hr_rows[rep_buid] = lp.add_row(std::move(row));
      }

      // C10: y_warm[p] ≤ Σ w[q] for q in [p-cold_periods, p-hot_periods-1]
      {
        auto row =
            SparseRow {
                .class_name = cname,
                .constraint_name = WarmStartName,
                .variable_uid = cuid,
                .context = bctx,
            }
                .less_equal(0.0);
        row[wmcol] = 1.0;
        const auto warm_start =
            p > cold_periods ? p - cold_periods : static_cast<size_t>(0);
        const auto warm_end =
            p > hot_periods ? p - hot_periods : static_cast<size_t>(0);
        for (size_t q = warm_start; q < warm_end; ++q) {
          const auto q_buid = blocks[period_starts[q]].uid();
          const auto wcol_it = wcols.find(q_buid);
          if (wcol_it != wcols.end()) {
            row[wcol_it->second] = -1.0;
          }
        }
        if (p < cold_periods && initial_hours_offline >= hot_time
            && initial_hours_offline < cold_time)
        {
          row.uppb = 1.0;
        }
        wr_rows[rep_buid] = lp.add_row(std::move(row));
      }
    }

    if (!hcols.empty()) {
      hot_start_cols_[st_key] = std::move(hcols);
    }
    if (!wmcols.empty()) {
      warm_start_cols_[st_key] = std::move(wmcols);
    }
    if (!ccols.empty()) {
      cold_start_cols_[st_key] = std::move(ccols);
    }
    if (!st_rows.empty()) {
      startup_type_rows_[st_key] = std::move(st_rows);
    }
    if (!hr_rows.empty()) {
      hot_start_rows_[st_key] = std::move(hr_rows);
    }
    if (!wr_rows.empty()) {
      warm_start_rows_[st_key] = std::move(wr_rows);
    }
  }

  // Store index holders
  if (!ucols.empty()) {
    status_cols_[st_key] = std::move(ucols);
  }
  if (!vcols.empty()) {
    startup_cols_[st_key] = std::move(vcols);
  }
  if (!wcols.empty()) {
    shutdown_cols_[st_key] = std::move(wcols);
  }
  if (!lrows.empty()) {
    logic_rows_[st_key] = std::move(lrows);
  }
  if (!gurows.empty()) {
    gen_upper_rows_[st_key] = std::move(gurows);
  }
  if (!glrows.empty()) {
    gen_lower_rows_[st_key] = std::move(glrows);
  }
  if (!erows.empty()) {
    exclusion_rows_[st_key] = std::move(erows);
  }
  if (!rurows.empty()) {
    ramp_up_rows_[st_key] = std::move(rurows);
  }
  if (!rdrows.empty()) {
    ramp_down_rows_[st_key] = std::move(rdrows);
  }

  return true;
}

bool CommitmentLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto pid = id();

  out.add_col_sol(cname, StatusName, pid, status_cols_);
  out.add_col_cost(cname, StatusName, pid, status_cols_);
  out.add_col_sol(cname, StartupName, pid, startup_cols_);
  out.add_col_cost(cname, StartupName, pid, startup_cols_);
  out.add_col_sol(cname, ShutdownName, pid, shutdown_cols_);
  out.add_col_cost(cname, ShutdownName, pid, shutdown_cols_);

  out.add_row_dual(cname, LogicName, pid, logic_rows_);
  out.add_row_dual(cname, GenUpperName, pid, gen_upper_rows_);
  out.add_row_dual(cname, GenLowerName, pid, gen_lower_rows_);
  out.add_row_dual(cname, ExclusionName, pid, exclusion_rows_);
  out.add_row_dual(cname, RampUpName, pid, ramp_up_rows_);
  out.add_row_dual(cname, RampDownName, pid, ramp_down_rows_);

  for (const auto& scols : segment_cols_) {
    out.add_col_sol(cname, SegmentName, pid, scols);
    out.add_col_cost(cname, SegmentName, pid, scols);
  }
  out.add_row_dual(cname, SegmentName, pid, segment_link_rows_);
  out.add_row_dual(cname, MinUpTimeName, pid, min_up_time_rows_);
  out.add_row_dual(cname, MinDownTimeName, pid, min_down_time_rows_);

  out.add_col_sol(cname, HotStartName, pid, hot_start_cols_);
  out.add_col_cost(cname, HotStartName, pid, hot_start_cols_);
  out.add_col_sol(cname, WarmStartName, pid, warm_start_cols_);
  out.add_col_cost(cname, WarmStartName, pid, warm_start_cols_);
  out.add_col_sol(cname, ColdStartName, pid, cold_start_cols_);
  out.add_col_cost(cname, ColdStartName, pid, cold_start_cols_);
  out.add_row_dual(cname, StartupTypeName, pid, startup_type_rows_);
  out.add_row_dual(cname, HotStartName, pid, hot_start_rows_);
  out.add_row_dual(cname, WarmStartName, pid, warm_start_rows_);

  return true;
}

}  // namespace gtopt
