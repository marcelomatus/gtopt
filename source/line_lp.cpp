#include <gtopt/line_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <range/v3/all.hpp>

#include "gtopt/block.hpp"

namespace gtopt
{

LineLP::LineLP(const InputContext& ic, Line pline)
    : CapacityBase(ic, ClassName, std::move(pline))
    , tmin(ic, ClassName, id(), std::move(line().tmin))
    , tmax(ic, ClassName, id(), std::move(line().tmax))
    , tcost(ic, ClassName, id(), std::move(line().tcost))
    , lossfactor(ic, ClassName, id(), std::move(line().lossfactor))
    , reactance(ic, ClassName, id(), std::move(line().reactance))
    , voltage(ic, ClassName, id(), std::move(line().voltage))
{
}

bool LineLP::add_to_lp(SystemContext& sc,
                       const ScenarioLP& scenario,
                       const StageLP& stage,
                       LinearProblem& lp)
{
  constexpr std::string_view cname = "line";

  if (!CapacityBase::add_to_lp(sc, scenario, stage, lp, cname)) {
    return false;
  }

  if (bus_a() == bus_b()) {
    return true;
  }

  const auto stage_index = stage.index();
  const auto scenario_index = scenario.index();

  const auto& bus_a_lp = sc.element<BusLP>(bus_a());
  const auto& bus_b_lp = sc.element<BusLP>(bus_b());
  if (!bus_a_lp.is_active(stage_index) || !bus_b_lp.is_active(stage_index)) {
    return true;
  }

  const auto& balance_rows_a =
      bus_a_lp.balance_rows_at(scenario_index, stage_index);
  const auto& balance_rows_b =
      bus_b_lp.balance_rows_at(scenario_index, stage_index);

  const auto& blocks = stage.blocks();

  const auto [stage_capacity, capacity_col] = capacity_and_col(stage_index, lp);
  const auto stage_tcost = tcost.at(stage_index).value_or(0.0);
  const auto stage_lossfactor = sc.stage_lossfactor(stage_index, lossfactor);
  const auto has_loss = stage_lossfactor > 0.0;

  BIndexHolder fpcols;
  fpcols.reserve(blocks.size());
  BIndexHolder cprows;
  cprows.reserve(blocks.size());
  BIndexHolder fncols;
  fncols.reserve(blocks.size());
  BIndexHolder cnrows;
  cnrows.reserve(blocks.size());

  for (const auto& [block_index, block, balance_row_a, balance_row_b] :
       enumerate<BlockIndex>(blocks, balance_rows_a, balance_rows_b))
  {
    const auto [block_tmax, block_tmin] = sc.block_maxmin_at(
        stage_index, block_index, tmax, tmin, stage_capacity, -stage_capacity);

    const auto block_tcost =
        sc.block_ecost(scenario_index, stage_index, block, stage_tcost);

    auto& brow_a = lp.row_at(balance_row_a);
    auto& brow_b = lp.row_at(balance_row_b);

    if (!has_loss || block_tmax > 0.0) {
      //  adding flowp variable

      const auto fpc = lp.add_col(
          {// flow variable
           .name = sc.stb_label(
               scenario_index, stage_index, block, cname, "fp", uid()),
           .lowb = has_loss ? 0 : block_tmin,
           .uppb = block_tmax,
           .cost = block_tcost});
      fpcols.push_back(fpc);

      // adding flowp to the bus balances
      brow_a[fpc] = -(1 + stage_lossfactor);
      brow_b[fpc] = +1;

      // adding the capacity constraint
      if (capacity_col) {
        SparseRow cprow {
            .name = sc.stb_label(
                scenario_index, stage_index, block, cname, "capp", uid())};
        cprow[capacity_col.value()] = 1;
        cprow[fpc] = -1;

        cprows.push_back(lp.add_row(std::move(cprow.greater_equal(0))));
      }
    }

    if (has_loss && block_tmin < 0.0) {
      //  adding flown variable

      const auto fnc = lp.add_col(
          {// flow variable
           .name = sc.stb_label(
               scenario_index, stage_index, block, cname, "fn", uid()),
           .lowb = 0,
           .uppb = -block_tmin,
           .cost = block_tcost});

      fncols.push_back(fnc);

      // adding flown to the bus balances
      brow_b[fnc] = -(1 + stage_lossfactor);
      brow_a[fnc] = +1;

      // adding the capacity constraint
      if (capacity_col) {
        SparseRow cnrow {
            .name = sc.stb_label(
                scenario_index, stage_index, block, cname, "capn", uid())};
        cnrow[capacity_col.value()] = 1;
        cnrow[fnc] = -1;

        cnrows.push_back(lp.add_row(std::move(cnrow.greater_equal(0))));
      }
    }
  }

  // adding the Kirchhoff relations if there is a line reactance
  if (const auto& stage_reactance = sc.stage_reactance(stage_index, reactance))
  {
    const auto& theta_a_cols =
        bus_a_lp.theta_cols_at(sc, scenario, stage, lp, blocks);
    const auto& theta_b_cols =
        bus_b_lp.theta_cols_at(sc, scenario, stage, lp, blocks);

    if (!theta_a_cols.empty() && !theta_b_cols.empty()) {
      const double scale_theta = sc.options().scale_theta();
      const double X = stage_reactance.value();
      const double V = voltage.at(stage_index).value_or(1);
      const double x = scale_theta * (X / (V * V));

      BIndexHolder trows;
      trows.reserve(blocks.size());

      for (auto&& [block_index, block] : enumerate<BlockIndex>(blocks)) {
        SparseRow trow(
            {.name = sc.stb_label(
                 scenario_index, stage_index, block, cname, "theta", uid())});
        trow.reserve(4);

        trow[theta_a_cols.at(block_index)] = 1;
        trow[theta_b_cols.at(block_index)] = -1;
        if (!fpcols.empty()) {
          trow[fpcols.at(block_index)] = x;
        }
        if (!fncols.empty()) {
          trow[fncols.at(block_index)] = -x;
        }

        trows.push_back(lp.add_row(std::move(trow.equal(0))));
      }

      if (!emplace_bholder(
               scenario_index, stage_index, theta_rows, std::move(trows))
               .second)
      {
        return false;
      }
    }
  }

  // save all rows and cols
  return emplace_bholder(
             scenario_index, stage_index, capacityp_rows, std::move(cprows))
             .second
      && emplace_bholder(
             scenario_index, stage_index, capacityn_rows, std::move(cnrows))
             .second
      && emplace_bholder(
             scenario_index, stage_index, flowp_cols, std::move(fpcols))
             .second
      && emplace_bholder(
             scenario_index, stage_index, flown_cols, std::move(fncols))
             .second;
}

bool LineLP::add_to_output(OutputContext& out) const
{
  constexpr std::string_view cname = ClassName;

  const auto pid = id();

  out.add_col_sol(cname, "flowp", pid, flowp_cols);
  out.add_col_cost(cname, "flowp", pid, flowp_cols);

  out.add_col_sol(cname, "flown", pid, flown_cols);
  out.add_col_cost(cname, "flown", pid, flown_cols);

  out.add_row_dual(cname, "capacityp", pid, capacityp_rows);
  out.add_row_dual(cname, "capacityn", pid, capacityn_rows);

  out.add_row_dual(cname, "theta", pid, theta_rows);

  return CapacityBase::add_to_output(out, cname);
}

}  // namespace gtopt
