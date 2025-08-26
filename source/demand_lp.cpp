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

DemandLP::DemandLP(Demand pdemand, const InputContext& ic)
    : CapacityBase(std::move(pdemand), ic, ClassName)
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
  static constexpr std::string_view cname = ClassName.short_name();

  if (!CapacityBase::add_to_lp(sc, scenario, stage, lp)) {
    return false;
  }

  if (!is_active(stage)) [[unlikely]] {
    return true;
  }

  const auto& bus_lp = sc.element<BusLP>(bus_sid());
  if (!bus_lp.is_active(stage)) [[unlikely]] {
    return true;
  }

  const auto [stage_capacity, capacity_col] = capacity_and_col(stage, lp);
  const auto stage_fcost = sc.demand_fail_cost(stage, fcost);
  const auto stage_lossfactor = lossfactor.optval(stage.uid()).value_or(0.0);

  const auto& bus_balance_rows = bus_lp.balance_rows_at(scenario, stage);
  const auto& blocks = stage.blocks();

  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  // adding the minimum energy constraint
  const auto stage_emin = emin.optval(stage.uid());
  auto stage_ecost = ecost.optval(stage.uid());
  if (!stage_ecost && stage_fcost) {
    stage_ecost = stage_fcost;
  }

  auto emin_row = [&](const auto& stage_emin,
                      const auto& stage_ecost) -> std::optional<SparseRow>
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

    emin_cols[st_key] = emin_col;

    auto row = SparseRow {.name = std::move(name)}.equal(0);
    row[emin_col] = -1.0;

    return row;
  }(stage_emin, stage_ecost);

  BIndexHolder<ColIndex> lcols;
  BIndexHolder<ColIndex> fcols;
  BIndexHolder<ColIndex> mcols;
  BIndexHolder<RowIndex> crows;
  BIndexHolder<RowIndex> brows;
  lcols.reserve(blocks.size());
  mcols.reserve(blocks.size());
  crows.reserve(blocks.size());
  fcols.reserve(blocks.size());
  brows.reserve(blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto bus_balance_row = bus_balance_rows.at(buid);
    const auto block_lmax = sc.block_max_at(stage, block, lmax, stage_capacity);

    const auto load_lowb = !stage_fcost ? block_lmax : 0;
    const auto load_uppb = !stage_fcost ? block_lmax : COIN_DBL_MAX;
    const auto lcol = lp.add_col(
        {.name = sc.lp_label(scenario, stage, block, cname, "load", uid()),
         .lowb = load_lowb,
         .uppb = load_uppb});

    if (stage_fcost) {
      auto name = sc.lp_label(scenario, stage, block, cname, "fail", uid());
      const auto fcol = lp.add_col(
          {.name = name,
           .cost = sc.block_ecost(scenario, stage, block, *stage_fcost)});
      fcols[buid] = fcol;

      auto frow = SparseRow {.name = std::move(name)}.equal(block_lmax);

      frow[lcol] = 1;
      frow[fcol] = 1;
      brows[buid] = lp.add_row(std::move(frow));
    }

    lcols[buid] = lcol;

    // adding load variable to the balance and load rows
    auto& bus_brow = lp.row_at(bus_balance_row);
    bus_brow[lcol] = -(1.0 + stage_lossfactor);

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
    if (stage_emin && emin_row) {
      const auto bdur = block.duration();

      const auto mcol = lp.add_col(
          {.name = sc.lp_label(scenario, stage, block, cname, "lman", uid()),
           .uppb = *stage_emin / bdur});

      mcols[buid] = mcol;
      (*emin_row)[mcol] = bdur;

      bus_brow[mcol] = -(1.0 + stage_lossfactor);
    }
  }

  if (!mcols.empty()) {
    emin_rows[st_key] = lp.add_row(std::move(*emin_row));
    lman_cols[st_key] = std::move(mcols);
  }

  if (!lcols.empty()) {
    load_cols[st_key] = std::move(lcols);
  }
  if (!crows.empty()) {
    capacity_rows[st_key] = std::move(crows);
  }

  if (!fcols.empty()) {
    fail_cols[st_key] = std::move(fcols);
    balance_rows[st_key] = std::move(brows);
  }

  return true;
}

bool DemandLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();
  const auto pid = id();

  out.add_col_sol(cname, "load", pid, load_cols);
  out.add_col_cost(cname, "load", pid, load_cols);
  out.add_row_dual(cname, "capacity", pid, capacity_rows);

  out.add_col_sol(cname, "emin", pid, emin_cols);
  out.add_col_cost(cname, "emin", pid, emin_cols);
  out.add_row_dual(cname, "emin", pid, emin_rows);

  out.add_col_sol(cname, "lman", pid, lman_cols);
  out.add_col_cost(cname, "lman", pid, lman_cols);

  out.add_col_sol(cname, "fail", pid, fail_cols);
  out.add_col_cost(cname, "fail", pid, fail_cols);
  out.add_row_dual(cname, "balance", pid, balance_rows);

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
