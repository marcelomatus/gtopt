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

  if (!CapacityBase::add_to_lp(sc, scenario, stage, lp, cname)) {
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

  // adding the minimum energy constraint
  const auto emin_row = [&](auto stage_emin,
                            auto stage_ecost) -> std::optional<Index>
  {
    if (!stage_emin) [[unlikely]] {
      return std::nullopt;
    }

    auto name = sc.st_label(scenario, stage, cname, "emin", uid());
    const auto emin_col = stage_ecost
        ? lp.add_col(  //
              {.name = name,
               .uppb = stage_emin.value(),
               .cost = -sc.stage_ecost(stage,
                                       stage_ecost.value() / stage.duration())})
        : lp.add_col(  //
              {.name = name,
               .lowb = stage_emin.value(),
               .uppb = stage_emin.value()});

    const GSTIndexHolder::key_type st_k {scenario.index(), stage.index()};
    emin_cols[st_k] = emin_col;

    SparseRow row {.name = std::move(name)};
    row[emin_col] = -1;

    return emin_rows[st_k] = lp.add_row(std::move(row.greater_equal(0)));
  }(emin.optval(stage.uid()), ecost.optval(stage.uid()));

  BIndexHolder lcols;
  lcols.reserve(blocks.size());
  BIndexHolder crows;
  crows.reserve(blocks.size());

  for (const auto& [block, balance_row] : std::views::zip(blocks, balance_rows))
  {
    const auto block_lmax = sc.block_max_at(stage, block, lmax, stage_capacity);

    const auto lcol = stage_fcost
        ? lp.add_col({.name = sc.stb_label(
                          scenario, stage, block, cname, "load", uid()),
                      .uppb = block_lmax,
                      .cost = -sc.block_ecost(
                          scenario, stage, block, stage_fcost.value())})
        : lp.add_col({.name = sc.stb_label(
                          scenario, stage, block, cname, "load", uid()),
                      .lowb = block_lmax,
                      .uppb = block_lmax});

    lcols.push_back(lcol);

    // adding load variable to the balance and load rows
    auto& brow = lp.row_at(balance_row);
    brow[lcol] = -(1 + stage_lossfactor);

    // adding the capacity constraint
    if (capacity_col) {
      SparseRow crow {
          .name = sc.stb_label(scenario, stage, block, cname, "cap", uid())};
      crow[capacity_col.value()] = 1;
      crow[lcol] = -1;

      crows.push_back(lp.add_row(std::move(crow.greater_equal(0))));
    }

    // adding the minimum energy constraint
    if (emin_row) {
      const auto dbloque = block.duration();
      lp.set_coeff(emin_row.value(), lcol, dbloque);
    }
  }

  auto [crow_it, crow_inserted] =
      emplace_bholder(scenario, stage, capacity_rows, std::move(crows));
  auto [lcol_it, lcol_inserted] =
      emplace_bholder(scenario, stage, load_cols, std::move(lcols));
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

  return CapacityBase::add_to_output(out, cname);
}

}  // namespace gtopt
