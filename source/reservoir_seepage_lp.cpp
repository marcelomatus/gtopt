/**
 * @file      reservoir_seepage_lp.cpp
 * @brief     Implementation of ReservoirSeepageLP methods
 * @date      Thu Jul 31 23:33:04 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the linear programming formulation for seepage systems.
 * Per-stage slope/constant schedules (from plpmanfi.dat Parquet files) are
 * read at construction time and applied directly as LP matrix coefficients
 * during add_to_lp() for each stage.  When piecewise-linear segments are
 * present, the LP constraint coefficients are updated dynamically based on
 * the reservoir volume via update_lp().
 */

#include <gtopt/input_context.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reservoir_seepage_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

ReservoirSeepageLP::ReservoirSeepageLP(const ReservoirSeepage& pseepage,
                                       InputContext& ic)
    : ObjectLP<ReservoirSeepage>(pseepage)
    , m_slope_sched_(ic, ClassName, id(), std::move(seepage().slope))
    , m_constant_sched_(ic, ClassName, id(), std::move(seepage().constant))
{
}

bool ReservoirSeepageLP::add_to_lp(const SystemContext& sc,
                                   const ScenarioLP& scenario,
                                   const StageLP& stage,
                                   LinearProblem& lp)
{
  static constexpr std::string_view ampl_class = "seepage";
  static constexpr std::string_view flow_alias = "flow";

  sc.register_ampl_element(ampl_class, id().second, uid());

  if (!is_active(stage)) {
    return true;
  }

  const auto& waterway = sc.element<WaterwayLP>(waterway_sid());
  const auto& reservoir = sc.element<ReservoirLP>(reservoir_sid());

  const auto& flow_cols = waterway.flow_cols_at(scenario, stage);
  const auto eini_col = reservoir.eini_col_at(scenario, stage);
  const auto efin_col = reservoir.efin_col_at(scenario, stage);

  // Determine effective slope and intercept (RHS).
  // Priority: piecewise segments (volume-dependent) > per-stage schedule >
  // scalar default 0.0.
  Real effective_slope = m_slope_sched_.at(stage.uid()).value_or(0.0);
  Real effective_rhs = m_constant_sched_.at(stage.uid()).value_or(0.0);

  if (!seepage().segments.empty()) {
    const auto eini_vol = reservoir.reservoir().eini.value_or(0.0);
    const auto coeffs = select_seepage_coeffs(seepage().segments, eini_vol);
    effective_slope = coeffs.slope;
    effective_rhs = coeffs.intercept;
  }

  // Physical slope — flatten() applies col_scale to matrix coefficients.
  const Real lp_slope = effective_slope;

  const auto& blocks = stage.blocks();

  BIndexHolder<RowIndex> frows;
  BIndexHolder<ColIndex> fcols;
  map_reserve(frows, blocks.size());
  map_reserve(fcols, blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();
    const auto fcol = flow_cols.at(buid);

    auto frow =
        SparseRow {
            .class_name = ClassName.full_name(),
            .constraint_name = SeepageName,
            .variable_uid = uid(),
            .context =
                make_block_context(scenario.uid(), stage.uid(), block.uid()),
        }
            .equal(effective_rhs);

    frow[eini_col] = frow[efin_col] = -lp_slope * 0.5;

    frow[fcol] = 1;

    frows[buid] = lp.add_row(std::move(frow));
    fcols[buid] = fcol;
  }

  // storing the indices for this scenario and stage
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  seepage_rows[st_key] = std::move(frows);
  seepage_cols[st_key] = std::move(fcols);

  // Register PAMPL-visible columns — "flow" and "seepage" both alias
  // the waterway's flow column that the seepage row constrains.
  if (!seepage_cols.at(st_key).empty()) {
    sc.add_ampl_variable(ampl_class,
                         uid(),
                         SeepageName,
                         scenario,
                         stage,
                         seepage_cols.at(st_key));
    sc.add_ampl_variable(ampl_class,
                         uid(),
                         flow_alias,
                         scenario,
                         stage,
                         seepage_cols.at(st_key));
  }

  // Store the coefficient state for later updates
  m_states_[st_key] = ReservoirSeepageState {
      .eini_col = eini_col,
      .efin_col = efin_col,
      .current_slope = effective_slope,
      .current_rhs = effective_rhs,
  };

  return true;
}

bool ReservoirSeepageLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();
  const auto pid = id();

  out.add_col_sol(cname, SeepageName, pid, seepage_cols);
  out.add_col_cost(cname, SeepageName, pid, seepage_cols);
  out.add_row_dual(cname, SeepageName, pid, seepage_rows);

  return true;
}

int ReservoirSeepageLP::update_lp(SystemLP& sys,
                                  const ScenarioLP& scenario,
                                  const StageLP& stage)
{
  if (seepage().segments.empty()) {
    return 0;
  }

  auto& li = sys.linear_interface();
  const auto& rsv = sys.element<ReservoirLP>(reservoir_sid());
  const auto default_volume = rsv.reservoir().eini.value_or(0.0);

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  auto& state = m_states_.at(st_key);

  const auto vini =
      rsv.physical_eini(sys, scenario, stage, default_volume, reservoir_sid());
  const auto vfin = rsv.physical_efin(sys, scenario, stage, default_volume);
  const Real volume = (vini + vfin) / 2.0;

  const auto coeffs = select_seepage_coeffs(seepage().segments, volume);

  const auto new_slope = coeffs.slope;
  const auto new_rhs = coeffs.intercept;

  if (new_slope == state.current_slope && new_rhs == state.current_rhs) {
    return 0;
  }

  int total = 0;
  const auto& frows = seepage_rows.at(st_key);

  for (const auto& [buid, row] : frows) {
    if (new_slope != state.current_slope) {
      li.set_coeff(row, state.eini_col, -new_slope * 0.5);
      li.set_coeff(row, state.efin_col, -new_slope * 0.5);
    }
    if (new_rhs != state.current_rhs) {
      li.set_rhs(row, new_rhs);
    }
    ++total;
  }

  SPDLOG_TRACE(
      "ReservoirSeepageLP uid={}: updated constraints "
      "(volume={:.1f}, slope={:.6f}, rhs={:.6f})",
      uid(),
      volume,
      new_slope,
      new_rhs);

  state.current_slope = new_slope;
  state.current_rhs = new_rhs;

  return total;
}

}  // namespace gtopt
