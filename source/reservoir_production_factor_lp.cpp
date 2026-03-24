/**
 * @file      reservoir_production_factor_lp.cpp
 * @brief     Implementation of ReservoirProductionFactorLP and coefficient
 * updates
 * @date      Mon Mar 10 17:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the LP registration, per-element coefficient update, and the
 * generalized `update_lp_coefficients()` hook used by the SDDP solver.
 */

#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/reservoir_production_factor_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

// ── add_to_lp ───────────────────────────────────────────────────────────────

bool ReservoirProductionFactorLP::add_to_lp(const SystemContext& sc,
                                            const ScenarioLP& scenario,
                                            const StageLP& stage,
                                            [[maybe_unused]] LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  // Locate the turbine — may be absent if the central has no electrical bus.
  const TurbineLP* turbine_ptr = nullptr;
  try {
    turbine_ptr = &sc.element<TurbineLP>(turbine_sid());
  } catch (const std::exception&) {
    SPDLOG_WARN(
        "ReservoirProductionFactor uid={}: turbine not found in LP; skipping.",
        uid());
    return true;
  }
  const auto& turbine = *turbine_ptr;

  const WaterwayLP* waterway_ptr = nullptr;
  try {
    waterway_ptr = &sc.element<WaterwayLP>(turbine.waterway_sid());
  } catch (const std::exception&) {
    SPDLOG_WARN(
        "ReservoirProductionFactor uid={}: waterway not found in LP; skipping.",
        uid());
    return true;
  }
  const auto& waterway = *waterway_ptr;

  const auto& conv_rows = turbine.conversion_rows_at(scenario, stage);
  const auto& flow_cols = waterway.flow_cols_at(scenario, stage);

  const auto& blocks = stage.blocks();

  BCoeffMap bmap;
  map_reserve(bmap, blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();
    if (conv_rows.contains(buid) && flow_cols.contains(buid)) {
      bmap[buid] = CoeffIndex {
          .row = conv_rows.at(buid),
          .col = flow_cols.at(buid),
      };
    }
  }

  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  m_coeff_indices_[st_key] = std::move(bmap);

  return true;
}

// ── add_to_output ───────────────────────────────────────────────────────────

bool ReservoirProductionFactorLP::add_to_output(
    [[maybe_unused]] OutputContext& out)
{
  // No direct output for production factor elements — the updated conversion
  // rate is reflected in the turbine's conversion dual output.
  return true;
}

// ── update_conversion_coeff ─────────────────────────────────────────────────

auto ReservoirProductionFactorLP::update_conversion_coeff(LinearInterface& li,
                                                          ScenarioUid suid,
                                                          StageUid tuid,
                                                          Real volume) const
    -> int
{
  if (!has_coeff_indices(suid, tuid)) {
    return 0;
  }

  const auto new_rate = compute_production_factor(volume);
  const auto& bmap = coeff_indices_at(suid, tuid);
  int count = 0;

  for (const auto& [buid, ci] : bmap) {
    // Turbine conversion row: generation − rate × flow = 0
    // The flow coefficient is stored as −rate
    li.set_coeff(ci.row, ci.col, -new_rate);
    ++count;
  }

  SPDLOG_TRACE(
      "ReservoirProductionFactor uid={}: updated {} coeffs "
      "(volume={:.1f}, rate={:.6f})",
      uid(),
      count,
      volume,
      new_rate);

  return count;
}

// ── Generalized LP coefficient update ───────────────────────────────────────

int update_lp_coefficients(SystemLP& system_lp,
                           [[maybe_unused]] const OptionsLP& options,
                           int iteration,
                           PhaseIndex phase)
{
  // Check solver capability once
  if (!LinearInterface::supports_set_coeff()) {
    if (iteration == 0) {
      SPDLOG_WARN(
          "update_lp_coefficients: set_coeff unsupported by solver, "
          "using static conversion rates (mean efficiency)");
    }
    // Cannot modify matrix coefficients — turbines keep the static
    // conversion_rate set during TurbineLP::add_to_lp().
    return 0;
  }

  int total = 0;

  // Iterate over all (scenario, stage) pairs in this SystemLP and dispatch
  // update_lp() to every collection element that satisfies HasUpdateLP.
  // This uses visit_elements with a compile-time if constexpr dispatch, so
  // only types implementing update_lp() (TurbineLP, ReservoirSeepageLP, ...)
  // are called — no separate per-type loops needed.
  for (auto&& stage : system_lp.phase().stages()) {
    for (auto&& scenario : system_lp.scene().scenarios()) {
      visit_elements(system_lp.collections(),
                     [&total, &system_lp, &scenario, &stage, phase, iteration](
                         auto& element) -> bool
                     {
                       using T = std::decay_t<decltype(element)>;
                       if constexpr (HasUpdateLP<T>) {
                         total += element.update_lp(
                             system_lp, scenario, stage, phase, iteration);
                       }
                       return true;
                     });
    }
  }

  return total;
}

}  // namespace gtopt
