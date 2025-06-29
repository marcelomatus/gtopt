/**
 * @file      demand_lp.cpp
 * @brief     Header of
 * @date      Thu Mar 27 22:31:31 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */
#include <gtopt/demand_lp.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_lp.hpp>
#include <range/v3/all.hpp>

namespace gtopt
{

DemandLP::DemandLP(const InputContext& ic, Demand pdemand)
    : CapacityBase(ic, ClassName, std::move(pdemand))
    , lmax(ic, ClassName, id(), std::move(object().lmax))
    , lossfactor(ic, ClassName, id(), std::move(object().lossfactor))
    , fcost(ic, ClassName, id(), std::move(object().fcost))
    , emin(ic, ClassName, id(), std::move(object().emin))
    , ecost(ic, ClassName, id(), std::move(object().ecost))
{
}

bool DemandLP::add_to_lp(SystemContext& sc,
                         const ScenarioLP& scenario,
                         const StageLP& stage,
                         LinearProblem& lp)
{
  constexpr std::string_view cname = "demand";

  if (!CapacityBase::add_to_lp(sc, scenario, stage, lp)) {
    return false;
  }

  if (!is_active(stage)) [[unlikely]] {
    return true;
  }

  const auto& bus_lp = sc.element<BusLP>(bus());
  if (!bus_lp.is_active(stage)) [[unlikely]] {
    return true;
  }

  const auto [stage_capacity, capacity_col] = capacity_and_col(stage, lp);
  const auto stage_fcost = sc.demand_fail_cost(stage, fcost);
  const auto stage_lossfactor = lossfactor.optval(stage.uid()).value_or(0.0);

  const auto& balance_rows = bus_lp.balance_rows_at(scenario, stage);
  const auto& blocks = stage.blocks();

  const auto st_k = std::pair {scenario.uid(), stage.uid()};

  // adding the minimum energy constraint
  auto emin_row = [&](auto stage_emin,
                      auto stage_ecost) -> std::optional<SparseRow>
  {
    if (!stage_emin) [[unlikely]] {
      return std::nullopt;
    }

    auto name = lp_label(sc, scenario, stage, "emin");

    const auto emin_col = stage_ecost
        ? lp.add_col(
              {.name = name,
               .uppb = *stage_emin,
               .cost = -sc.stage_ecost(stage, *stage_ecost / stage.duration())})
        : lp.add_col({.name = name, .lowb = *stage_emin, .uppb = *stage_emin});

    emin_cols[st_k] = emin_col;

    auto row = SparseRow {.name = std::move(name)}.greater_equal(0);
    row[emin_col] = -1.0;

    return {row};
  }(emin.optval(stage.uid()), ecost.optval(stage.uid()));

  BIndexHolder<ColIndex> lcols;
  lcols.reserve(blocks.size());
  BIndexHolder<RowIndex> crows;
  crows.reserve(blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto balance_row = balance_rows.at(buid);
    const auto block_lmax = sc.block_max_at(stage, block, lmax, stage_capacity);

    const auto lcol = stage_fcost
        ? lp.add_col(
              {.name =
                   sc.lp_label(scenario, stage, block, cname, "load", uid()),
               .uppb = block_lmax,
               .cost = -sc.block_ecost(scenario, stage, block, *stage_fcost)})
        : lp.add_col({.name = sc.lp_label(
                          scenario, stage, block, cname, "load", uid()),
                      .lowb = block_lmax,
                      .uppb = block_lmax});

    lcols[buid] = lcol;

    // adding load variable to the balance and load rows
    auto& brow = lp.row_at(balance_row);
    brow[lcol] = -(1.0 + stage_lossfactor);

    // adding the capacity constraint
    if (capacity_col) {
      auto crow = SparseRow {.name = sc.lp_label(
                                 scenario, stage, block, cname, "cap", uid())}
                      .greater_equal(0.0);

      crow[*capacity_col] = 1.0;
      crow[lcol] = -1.0;

      crows[buid] = lp.add_row(std::move(crow));
    }

    // adding the minimum energy constraint
    if (emin_row) {
      (*emin_row)[lcol] = block.duration();
    }
  }

  if (emin_row && emin_row->size() > 1U) {
    emin_rows[st_k] = lp.add_row(std::move(*emin_row));
  }

  const auto crow_inserted =
      emplace_bholder(scenario, stage, capacity_rows, std::move(crows)).second;
  const auto lcol_inserted =
      emplace_bholder(scenario, stage, load_cols, std::move(lcols)).second;

  return crow_inserted && lcol_inserted;
}

bool DemandLP::add_to_output(OutputContext& out) const
{
  constexpr std::string_view cname = ClassName;

  const auto pid = id();
  out.add_col_sol(cname, "load", pid, load_cols);
  out.add_col_cost(cname, "load", pid, load_cols);
  out.add_row_dual(cname, "capacity", pid, capacity_rows);

  out.add_col_sol(cname, "emin", pid, emin_cols);
  out.add_col_cost(cname, "emin", pid, emin_cols);
  out.add_row_dual(cname, "emin", pid, emin_rows);

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
