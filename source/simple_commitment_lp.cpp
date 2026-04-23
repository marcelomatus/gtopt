/**
 * @file      simple_commitment_lp.cpp
 * @brief     LP formulation for simplified commitment
 * @date      2026-04-13
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements SimpleCommitmentLP: creates a binary status variable u per
 * block and adds two constraints per block:
 * - p - Pmax*u <= 0  (upper generation limit)
 * - p - dispatch_pmin*u >= 0  (minimum output when dispatched)
 */

#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/simple_commitment_lp.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

SimpleCommitmentLP::SimpleCommitmentLP(const SimpleCommitment& sc,
                                       const InputContext& ic)
    : Base(sc, ic, ClassName)
    , generator_index_(ic.element_index(generator_sid()))
    , dispatch_pmin_(ic, ClassName, id(), std::move(object().dispatch_pmin))
{
}

bool SimpleCommitmentLP::add_to_lp(SystemContext& sc,
                                   const ScenarioLP& scenario,
                                   const StageLP& stage,
                                   LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  auto&& generator_lp = sc.element(generator_index_);
  if (!generator_lp.is_active(stage)) {
    return true;
  }

  const auto& generation_cols =
      generator_lp.generation_cols_at(scenario, stage);
  const auto& blocks = stage.blocks();

  if (blocks.empty()) {
    return true;
  }

  static constexpr std::string_view cname = ClassName.full_name();
  static constexpr auto ampl_name = ClassName.snake_case();
  const auto cuid = uid();

  const auto is_relax = simple_commitment().relax.value_or(false)
      || sc.simulation().phases()[stage.phase_index()].is_continuous();
  const auto is_must_run = simple_commitment().must_run.value_or(false);

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};

  BIndexHolder<ColIndex> ucols;
  BIndexHolder<RowIndex> gurows;
  BIndexHolder<RowIndex> glrows;
  map_reserve(ucols, blocks.size());
  map_reserve(gurows, blocks.size());
  map_reserve(glrows, blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto ctx = make_block_context(scenario.uid(), stage.uid(), buid);

    const auto gcol_it = generation_cols.find(buid);
    if (gcol_it == generation_cols.end()) {
      continue;
    }
    const auto gcol = gcol_it->second;

    const auto gen_pmax = lp.get_col_uppb(gcol);
    const auto gen_pmin = lp.get_col_lowb(gcol);

    // Resolve dispatch_pmin: use schedule value, fall back to generator's pmin
    const auto dpmin = dispatch_pmin_.at(stage.uid(), buid).value_or(gen_pmin);

    // Create binary status variable u (cost = 0, no noload cost)
    auto ucol = lp.add_col({
        .lowb = is_must_run ? 1.0 : 0.0,
        .uppb = 1.0,
        .cost = 0.0,
        .is_integer = !is_relax,
        .class_name = cname,
        .variable_name = StatusName,
        .variable_uid = cuid,
        .context = ctx,
    });
    ucols[buid] = ucol;

    // Set generation column lower bound to 0 (pmin enforcement via constraint)
    auto& gcol_ref = lp.col_at(gcol);
    gcol_ref.lowb = 0.0;

    // Upper generation limit: p - Pmax*u <= 0
    {
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = GenUpperName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .less_equal(0.0);
      row[gcol] = 1.0;
      row[ucol] = -gen_pmax;
      gurows[buid] = lp.add_row(std::move(row));
    }

    // Lower generation limit: p - dispatch_pmin*u >= 0
    {
      auto row =
          SparseRow {
              .class_name = cname,
              .constraint_name = GenLowerName,
              .variable_uid = cuid,
              .context = ctx,
          }
              .greater_equal(0.0);
      row[gcol] = 1.0;
      row[ucol] = -dpmin;
      glrows[buid] = lp.add_row(std::move(row));
    }
  }

  // Store index holders
  if (!ucols.empty()) {
    status_cols_[st_key] = std::move(ucols);
  }
  if (!gurows.empty()) {
    gen_upper_rows_[st_key] = std::move(gurows);
  }
  if (!glrows.empty()) {
    gen_lower_rows_[st_key] = std::move(glrows);
  }

  // Register PAMPL-visible status columns
  if (const auto it = status_cols_.find(st_key);
      it != status_cols_.end() && !it->second.empty())
  {
    sc.add_ampl_variable(
        ampl_name, uid(), StatusName, scenario, stage, it->second);
  }

  return true;
}

bool SimpleCommitmentLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();
  const auto pid = id();

  out.add_col_sol(cname, StatusName, pid, status_cols_);
  out.add_col_cost(cname, StatusName, pid, status_cols_);

  out.add_row_dual(cname, GenUpperName, pid, gen_upper_rows_);
  out.add_row_dual(cname, GenLowerName, pid, gen_lower_rows_);

  return true;
}

}  // namespace gtopt
