/**
 * @file      lng_terminal_lp.cpp
 * @brief     Implementation of LngTerminalLP class
 * @date      Sun Apr 13 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the LngTerminalLP class which provides the linear
 * programming representation of LNG storage terminals, including their
 * volume balance constraints and generator fuel consumption coupling.
 */

#include <gtopt/generator_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lng_terminal_lp.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

LngTerminalLP::LngTerminalLP(const LngTerminal& pterminal,
                             const InputContext& ic)
    : StorageBase(pterminal, ic, ClassName)
    , delivery(ic, ClassName, id(), std::move(terminal().delivery))
    , scost(ic, ClassName, id(), std::move(terminal().scost))
{
}

/**
 * @brief Adds LNG terminal constraints to the linear problem
 *
 * Adds constraints for:
 * - Tank volume balance (via StorageLP)
 * - Regasification (send-out) flow variables
 * - Delivery inflow schedule
 * - Generator fuel consumption coupling via heat rates
 * - Boil-off gas losses (via StorageLP annual_loss)
 * - Venting/spill with penalty cost
 */
bool LngTerminalLP::add_to_lp(SystemContext& sc,
                              const ScenarioLP& scenario,
                              const StageLP& stage,
                              LinearProblem& lp)
{
  static constexpr const auto& cname = ClassName;
  static constexpr auto ampl_name = ClassName.snake_case();

  if (!is_active(stage)) {
    return true;
  }

  auto&& blocks = stage.blocks();

  const auto fcr = flow_conversion_rate();

  // Resolve energy_scale from VariableScaleMap (default 1.0 if not set).
  const double energy_scale =
      sc.options().variable_scale_map().lookup("LngTerminal", "energy", uid());

  // ── Delivery inflow columns ───────────────────────────────────────────
  // Delivery is total m³ per stage. Convert to a constant rate [m³/h]
  // across all blocks by dividing by stage duration.
  const auto stage_delivery = delivery.at(stage.uid()).value_or(0.0);
  const auto stage_duration = stage.duration();

  BIndexHolder<ColIndex> del_cols;
  map_reserve(del_cols, blocks.size());

  if (stage_delivery > 0.0 && stage_duration > 0.0) {
    const auto delivery_rate = stage_delivery / stage_duration;

    for (auto&& block : blocks) {
      const auto buid = block.uid();
      // Fixed-rate delivery: bounds = [rate, rate] (equality)
      const auto dc = lp.add_col(SparseCol {
          .lowb = delivery_rate,
          .uppb = delivery_rate,
          .class_name = ClassName.full_name(),
          .variable_name = DeliveryName,
          .variable_uid = uid(),
          .context =
              make_block_context(scenario.uid(), stage.uid(), block.uid()),
      });
      del_cols[buid] = dc;
    }
  }

  // ── Call StorageBase::add_to_lp ───────────────────────────────────────
  // - finp = delivery columns (inflow to tank)
  // - fout = empty (generator consumption is injected directly into
  //          energy_rows after this call, matching PLP's model where
  //          the only physical outflow is generator fuel consumption)
  // - drain = venting (with spillway_cost/capacity)
  // - annual_loss = BOG
  const BIndexHolder<ColIndex> empty_fout;

  const auto mpf = terminal().mean_production_factor.value_or(
      LngTerminal::default_mean_production_factor);
  const auto stage_scost = sc.state_fail_cost(stage, scost);
  const double lng_scost = stage_scost.value_or(1.0) * mpf;

  const StorageOptions opts {
      .use_state_variable = terminal().use_state_variable.value_or(true),
      .class_name = ClassName.full_name(),
      .variable_uid = uid(),
      .energy_scale = energy_scale,
      .scost = lng_scost,
  };

  if (!StorageBase::add_to_lp(cname,
                              ampl_name,
                              sc,
                              scenario,
                              stage,
                              lp,
                              fcr,
                              del_cols,
                              1.0,
                              empty_fout,
                              1.0,
                              LinearProblem::DblMax,
                              std::nullopt,
                              spillway_cost(),
                              spillway_capacity(),
                              opts))
  {
    SPDLOG_CRITICAL("Failed to add storage constraints for LNG terminal {}",
                    uid());
    return false;
  }

  // ── Generator fuel consumption coupling ───────────────────────────────
  // For each linked generator, inject its fuel consumption into the storage
  // energy balance rows. The energy balance row has the form:
  //   V[t] = V[t-1]×(1-loss) - fcr×fout×dt + fcr×finp×dt - fcr×drain×dt
  // We add:  + fcr × heat_rate × generation × dt
  // (positive = outflow from tank, same sign as fout)
  auto&& energy_rows_map = energy_rows_at(scenario, stage);

  for (const auto& link : terminal().generators) {
    const auto gen_sid = GeneratorLPSId {link.generator};
    const auto& gen = sc.element<GeneratorLP>(gen_sid);

    if (!gen.is_active(stage)) {
      continue;
    }

    auto&& gen_cols = gen.generation_cols_at(scenario, stage);

    for (auto&& block : blocks) {
      const auto buid = block.uid();
      const auto gen_col = gen_cols.at(buid);
      const auto erow_idx = energy_rows_map.at(buid);

      auto& erow = lp.row_at(erow_idx);
      // fuel consumed = heat_rate × generation [MW] × duration [h]
      // × flow_conversion_rate [m³/(m³/h·h)]
      // Since fcr=1.0 by default, this is heat_rate × generation × duration
      // The energy balance expects outflow as positive coefficient.
      erow[gen_col] = fcr * link.heat_rate * block.duration();
    }
  }

  // ── Store column indices ──────────────────────────────────────────────
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  delivery_cols[st_key] = std::move(del_cols);

  // Register element-specific PAMPL columns.
  sc.add_ampl_variable(ampl_name,
                       uid(),
                       DeliveryName,
                       scenario,
                       stage,
                       delivery_cols.at(st_key));

  return true;
}

/**
 * @brief Adds LNG terminal solution variables to the output context
 */
bool LngTerminalLP::add_to_output(OutputContext& out) const
{
  static constexpr const auto& cname = ClassName;

  out.add_col_sol(cname, DeliveryName, id(), delivery_cols);
  out.add_col_cost(cname, DeliveryName, id(), delivery_cols);

  return StorageBase::add_to_output(out, ClassName.full_name());
}

}  // namespace gtopt
