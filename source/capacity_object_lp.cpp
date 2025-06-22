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
                                   LinearProblem& lp)
{
  if (!scenario.is_first()) {
    return true;
  }

  const auto stage_uid = stage.uid();

  const auto stage_expcap = m_expcap_.at(stage_uid).value_or(0.0);
  const auto stage_expmod = m_expmod_.at(stage_uid).value_or(0.0);
  const auto stage_maxexpcap = stage_expcap * stage_expmod;
  const auto stage_capacity = capacity_at(stage);
  const auto stage_capmax =
      m_capmax_.at(stage_uid).value_or(stage_maxexpcap + stage_capacity);

  const auto stage_hour_capcost =
      m_annual_capcost_.at(stage_uid).value_or(0.0) / hours_per_year;
  const auto stage_derating = m_annual_derating_.at(stage_uid).value_or(0.0)
      * stage.timeinit() / hours_per_year;

  auto prev_stage_capacity = stage_capacity;
  auto prev_capainst_col = std::optional<Index> {};
  auto prev_capacost_col = std::optional<Index> {};

  const auto [prev_stage, prev_phase] = sc.simulation().prev_stage(stage);
  if (prev_phase == nullptr) {
    if (prev_stage != nullptr) {
      prev_stage_capacity = capacity_at(*prev_stage, stage_capacity);
      prev_capainst_col = get_optvalue(capainst_cols, prev_stage->uid());
      prev_capacost_col = get_optvalue(capacost_cols, prev_stage->uid());
    }
  } else {
    if (prev_stage != nullptr) {
      auto process_prev_state =
          [&](const std::string_view col_name) -> std::optional<Index>
      {
        auto key = sv_key(col_name, prev_stage->uid(), scenario.uid());

        if (auto prev_svar = sc.get_state_variable(key); prev_svar) {
          auto col = lp.add_col({.name = t_label(sc, stage, col_name, "ini")});
          prev_svar->get().add_dependent_variable(scenario, stage, col);
          return col;
        }
        return std::nullopt;
      };

      prev_stage_capacity = stage_capacity;
      prev_capainst_col = process_prev_state("capainst");
      prev_capacost_col = process_prev_state("capacost");
    }
  }

  if (!prev_capainst_col && stage_maxexpcap <= 0) {
    return true;
  }

  SparseRow capainst_row {.name = t_label(sc, stage, "capainst")};
  const auto capainst_col = lp.add_col({
      .name = capainst_row.name,
      .lowb = stage_capacity,
      .uppb = stage_capmax,
      .cost = 0.0  // Explicit initialization
  });

  auto capainst_key = sv_key("capainst", stage.uid(), scenario.uid());
  sc.add_state_variable(capainst_key, scenario, stage, capainst_col);

  capainst_row[capainst_col] = -1;

  SparseRow capacost_row {.name = t_label(sc, stage, "capacost")};
  const auto capacost_col = lp.add_col({// capacost variable
                                        .name = capacost_row.name,
                                        .cost = sc.stage_ecost(stage, 1.0)});
  capacost_row[capacost_col] = +1;

  if (stage_maxexpcap > 0) {
    const auto expmod_col = expmod_cols[stage.uid()] =
        lp.add_col({// expmod variable
                    .name = t_label(sc, stage, "expmod"),
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
      capainst_cols.emplace(stage_uid, capainst_col).second;
  const bool capacost_success =
      capacost_cols.emplace(stage_uid, capacost_col).second;
  const bool capainst_row_success =
      capainst_rows
          .emplace(stage_uid, lp.add_row(std::move(capainst_row.equal(dcap))))
          .second;
  const bool capacost_row_success =
      capacost_rows
          .emplace(stage_uid, lp.add_row(std::move(capacost_row.equal(0.0))))
          .second;

  return capainst_success && capacost_success && capainst_row_success
      && capacost_row_success;
}

bool CapacityObjectBase::add_to_output(OutputContext& out) const
{
  out.add_col_sol(m_class_name_, "capainst", id(), capainst_cols);
  out.add_col_sol(m_class_name_, "capacost", id(), capacost_cols);
  out.add_col_sol(m_class_name_, "expmod", id(), expmod_cols);

  out.add_col_cost(m_class_name_, "capainst", id(), capainst_cols);
  out.add_col_cost(m_class_name_, "capacost", id(), capacost_cols);
  out.add_col_cost(m_class_name_, "expmod", id(), expmod_cols);

  out.add_row_dual(m_class_name_, "capainst", id(), capainst_rows);
  out.add_row_dual(m_class_name_, "capacost", id(), capacost_rows);

  return true;
}

}  // namespace gtopt
