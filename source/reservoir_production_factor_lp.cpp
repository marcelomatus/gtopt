/**
 * @file      reservoir_production_factor_lp.cpp
 * @brief     Implementation of ReservoirProductionFactorLP and coefficient
 * updates
 * @date      Mon Mar 10 17:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the LP registration, per-element coefficient update, and the
 * generalized `update_lp()` hook used by the SDDP solver.
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
  const auto eff = turbine.stage_efficiency(stage.uid());

  const auto& blocks = stage.blocks();

  BCoeffMap bmap;
  map_reserve(bmap, blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();
    if (conv_rows.contains(buid) && flow_cols.contains(buid)) {
      bmap[buid] = CoeffIndex {
          .row = conv_rows.at(buid),
          .col = flow_cols.at(buid),
          .efficiency = eff,
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

// ── update_lp ──────────────────────────────────────────────────────────────

int ReservoirProductionFactorLP::update_lp(SystemLP& sys,
                                           const ScenarioLP& scenario,
                                           const StageLP& stage) const
{
  const auto& rsv = sys.element<ReservoirLP>(reservoir_sid());

  if (!rsv.reservoir().eini.has_value()) {
    return 0;
  }
  const auto default_volume = rsv.reservoir().eini.value_or(0.0);

  const auto vini =
      rsv.physical_eini(sys, scenario, stage, default_volume, reservoir_sid());
  const auto vfin = rsv.physical_efin(sys, scenario, stage, default_volume);
  const Real volume = (vini + vfin) / 2.0;

  return update_conversion_coeff(
      sys.linear_interface(), scenario.uid(), stage.uid(), volume);
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
    // Turbine conversion row: generation − efficiency × prod_factor × flow = 0
    // The flow coefficient is stored as −(efficiency × prod_factor)
    li.set_coeff(ci.row, ci.col, -(ci.efficiency * new_rate));
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

}  // namespace gtopt
