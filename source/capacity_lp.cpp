/**
 * @file      capacity_lp.cpp
 * @brief     Header of
 * @date      Sat Mar 29 18:43:24 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <gtopt/capacity_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>

namespace gtopt
{

bool CapacityLP::lazy_add_to_lp(const SystemContext& sc,
                                const ScenarioIndex& scenario_index,
                                const StageIndex& stage_index,
                                LinearProblem& lp) const
{
  constexpr std::string_view cname = "cap";
  if (!sc.is_first_scenario(scenario_index)) {
    return true;
  }

  if (!is_active(stage_index)) {
    return true;
  }

  const auto stage_expcap = expcap.at(stage_index).value_or(0.0);
  const auto stage_capmax = capmax.at(stage_index);
  const auto stage_expmod = expmod.at(stage_index).value_or(0.0);
  const auto stage_maxexpcap = stage_expcap * stage_expmod;

  const auto prev_stage_index = !sc.is_first_stage(stage_index)
      ? OptStageIndex {stage_index - 1}
      : std::nullopt;

  const auto prev_capacity_col =
      get_optvalue_optkey(capacity_cols, prev_stage_index);

  if (!prev_capacity_col.has_value() && stage_maxexpcap <= 0) {
    return true;
  }

  const auto stage_capacity = capacity_at(stage_index);
  const auto stage_hour_capcost =
      annual_capcost.at(stage_index).value_or(0.0) / hours_per_year;
  const auto prev_stage_capacity =
      capacity_or(prev_stage_index, stage_capacity);
  const auto prev_capacost_col =
      get_optvalue_optkey(capacost_cols, prev_stage_index);
  const auto hour_derating =
      annual_derating.at(stage_index).value_or(0.0) / hours_per_year;
  const auto stage_derating =
      hour_derating * sc.stage_duration(prev_stage_index);

  SparseRow capacity_row {.name =
                              sc.t_label(stage_index, cname, "capcity", uid())};
  SparseRow capacost_row {.name =
                              sc.t_label(stage_index, cname, "capcost", uid())};

  const auto capacity_lb = stage_capacity.value_or(0.0);
  const auto capacity_ub = stage_capmax.has_value()
      ? stage_capmax.value()
      : stage_maxexpcap + capacity_lb;

  const auto capacity_col = capacity_cols[stage_index] =
      lp.add_col({// capacity variable
                  .name = capacity_row.name,
                  .lowb = capacity_lb,
                  .uppb = capacity_ub});

  capacity_row[capacity_col] = -1;

  const auto capacost_col = capacost_cols[stage_index] =
      lp.add_col({// capacost variable
                  .name = capacost_row.name,
                  .cost = sc.stage_ecost(stage_index, 1.0)});

  capacost_row[capacost_col] = +1;

  if (stage_maxexpcap > 0) {
    const auto expmod_col = expmod_cols[stage_index] =
        lp.add_col({// expmod variable
                    .name = sc.t_label(stage_index, cname, "expmod", uid()),
                    .uppb = stage_expmod});

    capacity_row[expmod_col] = +stage_expcap;
    capacost_row[expmod_col] = -stage_expcap * stage_hour_capcost;
  }

  if (prev_capacity_col.has_value()) {
    capacity_row[prev_capacity_col.value()] = +(1 - stage_derating);
  }

  if (prev_capacost_col.has_value()) {
    capacost_row[prev_capacost_col.value()] = -1;
  }

  const auto delta_capacity =
      prev_stage_capacity.value_or(0.0) - stage_capacity.value_or(0.0);

  capacity_rows[stage_index] =
      lp.add_row(std::move(capacity_row.equal(delta_capacity)));
  capacost_rows[stage_index] = lp.add_row(std::move(capacost_row.equal(0)));

  return true;
}

bool CapacityLP::add_to_output(OutputContext& out) const
{
  constexpr std::string_view cname = "Capacity";
  const auto pid = id();

  out.add_col_sol(cname, "capacity", pid, capacity_cols);
  out.add_col_sol(cname, "capacost", pid, capacost_cols);
  out.add_col_sol(cname, "expmod", pid, expmod_cols);

  out.add_col_cost(cname, "capacity", pid, capacity_cols);
  out.add_col_cost(cname, "capacost", pid, capacost_cols);
  out.add_col_cost(cname, "expmod", pid, expmod_cols);

  out.add_row_dual(cname, "capacity", pid, capacity_rows);
  out.add_row_dual(cname, "capacost", pid, capacost_rows);

  return true;
}

}  // namespace gtopt
