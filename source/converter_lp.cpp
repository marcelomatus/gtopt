/**
 * @file      converter_lp.cpp
 * @brief     Implementation of converter LP formulation
 * @date      Wed Apr  2 02:12:47 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements ConverterLP construction and add_to_lp, which
 * builds LP variables and constraints for energy conversion rate coupling.
 */

#include <gtopt/battery_lp.hpp>
#include <gtopt/converter_lp.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

ConverterLP::ConverterLP(const Converter& pconverter, InputContext& ic)
    : CapacityBase(pconverter, ic, Element::class_name)
    , conversion_rate(
          ic, Element::class_name, id(), std::move(converter().conversion_rate))
{
}

bool ConverterLP::add_to_lp(SystemContext& sc,
                            const ScenarioLP& scenario,
                            const StageLP& stage,
                            LinearProblem& lp)
{
  static constexpr auto ampl_name = Element::class_name.snake_case();

  if (!CapacityBase::add_to_lp(sc, ampl_name, scenario, stage, lp)) [[unlikely]]
  {
    return false;
  }

  if (!is_active(stage)) [[unlikely]] {
    return true;
  }

  const auto [opt_capacity, capacity_col] = capacity_and_col(stage, lp);

  const auto stage_conversion_rate =
      conversion_rate.at(stage.uid()).value_or(1.0);

  auto&& blocks = stage.blocks();

  auto&& generator = sc.element<GeneratorLP>(generator_sid());
  auto&& gen_cols = generator.generation_cols_at(scenario, stage);

  auto&& demand = sc.element<DemandLP>(demand_sid());
  auto&& demand_cols = demand.load_cols_at(scenario, stage);

  auto&& battery = sc.element<BatteryLP>(battery_sid());
  auto&& finp_cols = battery.finp_cols_at(scenario, stage);
  auto&& fout_cols = battery.fout_cols_at(scenario, stage);

  BIndexHolder<RowIndex> grows;
  BIndexHolder<RowIndex> drows;
  BIndexHolder<RowIndex> crows;
  map_reserve(grows, blocks.size());
  map_reserve(drows, blocks.size());
  map_reserve(crows, blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();

    const auto block_context =
        make_block_context(scenario.uid(), stage.uid(), block.uid());
    const auto gcol = gen_cols.at(buid);
    {
      auto grow =
          SparseRow {
              .class_name = Element::class_name.full_name(),
              .constraint_name = GenerationName,
              .variable_uid = uid(),
              .context = block_context,
          }
              .equal(0);
      const auto ocol = fout_cols.at(buid);

      grow[ocol] = -stage_conversion_rate;
      grow[gcol] = +1;
      grows[buid] = lp.add_row(std::move(grow));
    }

    const auto dcol = demand_cols.at(buid);
    {
      const auto icol = finp_cols.at(buid);
      auto drow =
          SparseRow {
              .class_name = Element::class_name.full_name(),
              .constraint_name = DemandName,
              .variable_uid = uid(),
              .context = block_context,
          }
              .equal(0);
      drow[icol] = -stage_conversion_rate;
      drow[dcol] = +1;
      drows[buid] = lp.add_row(std::move(drow));
    }

    // adding the capacity constraint
    if (capacity_col) {
      auto crow =
          SparseRow {
              .class_name = Element::class_name.full_name(),
              .constraint_name = CapacityName,
              .variable_uid = uid(),
              .context = block_context,
          }
              .greater_equal(0);

      crow[*capacity_col] = 1;
      crow[gcol] = -1;
      crow[dcol] = -1;

      crows[buid] = lp.add_row(std::move(crow));
    }
  }

  // Conditional commitment: when ``Converter.commitment`` is set,
  // gate the synthetic charge ``Demand`` and discharge ``Generator``
  // floors/ceilings with per-block binaries.  Mirrors UC.jl's
  // ``Minimum charge/discharge rate (MW)`` semantics (the floor only
  // fires when the battery is actively charging/discharging) and
  // PLEXOS ``Battery.Commitment Status``.
  //
  // Approach: read the column bounds that ``GeneratorLP`` /
  // ``DemandLP`` already stamped (block_pmax = uppb, block_pmin =
  // lowb on the generation/load columns), then RESET the static
  // lower bounds to 0 and emit C2-style rows
  //
  //     col − ub × u ≤ 0        (col_upper)
  //     col − lb × u ≥ 0        (col_lower, only when lb > 0)
  //
  // so the bounds only fire when the corresponding binary u is 1.
  // Default u ∈ [0, 1] continuous (LP relax — the cost-minimisation
  // objective still drives binaries to extreme points at the solver
  // because the inner Generator.gcost / Demand.fcost coefficients
  // make any fractional u suboptimal); ``integer_commitment`` flips
  // them to {0, 1} for a true MIP.
  if (converter().commitment.value_or(false)) {
    for (const auto& block : blocks) {
      const auto buid = block.uid();
      const auto block_ctx =
          make_block_context(scenario.uid(), stage.uid(), buid);
      const auto gcol = gen_cols.at(buid);
      const auto dcol = demand_cols.at(buid);
      const auto block_pmin = lp.get_col_lowb(gcol);
      const auto block_pmax = lp.get_col_uppb(gcol);
      const auto block_lmin = lp.get_col_lowb(dcol);
      const auto block_lmax = lp.get_col_uppb(dcol);

      // Discharge side: migrate ``Generator.pmin`` from a static col
      // floor to a u-gated row.  ``u_discharge`` exists only when the
      // discharge envelope is non-degenerate (pmax > 0).
      if (block_pmax > 0.0) {
        lp.col_at(gcol).lowb = 0.0;
        const auto u_disc = lp.add_col({
            .lowb = 0.0,
            .uppb = 1.0,
            .cost = 0.0,
            .is_integer = true,
            .class_name = Element::class_name.full_name(),
            .variable_name = UDischargeName,
            .variable_uid = uid(),
            .context = block_ctx,
        });
        {
          auto row =
              SparseRow {
                  .class_name = Element::class_name.full_name(),
                  .constraint_name = DischargeUpperName,
                  .variable_uid = uid(),
                  .context = block_ctx,
              }
                  .less_equal(0.0);
          row[gcol] = 1.0;
          row[u_disc] = -block_pmax;
          static_cast<void>(lp.add_row(std::move(row)));
        }
        if (block_pmin > 0.0) {
          auto row =
              SparseRow {
                  .class_name = Element::class_name.full_name(),
                  .constraint_name = DischargeLowerName,
                  .variable_uid = uid(),
                  .context = block_ctx,
              }
                  .greater_equal(0.0);
          row[gcol] = 1.0;
          row[u_disc] = -block_pmin;
          static_cast<void>(lp.add_row(std::move(row)));
        }
      }

      // Charge side: mirror the discharge gating onto the synthetic
      // ``Demand.lmin`` / ``Demand.lmax``.  Same skip condition
      // (lmax > 0); same migration of static lowb → u-gated row.
      if (block_lmax > 0.0) {
        lp.col_at(dcol).lowb = 0.0;
        const auto u_chg = lp.add_col({
            .lowb = 0.0,
            .uppb = 1.0,
            .cost = 0.0,
            .is_integer = true,
            .class_name = Element::class_name.full_name(),
            .variable_name = UChargeName,
            .variable_uid = uid(),
            .context = block_ctx,
        });
        {
          auto row =
              SparseRow {
                  .class_name = Element::class_name.full_name(),
                  .constraint_name = ChargeUpperName,
                  .variable_uid = uid(),
                  .context = block_ctx,
              }
                  .less_equal(0.0);
          row[dcol] = 1.0;
          row[u_chg] = -block_lmax;
          static_cast<void>(lp.add_row(std::move(row)));
        }
        if (block_lmin > 0.0) {
          auto row =
              SparseRow {
                  .class_name = Element::class_name.full_name(),
                  .constraint_name = ChargeLowerName,
                  .variable_uid = uid(),
                  .context = block_ctx,
              }
                  .greater_equal(0.0);
          row[dcol] = 1.0;
          row[u_chg] = -block_lmin;
          static_cast<void>(lp.add_row(std::move(row)));
        }
      }
    }
  }

  // storing the indices for this scenario and stage
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  capacity_rows[st_key] = std::move(crows);
  generation_rows[st_key] = std::move(grows);
  demand_rows[st_key] = std::move(drows);

  // Register converter attributes as aliases onto the associated
  // generator / demand per-block column maps (lifetime: generator &
  // demand outlive this call and the SimulationLP registry).
  //   discharge -> generator.generation
  //   charge    -> demand.load
  if (!gen_cols.empty()) {
    sc.add_ampl_variable(
        ampl_name, uid(), BatteryLP::DischargeName, scenario, stage, gen_cols);
  }
  if (!demand_cols.empty()) {
    sc.add_ampl_variable(
        ampl_name, uid(), BatteryLP::ChargeName, scenario, stage, demand_cols);
  }

  return true;
}

bool ConverterLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();

  out.add_row_dual(cname, GenerationName, id(), generation_rows);
  out.add_row_dual(cname, DemandName, id(), demand_rows);
  out.add_row_dual(cname, CapacityName, id(), capacity_rows);

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
