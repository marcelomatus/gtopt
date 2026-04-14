/**
 * @file      capacity_object_lp.cpp
 * @brief     Implementation of capacity-constrained object LP formulation
 * @date      Sat May 31 09:00:14 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the linear programming formulation for objects
 * with capacity constraints, including capacity expansion, cost tracking,
 * and output generation.
 */

#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

bool CapacityObjectBase::add_to_lp(SystemContext& sc,
                                   std::string_view ampl_class,
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
  auto prev_capainst_col = std::optional<ColIndex> {};
  auto prev_capacost_col = std::optional<ColIndex> {};

  const auto [prev_stage, prev_phase] = sc.simulation().prev_stage(stage);
  if (prev_phase == nullptr) {
    if (prev_stage != nullptr) {
      prev_stage_capacity = capacity_at(*prev_stage, stage_capacity);
      prev_capainst_col = get_optvalue(capainst_cols, prev_stage->uid());
      prev_capacost_col = get_optvalue(capacost_cols, prev_stage->uid());
    }
  } else {
    if (prev_stage != nullptr) {
      // Cross-phase boundary.  Under parallel phase construction within
      // a scene, the producer-side `StateVariable` in the previous
      // phase may not yet exist when this phase runs, so we cannot
      // call `get_state_variable(prev_key)` here.  Instead we
      // determine deterministically — from this element's own
      // expansion schedules — whether the previous phase will publish
      // capainst/capacost StateVariables (publishing is sticky from
      // the first stage where `stage_maxexpcap > 0`).  When yes, we
      // allocate the dependent columns up-front and queue the
      // producer-side `add_dependent_variable` link to be resolved
      // later by `PlanningLP::tighten_scene_phase_links`.
      bool prev_phase_publishes = false;
      for (const auto& s : sc.simulation().stages()) {
        if (s.index() > prev_stage->index()) {
          break;
        }
        const auto suid = s.uid();
        const auto se_expcap = m_expcap_.at(suid).value_or(0.0);
        const auto se_expmod = m_expmod_.at(suid).value_or(0.0);
        if (se_expcap * se_expmod > 0) {
          prev_phase_publishes = true;
          break;
        }
      }

      prev_stage_capacity = stage_capacity;

      if (prev_phase_publishes) {
        const auto stg_ctx = make_stage_context(scenario.uid(), stage.uid());
        auto make_dependent_col =
            [&](const std::string_view col_name) -> ColIndex
        {
          // Dependent state column: mirrors a state variable from the
          // previous phase.  Marked is_state for SDDP cut I/O; column
          // names are available at LpNamesLevel::all, but
          // state variable I/O uses the StateVariable map (ColIndex-based)
          // directly.  The producer-side link is queued via
          // `defer_state_link` and resolved in
          // `PlanningLP::tighten_scene_phase_links` after parallel phase
          // construction joins.
          auto col = lp.add_col({
              .is_state = true,
              .class_name = m_class_name_,
              .variable_name = col_name,
              .variable_uid = uid(),
              .context = stg_ctx,
          });
          sc.defer_state_link(sv_key_p(scenario, *prev_stage, col_name), col);
          return col;
        };

        prev_capainst_col = make_dependent_col(CapainstName);
        prev_capacost_col = make_dependent_col(CapacostName);
      }
    }
  }

  if (!prev_capainst_col && stage_maxexpcap <= 0) {
    return true;
  }

  const auto stg_ctx = make_stage_context(scenario.uid(), stage.uid());
  const auto capainst_col =
      sc.add_state_col(lp,
                       sv_key_p(scenario, stage, CapainstName),
                       SparseCol {
                           .lowb = stage_capacity,
                           .uppb = stage_capmax,
                           .cost = 0.0,
                           .class_name = m_class_name_,
                           .variable_name = CapainstName,
                           .variable_uid = uid(),
                           .context = stg_ctx,
                       });

  SparseRow capainst_row;
  capainst_row.class_name = m_class_name_;
  capainst_row.constraint_name = CapainstName;
  capainst_row.variable_uid = uid();
  capainst_row.context = stg_ctx;
  capainst_row[capainst_col] = -1;

  const auto capacost_col =
      sc.add_state_col(lp,
                       sv_key_p(scenario, stage, CapacostName),
                       SparseCol {
                           .cost = CostHelper::stage_ecost(stage, 1.0),
                           .class_name = m_class_name_,
                           .variable_name = CapacostName,
                           .variable_uid = uid(),
                           .context = stg_ctx,
                       });

  SparseRow capacost_row;
  capacost_row.class_name = m_class_name_;
  capacost_row.constraint_name = CapacostName;
  capacost_row.variable_uid = uid();
  capacost_row.context = stg_ctx;
  capacost_row[capacost_col] = +1;

  if (stage_maxexpcap > 0) {
    const auto expmod_col = expmod_cols[stage.uid()] = lp.add_col({
        .uppb = stage_expmod,
        .is_integer = m_integer_expmod_
            && !sc.simulation().phases()[stage.phase_index()].is_continuous(),
        .class_name = m_class_name_,
        .variable_name = ExpmodName,
        .variable_uid = uid(),
        .context = stg_ctx,
    });

    capainst_row[expmod_col] = +stage_expcap;
    capacost_row[expmod_col] = -stage_expcap * stage_hour_capcost;
  }

  if (prev_capainst_col) {
    capainst_row[*prev_capainst_col] = +(1 - stage_derating);
  }

  if (prev_capacost_col) {
    capacost_row[*prev_capacost_col] = -1;
  }

  // Store the indices for this scenario and stage
  capainst_cols[stage_uid] = capainst_col;
  capacost_cols[stage_uid] = capacost_col;

  const auto dcap = prev_stage_capacity - stage_capacity;
  capainst_rows[stage_uid] = lp.add_row(std::move(capainst_row.equal(dcap)));
  capacost_rows[stage_uid] = lp.add_row(std::move(capacost_row.equal(0.0)));

  // Register the stage-level capacity-expansion columns with the PAMPL
  // variable registry so user-constraint expressions like
  // `generator("G1").capainst` or `sum(g in generator(all : type="hydro"),
  // g.capacost) <= budget` resolve without each element having to
  // register them individually.
  if (!ampl_class.empty()) {
    sc.add_ampl_variable(
        ampl_class, uid(), CapainstName, scenario, stage, capainst_col);
    sc.add_ampl_variable(
        ampl_class, uid(), CapacostName, scenario, stage, capacost_col);
    if (const auto it = expmod_cols.find(stage.uid()); it != expmod_cols.end())
    {
      sc.add_ampl_variable(
          ampl_class, uid(), ExpmodName, scenario, stage, it->second);
    }
  }

  return true;
}

bool CapacityObjectBase::add_to_output(OutputContext& out) const
{
  out.add_col_sol(m_class_name_, CapainstName, id(), capainst_cols);
  out.add_col_sol(m_class_name_, CapacostName, id(), capacost_cols);
  out.add_col_sol(m_class_name_, ExpmodName, id(), expmod_cols);

  out.add_col_cost(m_class_name_, CapainstName, id(), capainst_cols);
  out.add_col_cost(m_class_name_, CapacostName, id(), capacost_cols);
  out.add_col_cost(m_class_name_, ExpmodName, id(), expmod_cols);

  out.add_row_dual(m_class_name_, CapainstName, id(), capainst_rows);
  out.add_row_dual(m_class_name_, CapacostName, id(), capacost_rows);

  return true;
}

}  // namespace gtopt
