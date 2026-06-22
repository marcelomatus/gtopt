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
#include <gtopt/commitment_lp.hpp>
#include <gtopt/converter_lp.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>

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

  // Tolerant element lookups: for expansion batteries with pmax=0
  // (active only in future stages), the synthetic generator, demand,
  // or battery LP elements may be absent or have no columns for this
  // (scenario, stage).  Skip the converter entirely in that case —
  // there is nothing to couple.
  const GeneratorLP* gen_ptr = nullptr;
  const DemandLP* dem_ptr = nullptr;
  const BatteryLP* bat_ptr = nullptr;
  try {
    gen_ptr = &sc.element<GeneratorLP>(generator_sid());
    dem_ptr = &sc.element<DemandLP>(demand_sid());
    bat_ptr = &sc.element<BatteryLP>(battery_sid());
  } catch (const std::exception&) {
    return true;  // element not in LP — skip silently
  }

  auto&& gen_cols = gen_ptr->lookup_generation_cols(scenario, stage);
  auto&& demand_cols = dem_ptr->load_cols_at(scenario, stage);
  auto&& finp_cols = bat_ptr->finp_cols_at(scenario, stage);
  auto&& fout_cols = bat_ptr->fout_cols_at(scenario, stage);

  BIndexHolder<RowIndex> grows;
  BIndexHolder<RowIndex> drows;
  BIndexHolder<RowIndex> crows;
  map_reserve(grows, blocks.size());
  map_reserve(drows, blocks.size());
  map_reserve(crows, blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();

    // LP-size / robustness: when the synthetic discharge generator or
    // charge demand elided this block (gold-standard zero-envelope P1
    // skip), there is no column to couple — skip the conversion and
    // capacity rows entirely.  Write-out rule: an absent coupling row
    // has a 0 dual (it never bound), the natural zero.
    const auto gcol_it = gen_cols.find(buid);
    const auto dcol_it = demand_cols.find(buid);
    const auto fout_it = fout_cols.find(buid);
    const auto finp_it = finp_cols.find(buid);
    if (gcol_it == gen_cols.end() || dcol_it == demand_cols.end()
        || fout_it == fout_cols.end() || finp_it == finp_cols.end())
    {
      continue;
    }

    const auto block_context =
        make_block_context(scenario.uid(), stage.uid(), block.uid());
    const auto gcol = gcol_it->second;
    {
      auto grow =
          SparseRow {
              .class_name = Element::class_name.full_name(),
              .constraint_name = GenerationName,
              .variable_uid = uid(),
              .context = block_context,
          }
              .equal(0);
      const auto ocol = fout_it->second;

      grow[ocol] = -stage_conversion_rate;
      grow[gcol] = +1;
      grows[buid] = lp.add_row(std::move(grow));
    }

    const auto dcol = dcol_it->second;
    {
      const auto icol = finp_it->second;
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
  // bounds with per-block binaries.  Mirrors PLEXOS
  // ``Battery.Commitment Status``.
  //
  // "One true source for u_commit" — when the generator side has a
  // CommitmentLP (always true for batteries via
  // ``System::expand_batteries()`` synthesis), the discharge
  // gating ``gen ≤ pmax × u`` / ``gen ≥ Commitment.pmin × u`` is
  // already emitted by ``CommitmentLP::add_to_lp``.  ConverterLP
  // then reuses that same ``u_commit`` column to gate the
  // charge-side demand bounds — no duplicate ``u_discharge`` /
  // ``u_charge`` cols are introduced.
  //
  // Fallback (legacy, kept for non-synthesized cases): when no
  // CommitmentLP matches the converter's generator, fall back to
  // creating local ``u_charge`` / ``u_discharge`` binaries (integer
  // by convention — the original ``Converter.commitment`` API).
  if (converter().commitment.value_or(false)) {
    // Look up the CommitmentLP that points at this converter's
    // discharge generator.  The synthesized commitment (uid named
    // ``uc_<bat>_gen``) ``relax = true`` by default — fractional u
    // is fine for an LP-only run.
    const BIndexHolder<ColIndex>* shared_ucol = nullptr;
    for (const auto& cmt : sc.elements<CommitmentLP>()) {
      if (cmt.generator_sid() != generator_sid()) {
        continue;
      }
      shared_ucol = cmt.find_status_cols(scenario, stage);
      if (shared_ucol != nullptr) {
        break;
      }
    }

    for (const auto& block : blocks) {
      const auto buid = block.uid();
      const auto block_ctx =
          make_block_context(scenario.uid(), stage.uid(), buid);
      const auto gcol_it = gen_cols.find(buid);
      const auto dcol_it = demand_cols.find(buid);
      if (gcol_it == gen_cols.end() || dcol_it == demand_cols.end()) {
        continue;
      }
      const auto gcol = gcol_it->second;
      const auto dcol = dcol_it->second;
      const auto block_pmin = lp.get_col_lowb(gcol);
      const auto block_pmax = lp.get_col_uppb(gcol);
      const auto block_lmin = lp.get_col_lowb(dcol);
      const auto block_lmax = lp.get_col_uppb(dcol);

      // ── Shared-u_commit path ────────────────────────────────────
      if (shared_ucol != nullptr) {
        const auto uc_it = shared_ucol->find(buid);
        if (uc_it == shared_ucol->end()) {
          continue;
        }
        const auto ucol = uc_it->second;

        // Discharge side is already gated by CommitmentLP via
        // ``gen_upper`` / ``gen_lower`` rows; nothing to add here.
        // Charge side: mirror the discharge gating onto the
        // synthetic demand col using the same shared u_commit.
        if (block_lmax > 0.0) {
          lp.col_at(dcol).lowb = 0.0;
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
            row[ucol] = -block_lmax;
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
            row[ucol] = -block_lmin;
            static_cast<void>(lp.add_row(std::move(row)));
          }
        }
        continue;
      }

      // ── Legacy fallback (no CommitmentLP) ──────────────────────
      // Discharge side: migrate ``Generator.pmin`` from a static col
      // floor to a u-gated row.  ``u_discharge`` exists only when the
      // discharge envelope is non-degenerate (pmax > 0).
      if (block_pmax > 0.0) {
        lp.col_at(gcol).lowb = 0.0;
        const std::array<BlockUid, 1> u_disc_blocks {buid};
        const auto u_disc = sc.add_integer_col(
            lp,
            IntegerVariable::key(scenario,
                                 stage,
                                 Element::class_name,
                                 uid(),
                                 UDischargeName,
                                 IntegerScope::Block,
                                 buid),
            SparseCol {
                .lowb = 0.0,
                .uppb = 1.0,
                .cost = 0.0,
                .class_name = Element::class_name.full_name(),
                .variable_name = UDischargeName,
                .variable_uid = uid(),
                .context = block_ctx,
            },
            IntegerDomain::Binary,
            IntegerScope::Block,
            buid,
            std::span<const BlockUid> {u_disc_blocks});
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

      // Charge side fallback: mirror the discharge gating onto the
      // synthetic ``Demand.lmin`` / ``Demand.lmax``.
      if (block_lmax > 0.0) {
        lp.col_at(dcol).lowb = 0.0;
        const std::array<BlockUid, 1> u_chg_blocks {buid};
        const auto u_chg = sc.add_integer_col(
            lp,
            IntegerVariable::key(scenario,
                                 stage,
                                 Element::class_name,
                                 uid(),
                                 UChargeName,
                                 IntegerScope::Block,
                                 buid),
            SparseCol {
                .lowb = 0.0,
                .uppb = 1.0,
                .cost = 0.0,
                .class_name = Element::class_name.full_name(),
                .variable_name = UChargeName,
                .variable_uid = uid(),
                .context = block_ctx,
            },
            IntegerDomain::Binary,
            IntegerScope::Block,
            buid,
            std::span<const BlockUid> {u_chg_blocks});
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
