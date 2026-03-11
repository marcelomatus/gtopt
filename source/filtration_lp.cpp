/**
 * @file      filtration_lp.cpp
 * @brief     Implementation of FiltrationLP methods
 * @date      Thu Jul 31 23:33:04 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the linear programming formulation for filtration systems.
 * When piecewise-linear segments are present, the LP constraint
 * coefficients are updated dynamically based on the reservoir volume.
 */

#include <gtopt/filtration_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{
bool FiltrationLP::add_to_lp(const SystemContext& sc,
                             const ScenarioLP& scenario,
                             const StageLP& stage,
                             LinearProblem& lp)
{
  static constexpr std::string_view cname = ClassName.short_name();

  if (!is_active(stage)) {
    return true;
  }

  const auto& waterway = sc.element<WaterwayLP>(waterway_sid());
  const auto& reservoir = sc.element<ReservoirLP>(reservoir_sid());

  const auto& flow_cols = waterway.flow_cols_at(scenario, stage);
  const auto eini_col = reservoir.eini_col_at(scenario, stage);
  const auto efin_col = reservoir.efin_col_at(scenario, stage);

  // Determine effective slope and intercept (RHS).
  // When piecewise segments are present, evaluate at the reservoir's
  // initial volume to select the active segment.
  Real effective_slope = slope();
  Real effective_rhs = constant();

  if (!filtration().segments.empty()) {
    const auto eini_vol = reservoir.reservoir().eini.value_or(0.0);
    const auto coeffs =
        select_filtration_coeffs(filtration().segments, eini_vol);
    effective_slope = coeffs.slope;
    effective_rhs = coeffs.intercept;
  }

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
            .name = sc.lp_label(scenario, stage, block, cname, "filt", uid()),
        }
            .equal(effective_rhs);

    frow[eini_col] = frow[efin_col] = -effective_slope * 0.5;

    frow[fcol] = 1;

    frows[buid] = lp.add_row(std::move(frow));
    fcols[buid] = fcol;
  }

  // storing the indices for this scenario and stage
  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  filtration_rows[st_key] = std::move(frows);
  filtration_cols[st_key] = std::move(fcols);

  // Store the coefficient state for later updates
  m_states_[st_key] = FiltrationState {
      .eini_col = eini_col,
      .efin_col = efin_col,
      .current_slope = effective_slope,
      .current_rhs = effective_rhs,
  };

  return true;
}

bool FiltrationLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();
  const auto pid = id();

  out.add_col_sol(cname, "filtration", pid, filtration_cols);
  out.add_col_cost(cname, "filtration", pid, filtration_cols);
  out.add_row_dual(cname, "filtration", pid, filtration_rows);

  return true;
}

int FiltrationLP::update_lp(SystemLP& sys,
                            const ScenarioLP& scenario,
                            const StageLP& stage,
                            PhaseIndex phase,
                            int iteration)
{
  // Only update when piecewise segments are present
  if (filtration().segments.empty()) {
    return 0;
  }

  auto& li = sys.linear_interface();

  // Determine current reservoir volume:
  //  - first iteration OR first phase → use static initial volume (eini)
  //  - otherwise → read from eini column (fixed to previous-phase efin)
  const auto& rsv = sys.element<ReservoirLP>(reservoir_sid());
  Real volume = rsv.reservoir().eini.value_or(0.0);

  // Use LP-bound volume only when we are past the first iteration AND past
  // the first phase.  In iteration 1 the LP has not yet been solved from a
  // previous phase, so there is no meaningful bound to read back.  In phase 0
  // the eini column is the fixed initial condition, so reservoir().eini is
  // always correct.
  if (iteration > 1 && phase != PhaseIndex {0}) {
    const auto eini_col = rsv.eini_col_at(scenario, stage);
    // eini is fixed as a bound — read col_low (= col_upp = trial value)
    volume = li.get_col_low()[eini_col];
  }

  // Select the active segment for the current volume
  const auto coeffs = select_filtration_coeffs(filtration().segments, volume);

  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  auto& state = m_states_.at(st_key);

  const auto new_slope = coeffs.slope;
  const auto new_rhs = coeffs.intercept;

  // Only update when coefficients actually changed
  if (new_slope == state.current_slope && new_rhs == state.current_rhs) {
    return 0;
  }

  const auto& frows = filtration_rows.at(st_key);
  int count = 0;

  for (const auto& [buid, row] : frows) {
    if (new_slope != state.current_slope) {
      li.set_coeff(row, state.eini_col, -new_slope * 0.5);
      li.set_coeff(row, state.efin_col, -new_slope * 0.5);
    }
    if (new_rhs != state.current_rhs) {
      li.set_rhs(row, new_rhs);
    }
    ++count;
  }

  SPDLOG_TRACE(
      "FiltrationLP uid={}: updated {} constraints "
      "(volume={:.1f}, slope={:.6f}, rhs={:.6f})",
      uid(),
      count,
      volume,
      new_slope,
      new_rhs);

  state.current_slope = new_slope;
  state.current_rhs = new_rhs;

  return count;
}

}  // namespace gtopt
