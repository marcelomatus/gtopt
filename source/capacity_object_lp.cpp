/**
 * @file      capacity_object_lp.cpp
 * @brief     Header of
 * @date      Sat May 31 09:00:14 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{
bool CapacityObjectBase::add_to_lp(SystemContext& sc,
                                   const ScenarioLP& scenario,
                                   const StageLP& stage,
                                   LinearProblem& lp,
                                   const std::string_view cname)
{
  if (!scenario.is_first()) {
    return true;
  }

  const auto stage_index = stage.index();

  const auto stage_expcap = m_expcap_.at(stage_index).value_or(0.0);
  const auto stage_expmod = m_expmod_.at(stage_index).value_or(0.0);
  const auto stage_maxexpcap = stage_expcap * stage_expmod;
  const auto stage_capacity = capacity_at(stage);
  const auto stage_capmax =
      m_capmax_.at(stage_index).value_or(stage_maxexpcap + stage_capacity);

  const auto stage_hour_capcost =
      m_annual_capcost_.at(stage_index).value_or(0.0) / hours_per_year;
  const auto stage_derating = m_annual_derating_.at(stage_index).value_or(0.0)
      * stage.timeinit() / hours_per_year;

  double prev_stage_capacity = stage_capacity;
  std::optional<Index> prev_capainst_col = std::nullopt;
  std::optional<Index> prev_capacost_col = std::nullopt;

  if (const auto stage_index = stage.index(); stage_index) {
    const auto prev_stage_index = StageIndex {stage_index - 1};
    prev_stage_capacity = capacity_at(prev_stage_index, stage_capacity);
    prev_capainst_col = get_optvalue(capainst_cols, prev_stage_index);
    prev_capacost_col = get_optvalue(capacost_cols, prev_stage_index);
  } else {
#ifdef NONE
    if (const auto phase_index = stage.phase_index(); phase_index) {
      auto process_prev_state =
          [&](const std::string_view var_suffix) -> std::optional<Index>
      {
        const auto var_name =
            sc.t_label(stage, cname, var_suffix, "ini", uid());

        if (const auto prev_svar = sc.simulation().get_prev_state_variable(
                var_name, stage, scenario_index);
            prev_svar)
        {
          auto col = lp.add_col({.name = var_name});
          register_state_variable(prev_svar, col);
          return col;
        }
        return std::nullopt;  // Return default Col if no state variable
      };

      prev_stage_capacity = stage_capacity;
      prev_capainst_col = process_prev_state("capainst");
      prev_capacost_col = process_prev_state("capacost");
    }
#endif
  }

  if (!prev_capainst_col && stage_maxexpcap <= 0) {
    return true;
  }

  SparseRow capainst_row {.name = sc.t_label(stage, cname, "capainst", uid())};
  const auto capainst_col = lp.add_col({
      .name = capainst_row.name,
      .lowb = stage_capacity,
      .uppb = stage_capmax,
      .cost = 0.0  // Explicit initialization
  });
  capainst_row[capainst_col] = -1;

  SparseRow capacost_row {.name = sc.t_label(stage, cname, "capacost", uid())};
  const auto capacost_col = lp.add_col({// capacost variable
                                        .name = capacost_row.name,
                                        .cost = sc.stage_ecost(stage, 1.0)});
  capacost_row[capacost_col] = +1;

  if (stage_maxexpcap > 0) {
    const auto expmod_col = expmod_cols[stage_index] =
        lp.add_col({// expmod variable
                    .name = sc.t_label(stage, cname, "expmod", uid()),
                    .uppb = stage_expmod});

    capainst_row[expmod_col] = +stage_expcap;
    capacost_row[expmod_col] = -stage_expcap * stage_hour_capcost;
  }

  if (prev_capainst_col.has_value()) {
    capainst_row[prev_capainst_col.value()] = +(1 - stage_derating);
  }

  if (prev_capacost_col.has_value()) {
    capacost_row[prev_capacost_col.value()] = -1;
  }

  const auto dcap = prev_stage_capacity - stage_capacity;

  const bool capainst_success =
      capainst_cols.emplace(stage_index, capainst_col).second;
  const bool capacost_success =
      capacost_cols.emplace(stage_index, capacost_col).second;
  const bool capainst_row_success =
      capainst_rows
          .emplace(stage_index, lp.add_row(std::move(capainst_row.equal(dcap))))
          .second;
  const bool capacost_row_success =
      capacost_rows
          .emplace(stage_index, lp.add_row(std::move(capacost_row.equal(0.0))))
          .second;

  return capainst_success && capacost_success && capainst_row_success
      && capacost_row_success;
}

bool CapacityObjectBase::add_to_output(OutputContext& out,
                                       std::string_view cname) const
{
  out.add_col_sol(cname, "capainst", id(), capainst_cols);
  out.add_col_sol(cname, "capacost", id(), capacost_cols);
  out.add_col_sol(cname, "expmod", id(), expmod_cols);

  out.add_col_cost(cname, "capainst", id(), capainst_cols);
  out.add_col_cost(cname, "capacost", id(), capacost_cols);
  out.add_col_cost(cname, "expmod", id(), expmod_cols);

  out.add_row_dual(cname, "capainst", id(), capainst_rows);
  out.add_row_dual(cname, "capacost", id(), capacost_rows);

  return true;
}

}  // namespace gtopt
