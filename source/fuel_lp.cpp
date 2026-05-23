/**
 * @file      fuel_lp.cpp
 * @brief     Implementation of FuelLP — parameter carrier + max-offtake cap
 * @date      2026-05-16
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * FuelLP resolves the Fuel schedules (price, heat content, emission
 * factors, max offtake) at construction.  Generators consume the
 * resolved schedules via the `param_*` accessors.
 *
 * When `Fuel.max_offtake` is set for a (scenario, stage), `add_to_lp`
 * walks every active `GeneratorLP` whose `Generator.fuel == this_fuel`,
 * collects its per-block generation cols + heat rates, and stamps a
 * single cap row:
 *
 *   Σ_g (heat_rate_g(s, b) · gen_g(s, t, b) · dur_b) ≤ max_offtake(s)
 *
 * Optionally a slack column priced at `max_offtake_cost(s)` is added
 * with coefficient −1 so the cap can be violated at a per-unit price.
 *
 * Mirrors PLEXOS's `FueMaxOffWeek_<fuel>` Constraint objects, which
 * sum heat-rate-weighted dispatch across every generator on a given
 * fuel band.
 */

#include <gtopt/cost_helper.hpp>
#include <gtopt/fuel_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

FuelLP::FuelLP(const Fuel& fuel, const InputContext& ic)
    : Base(fuel, ic, Element::class_name)
    , price_(ic, Element::class_name, id(), std::move(object().price))
    , heat_content_(
          ic, Element::class_name, id(), std::move(object().heat_content))
    , combustion_ef_(ic,
                     Element::class_name,
                     id(),
                     std::move(object().combustion_emission_factor))
    , upstream_ef_(ic,
                   Element::class_name,
                   id(),
                   std::move(object().upstream_emission_factor))
    , max_offtake_(
          ic, Element::class_name, id(), std::move(object().max_offtake))
    , max_offtake_cost_(
          ic, Element::class_name, id(), std::move(object().max_offtake_cost))
{
}

bool FuelLP::add_to_lp(const SystemContext& sc,
                       const ScenarioLP& scenario,
                       const StageLP& stage,
                       LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  const auto stage_uid = stage.uid();
  const auto stage_cap = param_max_offtake(stage_uid);
  if (!stage_cap) {
    // No cap set for this stage — FuelLP stays purely passive.
    return true;
  }

  const auto& blocks = stage.blocks();
  if (blocks.empty()) {
    return true;
  }

  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto scen_uid = scenario.uid();
  const auto stage_ctx = make_stage_context(scen_uid, stage_uid);

  SparseRow cap_row {
      .class_name = cname,
      .constraint_name = MaxOfftakeName,
      .variable_uid = uid(),
      .context = stage_ctx,
  };

  // Walk every GeneratorLP referencing this Fuel.  GeneratorLP runs
  // BEFORE FuelLP in lp_element_types_t, so generation_cols_at() is
  // already populated.
  std::size_t coupled = 0;
  for (const auto& gen : sc.elements<GeneratorLP>()) {
    const auto& fuel_ref = gen.generator().fuel;
    if (!fuel_ref.has_value()) {
      continue;
    }
    // Resolve the Fuel reference (Uid or Name) to a uid.  Generator
    // → Fuel is a SingleId so it may be either form; sc.element<>
    // already normalises both shapes, so we use that for robustness.
    const auto& gfuel = sc.element<FuelLP>(FuelLPSId {fuel_ref.value()});
    if (gfuel.uid() != uid()) {
      continue;
    }
    if (!gen.is_active(stage)) {
      continue;
    }
    // Use the tolerant accessor — `generation_cols_at` throws
    // `flat_map::at` when every block of (scenario, stage) was
    // skipped by GeneratorLP's zero-pmax P1 optimisation (the outer
    // key is then absent).  `lookup_generation_cols` returns an
    // empty inner map in that case, which the per-block loop below
    // handles correctly via the `gcols.find(buid)` check.
    const auto& gcols = gen.lookup_generation_cols(scenario, stage);
    for (const auto& block : blocks) {
      const auto buid = block.uid();
      const auto hr = gen.param_heat_rate(stage_uid, buid).value_or(0.0);
      if (hr <= 0.0) {
        continue;
      }
      const auto it = gcols.find(buid);
      if (it == gcols.end()) {
        continue;
      }
      // Σ (heat_rate · gen · dur) ≤ max_offtake.  Heat rate is in
      // fuel-units / MWh; generation is MW; duration is hours →
      // contribution is fuel-units, matching the cap's units.
      const double existing = cap_row[it->second];
      cap_row[it->second] = existing + (hr * block.duration());
    }
    ++coupled;
  }

  if (coupled == 0) {
    // No active generators reference this fuel at this stage — the
    // cap is trivially satisfied; skip the row to keep the LP sparse.
    return true;
  }

  // Optional slack column for soft cap.  Priced at max_offtake_cost
  // via the 2-arg `cost_factor(prob, disc)` overload (duration
  // defaults to 1.0) — NOT `scenario_stage_ecost`, which would
  // additionally multiply by `stage.duration()`.  Both the slack
  // column (fuel-units total) and `max_offtake_cost` ($/fuel-unit)
  // are stage-quantities, not rates; multiplying by duration would
  // over-count the penalty by the stage length (silently invisible
  // for 1-hour single-block stages, real bug for multi-hour stages).
  if (const auto cost = param_max_offtake_cost(stage_uid); cost && *cost > 0.0)
  {
    const auto slack_cost = *cost
        * CostHelper::cost_factor(scenario.probability_factor(),
                                  stage.discount_factor());
    const auto slack_col = lp.add_col(SparseCol {
        .lowb = 0.0,
        .cost = slack_cost,
        .class_name = cname,
        .variable_name = MaxOfftakeSlackName,
        .variable_uid = uid(),
        .context = stage_ctx,
    });
    cap_row[slack_col] = -1.0;
    max_offtake_slack_cols_[{scen_uid, stage_uid}] = slack_col;
  }

  max_offtake_rows_[{scen_uid, stage_uid}] =
      lp.add_row(std::move(cap_row).less_equal(*stage_cap));

  return true;
}

bool FuelLP::add_to_output(OutputContext& out) const
{
  if (max_offtake_rows_.empty()) {
    return true;
  }

  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto pid = id();

  out.add_row_dual(cname, MaxOfftakeName, pid, max_offtake_rows_);
  if (!max_offtake_slack_cols_.empty()) {
    out.add_col_sol(cname, MaxOfftakeSlackName, pid, max_offtake_slack_cols_);
    out.add_col_cost(cname, MaxOfftakeSlackName, pid, max_offtake_slack_cols_);
  }
  return true;
}

}  // namespace gtopt
