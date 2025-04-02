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
#include <gtopt/system_lp.hpp>
#include <range/v3/view/zip.hpp>

namespace gtopt
{

DemandLP::DemandLP(const InputContext& ic, Demand&& pdemand)
    : CapacityBase(ic, ClassName, std::move(pdemand))
    , lmax(ic, ClassName, id(), std::move(demand().lmax))
    , lossfactor(ic, ClassName, id(), std::move(demand().lossfactor))
    , fcost(ic, ClassName, id(), std::move(demand().fcost))
    , emin(ic, ClassName, id(), std::move(demand().emin))
    , ecost(ic, ClassName, id(), std::move(demand().ecost))
    , bus_index(ic.element_index(bus()))
{
}

bool DemandLP::add_to_lp(const SystemContext& sc, LinearProblem& lp)
{
  constexpr std::string_view cname = "dem";
  if (!CapacityBase::add_to_lp(sc, lp, cname)) {
    return false;
  }

  const auto stage_index = sc.stage_index();
  if (!is_active(stage_index)) [[unlikely]] {
    return true;
  }
  const auto& bus = sc.element(bus_index);
  if (!bus.is_active(stage_index)) [[unlikely]] {
    return true;
  }
  const auto scenery_index = sc.scenery_index();

  const auto [stage_capacity, capacity_col] = capacity_and_col(sc, lp);
  const auto stage_fcost = sc.demand_fail_cost(fcost);
  const auto stage_lossfactor = lossfactor.optval(stage_index).value_or(0.0);

  const auto& balance_rows = bus.balance_rows_at(scenery_index, stage_index);
  const auto& [blocks, block_indexes] = sc.stage_blocks_and_indexes();

  // adding the minimum energy constraint
  const auto emin_row = [this, &sc, &cname, &lp](
                            auto stage_emin,
                            auto stage_ecost) -> std::optional<size_t>
  {
    if (!stage_emin) [[unlikely]] {
      return std::nullopt;
    }

    auto name = sc.st_label(cname, "emin", uid());
    const auto emin_col = stage_ecost
        ? lp.add_col(  //
              {.name = name,
               .uppb = stage_emin.value(),
               .cost =
                   -sc.stage_cost(stage_ecost.value() / sc.stage_duration())})
        : lp.add_col(  //
              {.name = name,
               .lowb = stage_emin.value(),
               .uppb = stage_emin.value()});

    const GSTIndexHolder::key_type st_k {sc.scenery_index(), sc.stage_index()};
    emin_cols[st_k] = emin_col;

    SparseRow row {.name = std::move(name)};
    row[emin_col] = -1;

    return emin_rows[st_k] = lp.add_row(std::move(row.greater_equal(0)));
  }(emin.optval(stage_index), ecost.optval(stage_index));

  BIndexHolder lcols;
  lcols.reserve(blocks.size());
  BIndexHolder crows;
  crows.reserve(blocks.size());

  for (const auto [block_index, balance_row] :
       ranges::views::zip(block_indexes, balance_rows))
  {
    const auto& block = blocks[block_index];
    const auto block_lmax = sc.block_max_at(block_index, lmax, stage_capacity);

    const auto lcol = stage_fcost
        ? lp.add_col({.name = sc.stb_label(block, cname, "load", uid()),
                      .uppb = block_lmax,
                      .cost = -sc.block_cost(block, stage_fcost.value())})
        : lp.add_col({.name = sc.stb_label(block, cname, "load", uid()),
                      .lowb = block_lmax,
                      .uppb = block_lmax});

    lcols.push_back(lcol);

    // adding load variable to the balance and load rows
    auto& brow = lp.row_at(balance_row);
    brow[lcol] = -(1 + stage_lossfactor);

    // adding the capacity constraint
    if (capacity_col) {
      SparseRow crow {.name = sc.stb_label(block, cname, "cap", uid())};
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

  return sc.emplace_bholder(capacity_rows, std::move(crows)).second
      && sc.emplace_bholder(load_cols, std::move(lcols)).second;
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
