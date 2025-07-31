#include <gtopt/line_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>

namespace gtopt
{

LineLP::LineLP(Line pline, const InputContext& ic)
    : CapacityBase(std::move(pline), ic, ClassName)
    , tmax_ba(ic, ClassName, id(), std::move(line().tmax_ba))
    , tmax_ab(ic, ClassName, id(), std::move(line().tmax_ab))
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
  constexpr std::string_view cname = ClassName;

  if (!CapacityBase::add_to_lp(sc, scenario, stage, lp)) {
    return false;
  }

  if (bus_a() == bus_b()) {
    return true;
  }

  const auto& bus_a_lp = sc.element<BusLP>(bus_a());
  const auto& bus_b_lp = sc.element<BusLP>(bus_b());
  if (!bus_a_lp.is_active(stage) || !bus_b_lp.is_active(stage)) {
    return true;
  }

  const auto& balance_rows_a = bus_a_lp.balance_rows_at(scenario, stage);
  const auto& balance_rows_b = bus_b_lp.balance_rows_at(scenario, stage);

  const auto& blocks = stage.blocks();

  const auto [stage_capacity, capacity_col] = capacity_and_col(stage, lp);
  const auto stage_tcost = tcost.at(stage.uid()).value_or(0.0);
  const auto stage_lossfactor = sc.stage_lossfactor(stage, lossfactor);
  const auto has_loss = stage_lossfactor > 0.0;

  BIndexHolder<ColIndex> fpcols;
  fpcols.reserve(blocks.size());
  BIndexHolder<RowIndex> cprows;
  cprows.reserve(blocks.size());
  BIndexHolder<ColIndex> fncols;
  fncols.reserve(blocks.size());
  BIndexHolder<RowIndex> cnrows;
  cnrows.reserve(blocks.size());

  for (const auto& block : blocks) {
    const auto buid = block.uid();
    const auto balance_row_a = balance_rows_a.at(buid);
    const auto balance_row_b = balance_rows_b.at(buid);

    const auto [block_tmax_ab, block_tmax_ba] = sc.block_maxmin_at(
        stage, block, tmax_ab, tmax_ba, stage_capacity, -stage_capacity);

    const auto block_tcost =
        sc.block_ecost(scenario, stage, block, stage_tcost);

    auto& brow_a = lp.row_at(balance_row_a);
    auto& brow_b = lp.row_at(balance_row_b);

    if (!has_loss || block_tmax_ab > 0.0) {
      //  adding flowp variable

      const auto fpc = lp.add_col(
          {// flow variable
           .name = sc.lp_label(scenario, stage, block, cname, "fp", uid()),
           .lowb = has_loss ? 0 : -block_tmax_ba,
           .uppb = block_tmax_ab,
           .cost = block_tcost});
      fpcols[buid] = fpc;

      // adding flowp to the bus balances
      brow_a[fpc] = -(1 + stage_lossfactor);
      brow_b[fpc] = +1;

      // adding the capacity constraint
      if (capacity_col) {
        auto cprow =
            SparseRow {.name = sc.lp_label(
                           scenario, stage, block, cname, "capp", uid())}
                .greater_equal(0);
        cprow[*capacity_col] = 1;
        cprow[fpc] = -1;

        cprows[buid] = lp.add_row(std::move(cprow));
      }
    }

    if (has_loss && block_tmax_ba > 0.0) {
      //  adding flown variable

      const auto fnc = lp.add_col(
          {// flow variable
           .name = sc.lp_label(scenario, stage, block, cname, "fn", uid()),
           .lowb = 0,
           .uppb = block_tmax_ba,
           .cost = block_tcost});

      fncols[buid] = fnc;

      // adding flown to the bus balances
      brow_b[fnc] = -(1 + stage_lossfactor);
      brow_a[fnc] = +1;

      // adding the capacity constraint
      if (capacity_col) {
        SparseRow cnrow {
            .name = sc.lp_label(scenario, stage, block, cname, "capn", uid())};
        cnrow[capacity_col.value()] = 1;
        cnrow[fnc] = -1;

        cnrows[buid] = lp.add_row(std::move(cnrow.greater_equal(0)));
      }
    }
  }

  const auto st_key = std::pair {scenario.uid(), stage.uid()};

  // adding the Kirchhoff relations if there is a line reactance
  if (const auto& stage_reactance = sc.stage_reactance(stage, reactance)) {
    const auto& theta_a_cols =
        bus_a_lp.theta_cols_at(sc, scenario, stage, lp, blocks);
    const auto& theta_b_cols =
        bus_b_lp.theta_cols_at(sc, scenario, stage, lp, blocks);

    if (!theta_a_cols.empty() && !theta_b_cols.empty()) {
      const double scale_theta = sc.options().scale_theta();
      const double X = stage_reactance.value();
      const double V = voltage.at(stage.uid()).value_or(1);
      const double x = scale_theta * (X / (V * V));

      BIndexHolder<RowIndex> trows;
      trows.reserve(blocks.size());

      for (const auto& block : blocks) {
        const auto buid = block.uid();
        auto trow =
            SparseRow {.name = sc.lp_label(
                           scenario, stage, block, cname, "theta", uid())}
                .equal(0);

        trow.reserve(4);

        trow[theta_a_cols.at(buid)] = 1;
        trow[theta_b_cols.at(buid)] = -1;
        if (!fpcols.empty()) {
          trow[fpcols.at(buid)] = x;
        }
        if (!fncols.empty()) {
          trow[fncols.at(buid)] = -x;
        }

        trows[buid] = lp.add_row(std::move(trow));
      }

      theta_rows[st_key] = std::move(trows);
    }
  }

  // storing the indices for this scenario and stage
  capacityp_rows[st_key] = std::move(cprows);
  capacityn_rows[st_key] = std::move(cnrows);
  flowp_cols[st_key] = std::move(fpcols);
  flown_cols[st_key] = std::move(fncols);

  return true;
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

  return CapacityBase::add_to_output(out);
}

}  // namespace gtopt
