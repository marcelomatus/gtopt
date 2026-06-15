/**
 * @file      turbine_lp.cpp
 * @brief     Implementation of TurbineLP class
 * @date      Thu Jul 31 02:05:38 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the TurbineLP class which provides the linear
 * programming representation of hydroelectric turbines, including their
 * constraints and relationships with other system components.
 */

#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/turbine_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{
TurbineLP::TurbineLP(const Turbine& pturbine, InputContext& ic)
    : ObjectLP<Turbine>(pturbine)
    , production_factor(
          ic, Element::class_name, id(), std::move(turbine().production_factor))
    , efficiency(ic, Element::class_name, id(), std::move(turbine().efficiency))
    , capacity(ic, Element::class_name, id(), std::move(turbine().capacity))
{
}

/**
 * @brief Adds turbine constraints to the linear problem
 *
 * @param sc System context containing component relationships
 * @param scenario Current scenario being processed
 * @param stage Current stage being processed
 * @param lp Linear problem to add constraints to
 * @return true if successful, false on error
 *
 * Adds constraints that enforce the relationship between water flow through
 * the turbine and electrical power generation based on the conversion rate.
 */
bool TurbineLP::add_to_lp(const SystemContext& sc,
                          const ScenarioLP& scenario,
                          const StageLP& stage,
                          LinearProblem& lp)
{
  static constexpr auto ampl_name = Element::class_name.snake_case();

  if (!is_active(stage)) {
    return true;
  }

  const auto stage_production_factor =
      production_factor.at(stage.uid()).value_or(1.0);
  const auto stage_efficiency = efficiency.at(stage.uid()).value_or(1.0);
  const auto stage_conversion_rate = stage_efficiency * stage_production_factor;

  const auto stage_capacity = capacity.at(stage.uid());

  auto&& blocks = stage.blocks();

  const auto& generator = sc.element<GeneratorLP>(generator_sid());

  BIndexHolder<RowIndex> rrows;
  BIndexHolder<RowIndex> crows;
  map_reserve(rrows, blocks.size());
  map_reserve(crows, blocks.size());

  if (uses_flow()) {
    // ── Flow-connected turbine ─────────────────────────────────────
    // Use the FlowLP's column variable directly.  The flow column has
    // lowb == uppb == discharge[block], so the constraint:
    //   gen_power <= efficiency × production_factor × flow_col
    // automatically adapts when FlowLP::update_aperture changes
    // the flow column bounds — no separate aperture update needed.
    const auto& flow_lp = sc.element<FlowLP>(flow_sid());

    for (auto&& block : blocks) {
      const auto buid = block.uid();
      // Optional lookups so the zero-bound P1 skip in generator_lp /
      // flow_lp doesn't trip a `flat_map::at` here.  When either the
      // generation column or the flow column is missing for this
      // block, the conversion row would have no LP unknown to bind —
      // skip the row entirely (no constraint required when the
      // producer dropped its variable).
      const auto gcol_opt =
          generator.lookup_generation_col(scenario, stage, buid);
      const auto dcol_opt = flow_lp.lookup_flow_col(scenario, stage, buid);
      if (!gcol_opt || !dcol_opt) {
        continue;
      }
      const auto gcol = *gcol_opt;
      const auto dcol = *dcol_opt;

      auto rrow = SparseRow {
          .class_name = Element::class_name.full_name(),
          .constraint_name = ConversionName,
          .variable_uid = uid(),
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      };
      rrow[gcol] = 1.0;
      rrow[dcol] = -stage_conversion_rate;

      rrows[buid] = lp.add_row(std::move(rrow.less_equal(0)));
    }
  } else if (uses_junctions()) {
    // ── Built-in waterway turbine ──────────────────────────────────
    // The turbine owns its flow arc: a per-block flow column debits
    // `junction_a`, optionally credits `junction_b` (drains out of the
    // system when unset, for terminal run-to-sea plants), and is
    // converted to power at the linked generator.  This subsumes the
    // separate penstock Waterway + the conversion row into one element.
    const auto& junction_a = sc.element<JunctionLP>(junction_a_sid());
    if (!junction_a.is_active(stage)) {
      return true;
    }
    const auto& balance_rows_a = junction_a.balance_rows_at(scenario, stage);

    // `junction_b` is optional: present ⇒ credit the downstream balance;
    // absent ⇒ drain (the turbined flow leaves the modelled system).
    const BIndexHolder<RowIndex>* balance_rows_b = nullptr;
    if (has_junction_b()) {
      const auto& junction_b = sc.element<JunctionLP>(junction_b_sid());
      if (!junction_b.is_active(stage)) {
        return true;
      }
      balance_rows_b = &junction_b.balance_rows_at(scenario, stage);
    }

    BIndexHolder<ColIndex> fcols;
    map_reserve(fcols, blocks.size());

    const auto use_drain = drain();
    for (auto&& block : blocks) {
      const auto buid = block.uid();
      // Tolerate the zero-pmax P1 skip in generator_lp: no generation
      // column means there is no power unknown to bind, so the whole
      // flow arc + conversion row for this block is dropped.
      const auto gcol_opt =
          generator.lookup_generation_col(scenario, stage, buid);
      if (!gcol_opt) {
        continue;
      }
      const auto gcol = *gcol_opt;

      // Turbine-owned flow variable.  Turbines have no expansion model
      // (plain ObjectLP, no CapacityBase), so the fixed per-stage capacity
      // is imposed directly as the column upper bound rather than a separate
      // single-variable CapacityName row.  This keeps turbine_flow in only
      // the conversion + junction-balance equalities so presolve can
      // substitute it out; the capacity shadow price becomes the column
      // reduced cost instead of a row dual.
      const auto fc = lp.add_col({
          .lowb = 0.0,
          .uppb = stage_capacity ? *stage_capacity : LinearProblem::DblMax,
          .cost = 0.0,
          .class_name = Element::class_name.full_name(),
          .variable_name = FlowName,
          .variable_uid = uid(),
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      });
      fcols[buid] = fc;

      // hydraulic balance: debit junction_a, credit junction_b (if any)
      lp.row_at(balance_rows_a.at(buid))[fc] = -1.0;
      if (balance_rows_b != nullptr) {
        lp.row_at(balance_rows_b->at(buid))[fc] = 1.0;
      }

      // conversion: gen_power == conversion_rate × flow (≤ when draining)
      auto rrow = SparseRow {
          .class_name = Element::class_name.full_name(),
          .constraint_name = ConversionName,
          .variable_uid = uid(),
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      };
      rrow[fc] = -stage_conversion_rate;
      rrow[gcol] = 1.0;
      rrows[buid] =
          lp.add_row(std::move(use_drain ? rrow.less_equal(0) : rrow.equal(0)));
    }

    const auto st_key = std::tuple {scenario.uid(), stage.uid()};
    if (!fcols.empty()) {
      flow_cols[st_key] = std::move(fcols);
      sc.add_ampl_variable(
          ampl_name, uid(), FlowName, scenario, stage, flow_cols.at(st_key));
    }
  } else if (turbine().waterway.has_value()) {
    // ── Waterway-connected turbine (traditional) ───────────────────
    const auto& waterway = sc.element<WaterwayLP>(waterway_sid());

    const auto use_drain = drain();
    for (auto&& block : blocks) {
      const auto buid = block.uid();
      // Same rationale as the flow-connected branch: tolerate
      // zero-bound P1 skips in waterway_lp / generator_lp.
      const auto fcol_opt = waterway.lookup_flow_col(scenario, stage, buid);
      const auto gcol_opt =
          generator.lookup_generation_col(scenario, stage, buid);
      if (!fcol_opt || !gcol_opt) {
        continue;
      }
      const auto fcol = *fcol_opt;
      const auto gcol = *gcol_opt;

      auto rrow = SparseRow {
          .class_name = Element::class_name.full_name(),
          .constraint_name = ConversionName,
          .variable_uid = uid(),
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      };
      rrow[fcol] = -stage_conversion_rate;
      rrow[gcol] = 1.0;

      rrows[buid] =
          lp.add_row(std::move(use_drain ? rrow.less_equal(0) : rrow.equal(0)));

      if (stage_capacity) {
        auto crow =
            SparseRow {
                .class_name = Element::class_name.full_name(),
                .constraint_name = CapacityName,
                .variable_uid = uid(),
                .context = make_block_context(
                    scenario.uid(), stage.uid(), block.uid()),
            }
                .less_equal(*stage_capacity);
        crow[fcol] = 1;

        crows[buid] = lp.add_row(std::move(crow));
      }
    }
  } else {
    SPDLOG_WARN("Turbine uid={}: no waterway, flow or junction_a reference",
                uid());
    return false;
  }

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  conversion_rows[st_key] = std::move(rrows);
  capacity_rows[st_key] = std::move(crows);

  // Register the turbine's "generation" attribute under the turbine's own
  // uid + ampl_name, pointing at the associated generator's per-block
  // generation column map.  Safe: the generator's generation_cols map is
  // owned by the GeneratorLP instance and persists for the full solve.
  // `lookup_generation_cols` returns an empty map (not a throw) when
  // every block of (scenario, stage) was skipped by the P1 zero-pmax
  // optimization — registering an empty ampl variable would no-op,
  // so the `!empty()` guard skips the call entirely.
  const auto& gen_cols = generator.lookup_generation_cols(scenario, stage);
  if (!gen_cols.empty()) {
    sc.add_ampl_variable(ampl_name,
                         uid(),
                         GeneratorLP::GenerationName,
                         scenario,
                         stage,
                         gen_cols);
  }

  return true;
}

/**
 * @brief Adds turbine dual variables to the output context
 *
 * @param out Output context to write results to
 * @return true if successful, false on error
 *
 * Outputs the dual variables associated with the turbine's conversion
 * rate constraints for sensitivity analysis.
 */
bool TurbineLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();

  out.add_row_dual(cname, ConversionName, id(), conversion_rows);
  out.add_row_dual(cname, CapacityName, id(), capacity_rows);

  // Built-in waterway mode: emit the turbine-owned flow solution (empty
  // for waterway/flow-connected turbines, which own no flow column).
  out.add_col_sol(cname, FlowName, id(), flow_cols);

  return true;
}

}  // namespace gtopt
