/**
 * @file      junction_lp.cpp
 * @brief     Implementation of junction LP formulation
 * @date      Tue Jul 29 23:08:29 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This file implements the linear programming formulation for power system
 * junctions, including flow balance constraints and drain effects.
 */

#include <gtopt/junction_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

bool JunctionLP::add_to_lp(const SystemContext& sc,
                           const ScenarioLP& scenario,
                           const StageLP& stage,
                           LinearProblem& lp)
{
  static constexpr auto ampl_name = Element::class_name.snake_case();

  // Skip inactive junctions for this stage
  if (!is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();
  if (blocks.empty()) {
    return false;
  }

  // Reserve space for balance rows and drain columns
  BIndexHolder<RowIndex> brows;
  BIndexHolder<ColIndex> dcols;
  map_reserve(brows, blocks.size());
  map_reserve(dcols, blocks.size());

  const bool add_drain_col = drain();
  // Optional Junction fields ``drain_capacity`` / ``drain_cost`` let
  // plp2gtopt / plexos2gtopt encode PLP's ``VertMax`` / ``CVert``
  // (and PLEXOS's ``Storage.Max Spill`` / ``Spill Penalty``)
  // directly on the central's own junction, collapsing the legacy
  // synthetic ``<central>_ocean`` Junction + ``_ver`` Waterway pair
  // into a single ``Junction{drain: true, drain_capacity,
  // drain_cost}`` row.  Defaults (uppb = +∞, cost = 0) preserve the
  // legacy free-drain behaviour for junctions that don't carry the
  // new fields.
  const double drain_uppb = junction().drain_capacity.value_or(DblMax);
  const double drain_cost = junction().drain_cost.value_or(0.0);

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    // Create balance row for this block
    auto brow = SparseRow {
        .class_name = Element::class_name.full_name(),
        .constraint_name = BalanceName,
        .variable_uid = uid(),
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    };

    // Add drain column if needed
    if (add_drain_col) {
      const auto dcol = lp.add_col({
          .uppb = drain_uppb,
          .cost = drain_cost,
          .class_name = Element::class_name.full_name(),
          .variable_name = DrainName,
          .variable_uid = uid(),
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      });
      dcols[buid] = dcol;
      brow[dcol] = -1.0;  // Drain coefficient
    }

    brows[buid] = lp.add_row(std::move(brow));
  }

  // Store indices for this scenario and stage.  The outer-key
  // insertion for ``drain_cols`` is conditional on a non-empty inner
  // map — mirrors the line_lp pattern (commit 4604f4d3) so the
  // ``add_to_output`` and AMPL-registration paths can short-circuit
  // for junctions where ``drain()`` is false.  ``balance_rows`` is
  // always populated for active junctions, so its outer-key
  // assignment stays unconditional.
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  balance_rows[st_key] = std::move(brows);
  if (!dcols.empty()) {
    drain_cols[st_key] = std::move(dcols);
    // Register PAMPL-visible columns — drain only exists when enabled.
    sc.add_ampl_variable(
        ampl_name, uid(), DrainName, scenario, stage, drain_cols.at(st_key));
  }

  return true;
}

bool JunctionLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto pid = id();

  // `drain:cost` is **not** emitted — the drain column is constructed
  // with `.cost = 0.0` (no objective contribution), so its reduced
  // cost is zero by construction.  No downstream tooling consumes
  // `Junction/drain_cost.*` (verified by grep across scripts/ /
  // guiservice/ / integration_test/, 2026-05-14).  Mirrors the
  // line.flow:cost / bus.theta:cost drops.
  out.add_col_sol(cname, DrainName, pid, drain_cols);
  out.add_row_dual(cname, BalanceName, pid, balance_rows);

  return true;
}

}  // namespace gtopt
