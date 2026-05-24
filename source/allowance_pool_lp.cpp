/**
 * @file      allowance_pool_lp.cpp
 * @brief     Implementation of AllowancePoolLP (Phase 2 — banking only)
 * @date      Sun May 24 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Mirror of ``source/lng_terminal_lp.cpp``, retargeted to tCO₂
 * allowances.  Phase 2 wires only the free-allocation inflow + the
 * StorageLP banking carry; emissions-consumption coupling
 * (Phase 3) and auction purchases (Phase 4) land in follow-ups.
 */

#include <gtopt/allowance_pool_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

AllowancePoolLP::AllowancePoolLP(const AllowancePool& pool,
                                 const InputContext& ic)
    : StorageBase(pool, ic, Element::class_name)
    , delivery(ic, Element::class_name, id(), std::move(object().delivery))
{
}

bool AllowancePoolLP::add_to_lp(SystemContext& sc,
                                const ScenarioLP& scenario,
                                const StageLP& stage,
                                LinearProblem& lp)
{
  static constexpr const auto& cname = Element::class_name;
  static constexpr auto ampl_name = Element::class_name.snake_case();
  // tCO₂ is a stock, not a rate — no flow-conversion gymnastics
  // needed.  Free allocation is delivered as ``delivery / duration``
  // each hour and accumulates as banked allowances exactly as LNG
  // accumulates in a tank.
  static constexpr double flow_conversion_rate = 1.0;

  if (!is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();
  if (blocks.empty()) {
    return true;
  }

  const auto energy_scale = sc.options().variable_scale_map().lookup(
      "AllowancePool", "energy", uid());

  // ── Free-allocation inflow columns ────────────────────────────────────
  // Total tCO₂ delivered per stage = ``delivery``.  Convert to a
  // constant per-block rate by dividing by stage duration, then bound
  // the column at that rate (lowb = uppb = rate) so the inflow is
  // an equality, not a free decision — the LP cannot "refuse"
  // allocated allowances.
  const auto stage_delivery = delivery.at(stage.uid()).value_or(0.0);
  const auto stage_duration = stage.duration();

  BIndexHolder<ColIndex> fa_cols;
  map_reserve(fa_cols, blocks.size());

  if (stage_delivery > 0.0 && stage_duration > 0.0) {
    const auto rate = stage_delivery / stage_duration;
    for (auto&& block : blocks) {
      const auto buid = block.uid();
      const auto col = lp.add_col(SparseCol {
          .lowb = rate,
          .uppb = rate,
          .class_name = Element::class_name.full_name(),
          .variable_name = FreeAllocationName,
          .variable_uid = uid(),
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      });
      fa_cols[buid] = col;
    }
  }

  // ── StorageLP banking call ────────────────────────────────────────────
  // finp = free-allocation columns (inflow to the bank).
  // fout = empty — Phase 3 will stamp ``EmissionZone.production`` cols
  //                into the per-block energy rows directly, after the
  //                StorageBase call (same pattern as LngTerminalLP's
  //                generator coupling).
  // drain = unused for allowance pools.
  const BIndexHolder<ColIndex> empty_fout;

  const StorageOptions opts {
      .use_state_variable = allowance_pool().use_state_variable.value_or(true),
      .daily_cycle = allowance_pool().daily_cycle.value_or(false),
      .class_name = Element::class_name.full_name(),
      .variable_uid = uid(),
      .energy_scale = energy_scale,
  };

  if (!StorageBase::add_to_lp(
          cname,
          ampl_name,
          sc,
          scenario,
          stage,
          lp,
          flow_conversion_rate,
          fa_cols,
          [](BlockUid) { return 1.0; },
          empty_fout,
          [](BlockUid) { return 1.0; },
          LinearProblem::DblMax,
          std::nullopt,
          {},
          {},
          opts))
  {
    SPDLOG_CRITICAL("Failed to add storage constraints for allowance pool {}",
                    uid());
    return false;
  }

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  free_allocation_cols[st_key] = std::move(fa_cols);

  // Register PAMPL-visible columns.
  if (!free_allocation_cols.at(st_key).empty()) {
    sc.add_ampl_variable(ampl_name,
                         uid(),
                         FreeAllocationName,
                         scenario,
                         stage,
                         free_allocation_cols.at(st_key));
  }

  return true;
}

bool AllowancePoolLP::add_to_output(OutputContext& out) const
{
  static constexpr const auto& cname = Element::class_name;

  if (!free_allocation_cols.empty()) {
    out.add_col_sol(cname, FreeAllocationName, id(), free_allocation_cols);
    out.add_col_cost(cname, FreeAllocationName, id(), free_allocation_cols);
  }

  return StorageBase::add_to_output(out, cname);
}

}  // namespace gtopt
