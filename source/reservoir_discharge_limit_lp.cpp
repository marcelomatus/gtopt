/**
 * @file      reservoir_discharge_limit_lp.cpp
 * @brief     Implementation of ReservoirDischargeLimitLP methods
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the LP formulation for volume-dependent discharge limits.
 * Creates a stage-average `qeh` variable, block-level averaging constraints,
 * and a stage-level volume-dependent upper bound on discharge.
 * When piecewise-linear segments are present, update_lp() dynamically
 * adjusts the constraint coefficients based on current reservoir volume.
 */

#include <gtopt/input_context.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reservoir_discharge_limit_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

ReservoirDischargeLimitLP::ReservoirDischargeLimitLP(
    const ReservoirDischargeLimit& ddl, [[maybe_unused]] InputContext& ic)
    : ObjectLP<ReservoirDischargeLimit>(ddl)
{
}

bool ReservoirDischargeLimitLP::add_to_lp(const SystemContext& sc,
                                          const ScenarioLP& scenario,
                                          const StageLP& stage,
                                          LinearProblem& lp)
{
  static constexpr std::string_view cname = ClassName.short_name();

  if (!is_active(stage)) {
    return true;
  }

  const auto& ddl = reservoir_discharge_limit();
  if (ddl.segments.empty()) {
    return true;
  }

  const auto& waterway = sc.element<WaterwayLP>(waterway_sid());
  const auto& reservoir = sc.element<ReservoirLP>(reservoir_sid());

  const auto& flow_cols = waterway.flow_cols_at(scenario, stage);
  const auto eini_col = reservoir.eini_col_at(scenario, stage);
  const auto efin_col = reservoir.efin_col_at(scenario, stage);
  const double energy_scale = reservoir.energy_scale();

  // Select initial segment based on reservoir initial volume
  const auto eini_vol = reservoir.reservoir().eini.value_or(0.0);
  const auto coeffs = select_rdl_coeffs(ddl.segments, eini_vol);

  // Convert slope from physical to LP units
  const Real lp_slope = coeffs.slope * energy_scale;

  const auto& blocks = stage.blocks();
  const auto stage_dur = stage.duration();

  const auto st_key = std::pair {scenario.uid(), stage.uid()};

  // 1. Create qeh variable (free, stage-average hourly discharge)
  const auto qeh_col = lp.add_col(SparseCol {
      .name = sc.lp_col_label(scenario, stage, cname, "qeh", uid()),
  });
  qeh_cols[st_key] = qeh_col;

  // 2. Single averaging constraint: qeh - Σ_b (dur_b / dur_stage) × flow_b = 0
  auto avg_row =
      SparseRow {
          .name = sc.lp_row_label(scenario, stage, cname, "qavg", uid()),
      }
          .equal(0.0);

  avg_row[qeh_col] = 1.0;
  for (auto&& block : blocks) {
    const auto fcol = flow_cols.at(block.uid());
    const auto dur_ratio = block.duration() / stage_dur;
    avg_row[fcol] = -dur_ratio;
  }
  avg_rows[st_key] = lp.add_row(std::move(avg_row));

  // 3. Stage-level volume constraint:
  //    qeh - slope × energy_scale × 0.5 × eini
  //        - slope × energy_scale × 0.5 × efin  ≤  intercept
  auto vol_row =
      SparseRow {
          .name = sc.lp_row_label(scenario, stage, cname, "dvol", uid()),
      }
          .less_equal(coeffs.intercept);

  vol_row[qeh_col] = 1.0;
  vol_row[eini_col] = -lp_slope * 0.5;
  vol_row[efin_col] = -lp_slope * 0.5;

  vol_rows[st_key] = lp.add_row(std::move(vol_row));

  // Store state for update_lp
  m_states_[st_key] = RDLState {
      .eini_col = eini_col,
      .efin_col = efin_col,
      .current_slope = coeffs.slope,
      .current_rhs = coeffs.intercept,
  };

  return true;
}

bool ReservoirDischargeLimitLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();
  const auto pid = id();

  out.add_col_sol(cname, "qeh", pid, qeh_cols);
  out.add_row_dual(cname, "dvol", pid, vol_rows);

  return true;
}

int ReservoirDischargeLimitLP::update_lp(SystemLP& sys,
                                         const ScenarioLP& scenario,
                                         const StageLP& stage)
{
  if (reservoir_discharge_limit().segments.size() < 2) {
    return 0;
  }

  auto& li = sys.linear_interface();
  const auto& rsv = sys.element<ReservoirLP>(reservoir_sid());
  const auto default_volume = rsv.reservoir().eini.value_or(0.0);

  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  auto& state = m_states_.at(st_key);

  const auto vini =
      rsv.physical_eini(sys, scenario, stage, default_volume, reservoir_sid());
  const auto vfin = rsv.physical_efin(sys, scenario, stage, default_volume);
  const Real volume = (vini + vfin) / 2.0;

  const auto coeffs =
      select_rdl_coeffs(reservoir_discharge_limit().segments, volume);

  const auto new_slope = coeffs.slope;
  const auto new_rhs = coeffs.intercept;

  if (new_slope == state.current_slope && new_rhs == state.current_rhs) {
    return 0;
  }

  int total = 0;
  const auto row = vol_rows.at(st_key);

  if (new_slope != state.current_slope) {
    li.set_coeff(row, state.eini_col, -new_slope * 0.5);
    li.set_coeff(row, state.efin_col, -new_slope * 0.5);
    ++total;
  }
  if (new_rhs != state.current_rhs) {
    li.set_rhs(row, new_rhs);
    ++total;
  }

  SPDLOG_TRACE(
      "ReservoirDischargeLimitLP uid={}: updated constraints "
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
