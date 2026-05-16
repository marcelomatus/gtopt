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
    SPDLOG_WARN("Turbine uid={}: no waterway or flow reference", uid());
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

  return true;
}

}  // namespace gtopt
