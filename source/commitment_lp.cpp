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
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reserve_provision_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

CommitmentLP::CommitmentLP(const Commitment& commitment, const InputContext& ic)
    : Base(commitment, ic, ClassName)
    , generator_index_(ic.element_index(generator_sid()))
    , startup_cost_(ic, ClassName, id(), std::move(object().startup_cost))
    , shutdown_cost_(ic, ClassName, id(), std::move(object().shutdown_cost))
    , fuel_cost_(ic, ClassName, id(), std::move(object().fuel_cost))
    , fuel_emission_factor_(
          ic, ClassName, id(), std::move(object().fuel_emission_factor))
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

  static constexpr std::string_view cname = ClassName.short_name();
  const auto cuid = uid();

  // Resolve commitment parameters
  const auto noload = commitment().noload_cost.value_or(0.0);
  const auto stage_startup_cost =
      startup_cost_.optval(stage.uid()).value_or(0.0);
  const auto stage_shutdown_cost =
      shutdown_cost_.optval(stage.uid()).value_or(0.0);
  const auto initial_u = commitment().initial_status.value_or(0.0);
  const auto is_relax = commitment().relax.value_or(false);
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
  const auto stage_fuel_cost =
      has_segments ? fuel_cost_.optval(stage.uid()).value_or(0.0) : 0.0;

  // Pre-size per-segment column holders
  if (has_segments && segment_cols_.empty()) {
    segment_cols_.resize(pmax_segs.size());
  }

  // Resolve fuel_emission_factor [tCO2/GJ] from commitment (like fuel_cost)
  const auto stage_fuel_ef =
      fuel_emission_factor_.optval(stage.uid()).value_or(0.0);

  // Resolve emission parameters from system options
  const auto& emission_cost_field = sc.options().emission_cost();
  // Generator's emission_factor [tCO2/MWh] is used for non-segment cases.
  // When segments are present and fuel_emission_factor is set,
  // emission per segment = fuel_emission_factor × heat_rate_k [tCO2/MWh].
  const auto stage_emission_factor =
      generator_lp.param_emission_factor(stage.uid()).value_or(0.0);

  // Evaluate emission_cost as scalar or stage-indexed array
  double stage_emission_cost = 0.0;
  if (emission_cost_field.has_value()) {
    const auto& ec = *emission_cost_field;
    if (std::holds_alternative<Real>(ec)) {
      stage_emission_cost = std::get<Real>(ec);
    } else if (std::holds_alternative<std::vector<Real>>(ec)) {
      const auto& vec = std::get<std::vector<Real>>(ec);
      const auto sidx = static_cast<size_t>(stage.uid());
      if (sidx < vec.size()) {
        stage_emission_cost = vec[sidx];
      }
    }
  }

  // Resolve startup cost tiers
  const auto opt_hot_cost = commitment().hot_start_cost;
  const auto opt_warm_cost = commitment().warm_start_cost;
  const auto opt_cold_cost = commitment().cold_start_cost;
  const auto opt_hot_time = commitment().hot_start_time;
  const auto opt_cold_time = commitment().cold_start_time;
  const bool has_startup_tiers = opt_hot_cost.has_value()
      && opt_warm_cost.has_value() && opt_cold_cost.has_value()
      && opt_hot_time.has_value() && opt_cold_time.has_value();

  const auto st_key = std::pair {scenario.uid(), stage.uid()};

  BIndexHolder<ColIndex> ucols;
  BIndexHolder<ColIndex> vcols;
  BIndexHolder<ColIndex> wcols;
  BIndexHolder<RowIndex> lrows;
  BIndexHolder<RowIndex> gurows;
  BIndexHolder<RowIndex> glrows;
  BIndexHolder<RowIndex> erows;
  BIndexHolder<RowIndex> rurows;
  BIndexHolder<RowIndex> rdrows;
  map_reserve(ucols, blocks.size());
  map_reserve(vcols, blocks.size());
  map_reserve(wcols, blocks.size());
  map_reserve(lrows, blocks.size());
  map_reserve(gurows, blocks.size());
  map_reserve(glrows, blocks.size());
  map_reserve(erows, blocks.size());
  map_reserve(rurows, blocks.size());
  map_reserve(rdrows, blocks.size());

  ColIndex prev_ucol {};
  ColIndex prev_gcol {};
  bool first_block = true;

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto ctx =
        make_block_context(scenario.uid(), stage.uid(), block.uid());

    const auto gcol_it = generation_cols.find(buid);
    if (gcol_it == generation_cols.end()) {
      continue;
    }
    const auto gcol = gcol_it->second;

    const auto gen_pmax = lp.get_col_uppb(gcol);
    const auto gen_pmin = lp.get_col_lowb(gcol);

    // ── Create u (status) variable ──
    const auto u_cost = CostHelper::block_ecost(scenario, stage, block, noload);
    auto ucol = lp.add_col({
        .lowb = is_must_run ? 1.0 : 0.0,
        .uppb = 1.0,
        .cost = u_cost,
        .is_integer = !is_relax,
        .class_name = cname,
        .variable_name = StatusName,
        .variable_uid = cuid,
        .context = ctx,
    });
    ucols[buid] = ucol;

    // ── Create v (startup) variable ──
    // When startup tiers are active, v[t] cost is zero — cost is carried
    // by the hot/warm/cold tier variables instead.
    const auto v_cost = has_startup_tiers
        ? 0.0
        : CostHelper::block_ecost(scenario, stage, block, stage_startup_cost);
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
    vcols[buid] = vcol;

    // ── Create w (shutdown) variable ──
    const auto w_cost =
        CostHelper::block_ecost(scenario, stage, block, stage_shutdown_cost);
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
    wcols[buid] = wcol;

    // ── C2: Generation upper bound: p - Pmax*u <= 0 ──
    // Widen the generation variable to [0, Pmax] since pmin is now
    // enforced conditionally via Pmin*u.
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

    // ── C1: Logic transition ──
    // u[t] - u[t-1] - v[t] + w[t] = 0
    // For first block: u[0] - v[0] + w[0] = initial_status
    {
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = LogicName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .equal(first_block ? initial_u : 0.0);
      row[ucol] = 1.0;
      if (!first_block) {
        row[prev_ucol] = -1.0;
      }
      row[vcol] = -1.0;
      row[wcol] = 1.0;
      lrows[buid] = lp.add_row(std::move(row));
    }

    // ── C3: Exclusion: v[t] + w[t] <= 1 ──
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
      erows[buid] = lp.add_row(std::move(row));
    }

    // ── C4: Ramp up: p[t] - p[t-1] ≤ RU·u[t-1] + SU·v[t] ──
    // Only when ramp_up or startup_ramp is defined
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
        // RHS already includes the initial contribution.
      } else {
        row[prev_gcol] = -1.0;
        row[prev_ucol] = -ru;
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
        // -p[0] ≤ RD·u_init + SD·(1-u_init) - p_prev
        // RHS includes the initial contribution.
      } else {
        row[prev_gcol] = 1.0;
        row[ucol] = -rd;
        row[wcol] = -sd;
      }
      rdrows[buid] = lp.add_row(std::move(row));
    }

    // ── Emission cost adder on generation variable ──
    // For non-segment generators: use generator's emission_factor [tCO2/MWh]
    // For segment generators: emission is handled per-segment below using
    //   fuel_emission_factor [tCO2/GJ] × heat_rate_k [GJ/MWh]
    if (stage_emission_cost > 0.0 && stage_emission_factor > 0.0
        && !has_segments)
    {
      const auto emission_adder = CostHelper::block_ecost(
          scenario, stage, block, stage_emission_cost * stage_emission_factor);
      gcol_ref.cost += emission_adder;
    }

    // ── Piecewise heat rate curve ──
    // Literature formulation (Morales-España, Carrión & Arroyo):
    //   p = Pmin·u + δ_1 + ... + δ_K              (linking)
    //   0 ≤ δ_k ≤ w_k · u                         (segment bounds)
    //   cost(δ_k) = fuel_cost × heat_rate_k        (per-segment cost)
    //
    // The segment bounds δ_k ≤ w_k·u ensure zero output when uncommitted.
    // Filling order is natural: increasing heat rates → increasing costs.
    if (has_segments && stage_fuel_cost > 0.0) {
      // Zero out the original generation cost — segments carry cost now.
      // Any emission cost that was added above is also zeroed; emission
      // cost is re-added to each segment variable below.
      gcol_ref.cost = 0.0;

      // Emission cost per MWh for segments: prefer fuel_emission_factor × h_k,
      // fall back to generator emission_factor if fuel_emission_factor not set.
      // seg_emission_base is the per-MWh factor BEFORE multiplying by h_k.
      // When fuel_emission_factor is available: emission/MWh = ef × h_k,
      // so seg_emission_base = emission_cost × fuel_emission_factor (multiply
      // by h_k per segment below).
      // When only generator emission_factor: emission/MWh is constant,
      // so seg_emission_base = emission_cost × emission_factor (no h_k factor).
      const bool use_fuel_ef = stage_fuel_ef > 0.0 && stage_emission_cost > 0.0;
      double seg_emission_base = 0.0;
      if (use_fuel_ef) {
        seg_emission_base = stage_emission_cost * stage_fuel_ef;
      } else if (stage_emission_cost > 0.0 && stage_emission_factor > 0.0) {
        seg_emission_base = stage_emission_cost * stage_emission_factor;
      }

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
      for (size_t k = 0; k < pmax_segs.size(); ++k) {
        const auto seg_width = pmax_segs[k] - prev_breakpoint;
        if (seg_width <= 0.0) {
          prev_breakpoint = pmax_segs[k];
          continue;
        }

        // Segment cost = fuel_cost × h_k + emission_adder
        // When fuel_emission_factor is set: emission_adder = ec × ef × h_k
        // Otherwise: emission_adder = ec × gen_emission_factor (constant)
        const auto seg_emission =
            use_fuel_ef ? seg_emission_base * hr_segs[k] : seg_emission_base;
        const auto seg_marginal = (stage_fuel_cost * hr_segs[k]) + seg_emission;
        const auto seg_cost =
            CostHelper::block_ecost(scenario, stage, block, seg_marginal);

        const auto seg_ctx =
            make_block_context(scenario.uid(), stage.uid(), block.uid());

        // Create segment variable δ_k with bounds [0, w_k]
        // The commitment-dependent bound δ_k ≤ w_k·u is enforced by
        // a separate constraint row below.
        auto seg_col = lp.add_col({
            .lowb = 0.0,
            .uppb = seg_width,
            .cost = seg_cost,
            .class_name = cname,
            .variable_name = SegmentName,
            .variable_uid = cuid,
            .context = seg_ctx,
        });

        // Segment bound row: δ_k - w_k·u ≤ 0  →  δ_k ≤ w_k·u
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
        prev_breakpoint = pmax_segs[k];
      }

      segment_link_rows_[st_key][buid] = lp.add_row(std::move(link_row));
    }

    prev_ucol = ucol;
    prev_gcol = gcol;
    first_block = false;
  }

  // ── Reserve-UC integration ──
  // Modify existing reserve provision headroom rows to be conditional on u.
  // Up-provision:   g + r_up ≤ Pmax      →  g + r_up - Pmax·u ≤ 0
  // Down-provision: g - r_dn ≥ Pmin      →  g - r_dn - Pmin·u ≥ 0
  if (!ucols.empty()) {
    for (const auto& rprov : sc.elements<ReserveProvisionLP>()) {
      if (rprov.generator_sid() != generator_sid()) {
        continue;
      }
      for (const auto& block : blocks) {
        const auto buid = block.uid();
        const auto ucol_it = ucols.find(buid);
        if (ucol_it == ucols.end()) {
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

  // ── C6: Min up time ──
  // Σ_{τ=t}^{min(t+UT-1,T)} u[τ] ≥ UT_blocks · v[t]
  // For each block t where v[t] exists, sum u[τ] over the next UT blocks.
  const auto min_up_hours = commitment().min_up_time.value_or(0.0);
  if (min_up_hours > 0.0 && !ucols.empty() && !vcols.empty()) {
    BIndexHolder<RowIndex> mut_rows;
    // Convert hours to block count using block durations
    const auto nblocks = blocks.size();
    for (size_t t = 0; t < nblocks; ++t) {
      const auto buid_t = blocks[t].uid();
      const auto vcol_it = vcols.find(buid_t);
      if (vcol_it == vcols.end()) {
        continue;
      }

      // Count how many blocks from t onward cover min_up_hours
      double accum_hours = 0.0;
      size_t ut_blocks = 0;
      for (size_t tau = t; tau < nblocks && accum_hours < min_up_hours; ++tau) {
        accum_hours += blocks[tau].duration();
        ++ut_blocks;
      }
      if (ut_blocks <= 1) {
        continue;  // trivially satisfied
      }

      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = MinUpTimeName,
              .variable_uid = cuid,
              .context =
                  make_block_context(scenario.uid(), stage.uid(), buid_t),
          }
              .greater_equal(0.0);
      // Σ u[τ] - UT_blocks · v[t] ≥ 0
      for (size_t tau = t; tau < t + ut_blocks && tau < nblocks; ++tau) {
        const auto ucol_it = ucols.find(blocks[tau].uid());
        if (ucol_it != ucols.end()) {
          row[ucol_it->second] = 1.0;
        }
      }
      row[vcol_it->second] = -static_cast<double>(ut_blocks);
      mut_rows[buid_t] = lp.add_row(std::move(row));
    }
    if (!mut_rows.empty()) {
      min_up_time_rows_[st_key] = std::move(mut_rows);
    }
  }

  // ── C7: Min down time ──
  // Σ_{τ=t}^{min(t+DT-1,T)} (1 - u[τ]) ≥ DT_blocks · w[t]
  // Rearranged: -Σ u[τ] ≥ DT_blocks · w[t] - (span)
  //          →  Σ u[τ] + DT_blocks · w[t] ≤ span
  const auto min_down_hours = commitment().min_down_time.value_or(0.0);
  if (min_down_hours > 0.0 && !ucols.empty() && !wcols.empty()) {
    BIndexHolder<RowIndex> mdt_rows;
    const auto nblocks = blocks.size();
    for (size_t t = 0; t < nblocks; ++t) {
      const auto buid_t = blocks[t].uid();
      const auto wcol_it = wcols.find(buid_t);
      if (wcol_it == wcols.end()) {
        continue;
      }

      double accum_hours = 0.0;
      size_t dt_blocks = 0;
      for (size_t tau = t; tau < nblocks && accum_hours < min_down_hours; ++tau)
      {
        accum_hours += blocks[tau].duration();
        ++dt_blocks;
      }
      if (dt_blocks <= 1) {
        continue;
      }

      const auto span = std::min(t + dt_blocks, nblocks) - t;
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = MinDownTimeName,
              .variable_uid = cuid,
              .context =
                  make_block_context(scenario.uid(), stage.uid(), buid_t),
          }
              .less_equal(static_cast<double>(span));
      // Σ u[τ] + DT_blocks · w[t] ≤ span
      for (size_t tau = t; tau < t + dt_blocks && tau < nblocks; ++tau) {
        const auto ucol_it = ucols.find(blocks[tau].uid());
        if (ucol_it != ucols.end()) {
          row[ucol_it->second] = 1.0;
        }
      }
      row[wcol_it->second] = static_cast<double>(dt_blocks);
      mdt_rows[buid_t] = lp.add_row(std::move(row));
    }
    if (!mdt_rows.empty()) {
      min_down_time_rows_[st_key] = std::move(mdt_rows);
    }
  }

  // ── C8/C9/C10: Hot/warm/cold startup cost tiers ──
  // When all tier parameters are defined, create per-block tier variables
  // y_hot, y_warm, y_cold with constraints:
  //   C8: v[t] = y_hot[t] + y_warm[t] + y_cold[t]  (type selection)
  //   C9: y_hot[t] ≤ Σ w[τ] for τ in hot window    (recent shutdown)
  //   C10: y_warm[t] ≤ Σ w[τ] for τ in warm window  (medium offline)
  //   cold start is the residual via C8.
  if (has_startup_tiers && !vcols.empty() && !wcols.empty()) {
    const auto hot_cost = *opt_hot_cost;
    const auto warm_cost = *opt_warm_cost;
    const auto cold_cost = *opt_cold_cost;
    const auto hot_time = *opt_hot_time;
    const auto cold_time = *opt_cold_time;
    const auto initial_hours_offline =
        (initial_u < 0.5) ? commitment().initial_hours.value_or(1e6) : 0.0;

    BIndexHolder<ColIndex> hcols;
    BIndexHolder<ColIndex> wmcols;
    BIndexHolder<ColIndex> ccols;
    BIndexHolder<RowIndex> st_rows;
    BIndexHolder<RowIndex> hr_rows;
    BIndexHolder<RowIndex> wr_rows;

    const auto nblocks = blocks.size();
    for (size_t t = 0; t < nblocks; ++t) {
      const auto buid_t = blocks[t].uid();
      const auto vcol_it = vcols.find(buid_t);
      if (vcol_it == vcols.end()) {
        continue;
      }

      const auto bctx = make_block_context(scenario.uid(), stage.uid(), buid_t);

      // Create tier variables with their respective costs
      const auto h_cost =
          CostHelper::block_ecost(scenario, stage, blocks[t], hot_cost);
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
      hcols[buid_t] = hcol;

      const auto wm_cost =
          CostHelper::block_ecost(scenario, stage, blocks[t], warm_cost);
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
      wmcols[buid_t] = wmcol;

      const auto c_cost =
          CostHelper::block_ecost(scenario, stage, blocks[t], cold_cost);
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
      ccols[buid_t] = ccol;

      // C8: v[t] - y_hot[t] - y_warm[t] - y_cold[t] = 0
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
        st_rows[buid_t] = lp.add_row(std::move(row));
      }

      // Convert hot/cold time thresholds to block indices by accumulating
      // block durations backwards from block t.
      // hot window: blocks in [t - hot_blocks, t-1] where offline < hot_time
      // warm window: blocks in [t - cold_blocks, t - hot_blocks - 1]

      // Count blocks back from t that cover hot_time hours
      double accum = 0.0;
      size_t hot_blocks = 0;
      for (size_t b = t; b > 0 && accum < hot_time; --b) {
        accum += blocks[b - 1].duration();
        ++hot_blocks;
      }

      accum = 0.0;
      size_t cold_blocks = 0;
      for (size_t b = t; b > 0 && accum < cold_time; --b) {
        accum += blocks[b - 1].duration();
        ++cold_blocks;
      }

      // C9: y_hot[t] ≤ Σ w[τ] for τ in [t-hot_blocks, t-1]
      //     + initial contribution if t is near start and unit was offline
      //     for less than hot_time hours
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
        for (size_t b = (t > hot_blocks ? t - hot_blocks : 0); b < t; ++b) {
          const auto wcol_it = wcols.find(blocks[b].uid());
          if (wcol_it != wcols.end()) {
            row[wcol_it->second] = -1.0;
          }
        }
        // Initial condition: if unit was offline at start and offline hours
        // are within hot threshold, add implicit shutdown before horizon
        if (t < hot_blocks && initial_hours_offline < hot_time) {
          row.uppb = 1.0;  // RHS includes initial implicit w
        }
        hr_rows[buid_t] = lp.add_row(std::move(row));
      }

      // C10: y_warm[t] ≤ Σ w[τ] for τ in [t-cold_blocks, t-hot_blocks-1]
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
            t > cold_blocks ? t - cold_blocks : static_cast<size_t>(0);
        const auto warm_end =
            t > hot_blocks ? t - hot_blocks : static_cast<size_t>(0);
        for (size_t b = warm_start; b < warm_end; ++b) {
          const auto wcol_it = wcols.find(blocks[b].uid());
          if (wcol_it != wcols.end()) {
            row[wcol_it->second] = -1.0;
          }
        }
        // Initial condition: offline hours in warm range
        if (t < cold_blocks && initial_hours_offline >= hot_time
            && initial_hours_offline < cold_time)
        {
          row.uppb = 1.0;
        }
        wr_rows[buid_t] = lp.add_row(std::move(row));
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
  static constexpr std::string_view cname = ClassName.full_name();
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
