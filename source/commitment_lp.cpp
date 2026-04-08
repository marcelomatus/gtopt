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

  // Resolve emission parameters from system options
  const auto& emission_cost_field = sc.options().emission_cost();
  const auto stage_emission_factor =
      generator_lp.param_emission_factor(stage.uid()).value_or(0.0);

  // Evaluate emission_cost as scalar or stage-indexed array
  double stage_emission_cost = 0.0;
  if (emission_cost_field.has_value() && stage_emission_factor > 0.0) {
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
    const auto v_cost =
        CostHelper::block_ecost(scenario, stage, block, stage_startup_cost);
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
    if (stage_emission_cost > 0.0 && stage_emission_factor > 0.0) {
      const auto emission_adder = CostHelper::block_ecost(
          scenario, stage, block, stage_emission_cost * stage_emission_factor);
      gcol_ref.cost += emission_adder;
    }

    // ── Piecewise heat rate curve ──
    // Replace the single gcost with K segment variables:
    //   p = Pmin·u + δ_1 + ... + δ_K
    //   cost(δ_k) = fuel_cost × heat_rate_k × block_ecost
    if (has_segments && stage_fuel_cost > 0.0) {
      // Zero out the original generation cost — segments carry cost now
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
      for (size_t k = 0; k < pmax_segs.size(); ++k) {
        const auto seg_width = pmax_segs[k] - prev_breakpoint;
        if (seg_width <= 0.0) {
          prev_breakpoint = pmax_segs[k];
          continue;
        }
        const auto seg_cost = CostHelper::block_ecost(
            scenario, stage, block, stage_fuel_cost * hr_segs[k]);

        const auto seg_ctx =
            make_block_context(scenario.uid(), stage.uid(), block.uid());
        auto seg_col = lp.add_col({
            .lowb = 0.0,
            .uppb = seg_width,
            .cost = seg_cost,
            .class_name = cname,
            .variable_name = SegmentName,
            .variable_uid = cuid,
            .context = seg_ctx,
        });

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

  return true;
}

}  // namespace gtopt
