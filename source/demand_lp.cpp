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
    , lmax(ic, ClassName, id(), std::move(demand().lmax))
    , lossfactor(ic, ClassName, id(), std::move(demand().lossfactor))
    , fcost(ic, ClassName, id(), std::move(demand().fcost))
    , emin(ic, ClassName, id(), std::move(demand().emin))
    , ecost(ic, ClassName, id(), std::move(demand().ecost))
{
}

bool DemandLP::add_to_lp(const SystemContext& sc,
                         const ScenarioIndex& scenario_index,
                         const StageIndex& stage_index,
                         LinearProblem& lp)
{
  constexpr std::string_view cname = "dem";
  if (!CapacityBase::add_to_lp(sc, scenario_index, stage_index, lp, cname)) {
    return false;
  }

  if (!is_active(stage_index)) [[unlikely]] {
    return true;
  }
  const auto& bus_lp = sc.element<BusLP>(bus());
  if (!bus_lp.is_active(stage_index)) [[unlikely]] {
    return true;
  }

  const auto [stage_capacity, capacity_col] = capacity_and_col(stage_index, lp);
  const auto& fcost_val = fcost.optval(stage_index);
  const auto& lossfactor_val = lossfactor.optval(stage_index);
  const auto stage_fcost = sc.demand_fail_cost(stage_index, fcost_val);
  const auto stage_lossfactor = lossfactor_val.value_or(0.0);

  const auto& balance_rows =
      bus_lp.balance_rows_at(scenario_index, stage_index);
  const auto& blocks = sc.stage_blocks(stage_index);

  // adding the minimum energy constraint
  const auto emin_row = [&](auto stage_emin,
                            auto stage_ecost) -> std::optional<Index>
  {
    if (!stage_emin) [[unlikely]] {
      return std::nullopt;
    }

    auto name = sc.st_label(scenario_index, stage_index, cname, "emin", uid());
    const auto emin_col = stage_ecost
        ? lp.add_col(  //
              {.name = name,
               .uppb = stage_emin.value(),
               .cost = -sc.stage_cost(
                   stage_index,
                   stage_ecost.value() / sc.stage_duration(stage_index))})
        : lp.add_col(  //
              {.name = name,
               .lowb = stage_emin.value(),
               .uppb = stage_emin.value()});

    const GSTIndexHolder::key_type st_k {scenario_index, stage_index};
    emin_cols[st_k] = emin_col;

    SparseRow row {.name = std::move(name)};
    row[emin_col] = -1;

    return emin_rows[st_k] = lp.add_row(std::move(row.greater_equal(0)));
  }(emin.optval(stage_index), ecost.optval(stage_index));

  BIndexHolder lcols;
  lcols.reserve(blocks.size());
  BIndexHolder crows;
  crows.reserve(blocks.size());

  for (const auto& [block_index, block, balance_row] :
       enumerate<BlockIndex>(blocks, balance_rows))
  {
    const auto block_lmax =
        sc.block_max_at(stage_index, block_index, lmax, stage_capacity);

    const auto lcol = stage_fcost
        ? lp.add_col(
              {.name = sc.stb_label(
                   scenario_index, stage_index, block, cname, "load", uid()),
               .uppb = block_lmax,
               .cost = -sc.block_cost(
                   scenario_index, stage_index, block, stage_fcost.value())})
        : lp.add_col(
              {.name = sc.stb_label(
                   scenario_index, stage_index, block, cname, "load", uid()),
               .lowb = block_lmax,
               .uppb = block_lmax});

    lcols.push_back(lcol);

    // adding load variable to the balance and load rows
    auto& brow = lp.row_at(balance_row);
    brow[lcol] = -(1 + stage_lossfactor);

    // adding the capacity constraint
    if (capacity_col) {
      SparseRow crow {
          .name = sc.stb_label(
              scenario_index, stage_index, block, cname, "cap", uid())};
      crow[capacity_col.value()] = 1;
      crow[lcol] = -1;

      crows.push_back(lp.add_row(std::move(crow.greater_equal(0))));
    }

    // adding the minimum energy constraint
    if (emin_row) {
      const auto dbloque = blocks[block_index].duration();
      lp.set_coeff(emin_row.value(), lcol, dbloque);
    }
  }

  auto [crow_it, crow_inserted] = emplace_bholder(
      scenario_index, stage_index, capacity_rows, std::move(crows));
  auto [lcol_it, lcol_inserted] = emplace_bholder(
      scenario_index, stage_index, load_cols, std::move(lcols));
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
