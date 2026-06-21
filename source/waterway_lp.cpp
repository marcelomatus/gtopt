/**
 * @file      waterway_lp.cpp
 * @brief     Implementation of waterway LP formulation
 * @date      Wed Jul 30 12:02:36 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements WaterwayLP construction and add_to_lp, which
 * builds LP flow variables and constraints between hydro junctions.
 */

#include <gtopt/cost_helper.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/waterway_lp.hpp>

namespace gtopt
{

WaterwayLP::WaterwayLP(const Waterway& pwaterway, const InputContext& ic)
    : ObjectLP<Waterway>(pwaterway)
    , fmin(ic, Element::class_name, id(), std::move(waterway().fmin))
    , fmin_fcost(
          ic, Element::class_name, id(), std::move(waterway().fmin_fcost))
    , fmax(ic, Element::class_name, id(), std::move(waterway().fmax))
    , capacity(ic, Element::class_name, id(), std::move(waterway().capacity))
    , lossfactor(
          ic, Element::class_name, id(), std::move(waterway().lossfactor))
    , fcost(ic, Element::class_name, id(), std::move(waterway().fcost))
{
}

bool WaterwayLP::add_to_lp(const SystemContext& sc,
                           const ScenarioLP& scenario,
                           const StageLP& stage,
                           LinearProblem& lp)
{
  static constexpr auto ampl_name = Element::class_name.snake_case();

  if (!is_active(stage)) {
    return true;
  }

  // ``junction_b`` is OPTIONAL: when unset the waterway is an outflow,
  // draining its carried flow out of the system at ``junction_a`` (no
  // downstream credit, no synthetic ocean / sink junction needed).
  // Mirrors ``Turbine.junction_b``'s built-in waterway drain mode.
  const auto& junction_a = sc.element<JunctionLP>(junction_a_sid());
  if (!junction_a.is_active(stage)) {
    return true;
  }
  const auto& balance_rows_a = junction_a.balance_rows_at(scenario, stage);

  const BIndexHolder<RowIndex>* balance_rows_b = nullptr;
  if (has_junction_b()) {
    if (junction_a_sid() == junction_b_sid()) {
      return true;
    }
    const auto& junction_b = sc.element<JunctionLP>(junction_b_sid());
    if (!junction_b.is_active(stage)) {
      return true;
    }
    balance_rows_b = &junction_b.balance_rows_at(scenario, stage);
  }

  const auto stage_capacity =
      capacity.at(stage.uid()).value_or(LinearProblem::DblMax);
  const auto stage_lossfactor = sc.stage_lossfactor(stage, lossfactor);
  const auto stage_fcost = fcost.optval(stage.uid());

  const auto& blocks = stage.blocks();

  BIndexHolder<ColIndex> fcols;
  map_reserve(fcols, blocks.size());

  for (auto&& block : blocks) {
    const auto buid = block.uid();

    const auto [block_fmax, block_fmin] =
        sc.block_maxmin_at(stage, block, fmax, fmin, stage_capacity);

    // Soft-`fmin`: when `fmin_fcost` is set (> 0) and there is a real floor
    // (`block_fmin > 0`), relax the hard flow lower bound to 0 and enforce
    // the floor via an `unserved` slack + `fmin_soft` row (added below).
    const auto block_fmin_fcost = fmin_fcost.optval(stage.uid(), buid);
    const bool soft_fmin =
        block_fmin > 0.0 && block_fmin_fcost.value_or(0.0) > 0.0;

    // P1 LP-size: when both bounds are zero the flow variable is
    // fixed at zero — the LP column and the two `brow[...] = ...`
    // coefficient writes contribute nothing.  Skip the whole block
    // (saves 1 col per closed-flow block; balance rows are unchanged).
    if (block_fmax == 0.0 && block_fmin == 0.0) [[unlikely]] {
      continue;
    }

    const auto balance_row_a = balance_rows_a.at(buid);

    auto& brow_a = lp.row_at(balance_row_a);

    //  adding flow variable

    const auto fc = lp.add_col({
        .lowb = soft_fmin ? 0.0 : block_fmin,
        .uppb = block_fmax,
        .cost = stage_fcost
            ? CostHelper::block_ecost(scenario, stage, block, *stage_fcost)
            : 0.0,
        .class_name = Element::class_name.full_name(),
        .variable_name = FlowName,
        .variable_uid = uid(),
        .context = make_block_context(scenario.uid(), stage.uid(), block.uid()),
    });

    fcols[buid] = fc;

    // adding flow to the junction balances, including the losses.
    // Outflow mode (``junction_b`` unset) skips the downstream credit so
    // the water just leaves the system at ``junction_a``.
    brow_a[fc] = -1.0;
    if (balance_rows_b != nullptr) {
      lp.row_at(balance_rows_b->at(buid))[fc] = 1.0 - stage_lossfactor;
    }

    // ── Soft-`fmin` floor ──────────────────────────────────────────────
    // `flow + unserved ≥ fmin`, `unserved ≥ 0` priced at `fmin_fcost`
    // (mirrors GeneratorLP's soft-pmin).  The flow still routes
    // junction_a → junction_b above; this only relaxes the floor so a
    // water-short forced / ecological flow under-delivers at a penalty
    // instead of going infeasible.
    if (soft_fmin) {
      const auto uc = lp.add_col({
          .lowb = 0.0,
          .uppb = block_fmin,
          .cost = CostHelper::block_ecost(
              scenario, stage, block, block_fmin_fcost.value_or(0.0)),
          .class_name = Element::class_name.full_name(),
          .variable_name = FminUnservedName,
          .variable_uid = uid(),
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      });
      auto srow =
          SparseRow {
              .class_name = Element::class_name.full_name(),
              .constraint_name = FminSoftName,
              .variable_uid = uid(),
              .context =
                  make_block_context(scenario.uid(), stage.uid(), block.uid()),
          }
              .greater_equal(block_fmin);
      srow[fc] = 1.0;
      srow[uc] = 1.0;
      [[maybe_unused]] const auto srow_idx = lp.add_row(std::move(srow));
    }
  }

  // Conditional outer-key insertion (line_lp pattern, commit
  // 4604f4d3): only insert the (s, t) outer key when at least one
  // block populated the inner map.  For active waterways `fcols`
  // is non-empty in practice, but aligning with the rest of the
  // codebase removes the post-insert `at(st_key).empty()` guard.
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  if (!fcols.empty()) {
    flow_cols[st_key] = std::move(fcols);
    // Register PAMPL-visible columns.
    sc.add_ampl_variable(
        ampl_name, uid(), FlowName, scenario, stage, flow_cols.at(st_key));
  }

  return true;
}

bool WaterwayLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  const auto pid = id();

  // `flow:cost` is **not** emitted — the waterway flow variable's
  // reduced cost has no dispatch-cost interpretation (unlike a
  // generator) and no downstream tooling consumes
  // `Waterway/flow_cost.*` (verified by grep across scripts/ /
  // guiservice/ / integration_test/, 2026-05-14).  Mirrors the
  // bus.theta:cost / junction.drain:cost drops.
  out.add_col_sol(cname, FlowName, pid, flow_cols);

  return true;
}

}  // namespace gtopt
