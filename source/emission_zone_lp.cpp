/**
 * @file      emission_zone_lp.cpp
 * @brief     LP-active wiring for EmissionZone (production col + balance row)
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Per (scenario, stage) build:
 *
 *   for each block b:
 *     col[production_b]: lowb=0, cost = price · dur_b (when price set)
 *     row[balance_b]:    + production_b  = 0   (sources inject the
 *                                              `-rate · dur` coefficient
 *                                              on each generator
 *                                              dispatch column later)
 *
 *   per stage, if cap set:
 *     row[cap]:  Σ_b production_b  ≤  cap  (+ slack with cap_cost
 *                                          penalty when cap_cost set)
 */

#include <gtopt/cost_helper.hpp>
#include <gtopt/emission_zone_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

EmissionZoneLP::EmissionZoneLP(const EmissionZone& zone, const InputContext& ic)
    : Base(zone, ic, Element::class_name)
    , cap_(ic, Element::class_name, id(), std::move(object().cap))
    , cap_cost_(ic, Element::class_name, id(), std::move(object().cap_cost))
    , price_(ic, Element::class_name, id(), std::move(object().price))
{
}

bool EmissionZoneLP::add_to_lp(const SystemContext& /*sc*/,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  static constexpr std::string_view cname = Element::class_name.full_name();

  const auto& blocks = stage.blocks();
  if (blocks.empty()) {
    return true;
  }

  const auto stage_uid = stage.uid();
  const auto scen_uid = scenario.uid();
  const auto stage_price = param_price(stage_uid);
  const auto stage_cap = param_cap(stage_uid);
  const auto stage_cap_cost = param_cap_cost(stage_uid);

  BIndexHolder<ColIndex> prod_cols;
  BIndexHolder<RowIndex> balance_rows;
  map_reserve(prod_cols, blocks.size());
  map_reserve(balance_rows, blocks.size());

  // ── Per-block production column + balance row ─────────────────────────
  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto block_ctx = make_block_context(scen_uid, stage_uid, buid);

    // Production-column cost: per-ton tax × block duration when price set.
    // Discount + scenario probability + scale_objective are applied by
    // CostHelper::block_ecost (matches every other thermal cost in
    // gtopt — keeps LP physics consistent across element types).
    const double prod_cost = stage_price
        ? CostHelper::block_ecost(scenario, stage, block, *stage_price)
        : 0.0;

    const auto pcol = lp.add_col(SparseCol {
        .lowb = 0.0,
        .cost = prod_cost,
        .class_name = cname,
        .variable_name = ProductionName,
        .variable_uid = uid(),
        .context = block_ctx,
    });
    prod_cols[buid] = pcol;

    SparseRow bal {
        .class_name = cname,
        .constraint_name = BalanceName,
        .variable_uid = uid(),
        .context = block_ctx,
    };
    bal[pcol] = 1.0;
    balance_rows[buid] = lp.add_row(std::move(bal).equal(0.0));
  }

  // Stash for EmissionSourceLP::add_to_lp lookups + output emission.
  const std::tuple st_key {scen_uid, stage_uid};
  production_cols_[st_key] = std::move(prod_cols);
  balance_rows_[st_key] = std::move(balance_rows);

  // ── Optional per-stage cap row ───────────────────────────────────────
  if (stage_cap) {
    SparseRow cap_row {
        .class_name = cname,
        .constraint_name = CapName,
        .variable_uid = uid(),
        .context = make_stage_context(scen_uid, stage_uid),
    };

    // Σ_b production_b ≤ cap   (each block's contribution carries its
    // duration so the sum is a true tonnage figure, not MW·blocks).
    for (const auto& block : blocks) {
      const auto buid = block.uid();
      const auto pcol = production_cols_[st_key][buid];
      cap_row[pcol] = block.duration();
    }

    // Soft cap: introduce a slack column with cap_cost penalty so the
    // LP can violate the cap at a (per-ton) cost rather than infeasing.
    if (stage_cap_cost) {
      // Apply the same probability / discount / scale chain to the
      // slack penalty as we did to the production-col tax above.  Use
      // a representative duration of 1h — the slack is a per-stage
      // quantity, not a per-block one, so the duration cancels out
      // when the cap row aggregates already-duration-scaled
      // production contributions.
      const auto slack_cost =
          CostHelper::scenario_stage_ecost(scenario, stage, *stage_cap_cost);
      const auto slack = lp.add_col(SparseCol {
          .lowb = 0.0,
          .cost = slack_cost,
          .class_name = cname,
          .variable_name = CapSlackName,
          .variable_uid = uid(),
          .context = make_stage_context(scen_uid, stage_uid),
      });
      cap_row[slack] = -1.0;
      cap_slack_cols_[st_key] = slack;
    }

    cap_rows_[st_key] = lp.add_row(std::move(cap_row).less_equal(*stage_cap));
  }

  return true;
}

bool EmissionZoneLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto pid = id();

  // Per-(scenario, stage, block) streams.
  out.add_col_sol(cname, ProductionName, pid, production_cols_);
  out.add_col_cost(cname, ProductionName, pid, production_cols_);
  out.add_row_dual(cname, BalanceName, pid, balance_rows_);

  // Per-(scenario, stage) cap streams.
  out.add_row_dual(cname, CapName, pid, cap_rows_);
  if (!cap_slack_cols_.empty()) {
    out.add_col_sol(cname, CapSlackName, pid, cap_slack_cols_);
    out.add_col_cost(cname, CapSlackName, pid, cap_slack_cols_);
  }

  return true;
}

}  // namespace gtopt
