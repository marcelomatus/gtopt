/**
 * @file      reservoir_efficiency_lp.cpp
 * @brief     Implementation of ReservoirEfficiencyLP and coefficient updates
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
#include <gtopt/reservoir_efficiency_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

// ── add_to_lp ───────────────────────────────────────────────────────────────

bool ReservoirEfficiencyLP::add_to_lp(const SystemContext& sc,
                                      const ScenarioLP& scenario,
                                      const StageLP& stage,
                                      [[maybe_unused]] LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  // Locate the turbine and its stored conversion rows and flow columns
  const auto& turbine = sc.element<TurbineLP>(turbine_sid());
  const auto& waterway =
      sc.element<WaterwayLP>(WaterwayLPSId {turbine.turbine().waterway});

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

bool ReservoirEfficiencyLP::add_to_output(
    [[maybe_unused]] OutputContext& out) const
{
  // No direct output for efficiency elements — the updated conversion
  // rate is reflected in the turbine's conversion dual output.
  return true;
}

// ── update_conversion_coeff ─────────────────────────────────────────────────

auto ReservoirEfficiencyLP::update_conversion_coeff(LinearInterface& li,
                                                    ScenarioUid suid,
                                                    StageUid tuid,
                                                    Real volume) const -> int
{
  if (!has_coeff_indices(suid, tuid)) {
    return 0;
  }

  const auto new_rate = compute_efficiency(volume);
  const auto& bmap = coeff_indices_at(suid, tuid);
  int count = 0;

  for (const auto& [buid, ci] : bmap) {
    // Turbine conversion row: generation − rate × flow = 0
    // The flow coefficient is stored as −rate
    li.set_coeff(ci.row, ci.col, -new_rate);
    ++count;
  }

  SPDLOG_TRACE(
      "ReservoirEfficiency uid={}: updated {} coeffs "
      "(volume={:.1f}, rate={:.6f})",
      uid(),
      count,
      volume,
      new_rate);

  return count;
}

// ── Generalized LP coefficient update ───────────────────────────────────────

int update_lp_coefficients(
    SystemLP& system_lp,
    const OptionsLP& options,
    const std::function<Real(const ReservoirLPSId&)>& reservoir_volume,
    int iteration)
{
  auto& li = system_lp.linear_interface();

  // Check solver capability once
  if (!li.supports_set_coeff()) {
    if (iteration == 0) {
      SPDLOG_WARN(
          "update_lp_coefficients: set_coeff unsupported by solver, "
          "using static conversion rates (mean efficiency)");
    }
    // Cannot modify matrix coefficients — turbine keeps the static
    // conversion_rate set during TurbineLP::add_to_lp().
    return 0;
  }

  int total = 0;

  // Iterate over all (scenario, stage) pairs in this SystemLP
  for (auto&& stage : system_lp.phase().stages()) {
    for (auto&& scenario : system_lp.scene().scenarios()) {
      const auto suid = scenario.uid();
      const auto tuid = stage.uid();

      // 1. Turbine efficiency updates (ReservoirEfficiencyLP)
      for (auto& eff : system_lp.elements<ReservoirEfficiencyLP>()) {
        // Check per-element skip count
        const auto skip = eff.effective_update_skip(options);
        if (iteration > 0 && skip > 0 && (iteration % (skip + 1)) != 0) {
          continue;
        }

        const auto vol = reservoir_volume(eff.reservoir_sid());
        total += eff.update_conversion_coeff(li, suid, tuid, vol);
      }

      // 2. Future: filtration updates, linearised line-loss updates, etc.
      //    Add additional update loops here following the same pattern.
    }
  }

  return total;
}

}  // namespace gtopt
