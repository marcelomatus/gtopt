#include <gtopt/bus_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

auto BusLP::needs_kirchhoff(const SystemContext& sc) const noexcept -> bool
{
  return !sc.options().use_single_bus() && sc.options().use_kirchhoff()
      && bus().needs_kirchhoff(sc.options().kirchhoff_threshold());
}

auto BusLP::lazy_add_theta(const SystemContext& sc,
                           const ScenarioLP& scenario,
                           const StageLP& stage,
                           LinearProblem& lp,
                           const std::vector<BlockLP>& blocks) const
    -> const BIndexHolder<ColIndex>&
{
  BIndexHolder<ColIndex> tblocks;
  tblocks.reserve(blocks.size());

  if (stage.is_active() && needs_kirchhoff(sc)) {
    const auto& theta = reference_theta();

    for (auto&& block : blocks) {
      SparseCol theta_col {.name =
                               lp_label(sc, scenario, stage, block, "theta")};
      const auto tc = lp.add_col(theta ? std::move(theta_col.equal(*theta))
                                       : std::move(theta_col.free()));
      tblocks[block.uid()] = tc;
    }
  }

  constexpr bool EmptyOk = true;
  const auto [iter, inserted] =
      emplace_bholder(scenario, stage, theta_cols, std::move(tblocks), EmptyOk);

  if (inserted) [[likely]] {
    return iter->second;
  }

  const auto* const msg = "can't insert a new theta index holder";
  SPDLOG_CRITICAL(msg);
  throw std::runtime_error(msg);
}

bool BusLP::add_to_lp(const SystemContext& sc,
                      const ScenarioLP& scenario,
                      const StageLP& stage,
                      LinearProblem& lp)
{
  if (!is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();

  BIndexHolder<RowIndex> brows;
  brows.reserve(blocks.size());

  for (auto&& block : blocks) {
    brows[block.uid()] =
        lp.add_row({.name = lp_label(sc, scenario, stage, block, "bal")});
  }

  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  balance_rows[st_key] = std::move(brows);

  return true;
}

bool BusLP::add_to_output(OutputContext& out) const
{
  constexpr std::string_view cname = ClassName;
  const auto pid = id();

  out.add_row_dual(cname, "balance", pid, balance_rows);

  const auto inv_scale_theta = 1.0 / out.options().scale_theta();
  out.add_col_sol(cname,
                  "theta",
                  pid,
                  theta_cols,
                  [=](auto&& value) { return value * inv_scale_theta; });

  out.add_col_cost(cname,
                   "theta",
                   pid,
                   theta_cols,
                   [=](auto&& value) { return value * inv_scale_theta; });

  return true;
}

}  // namespace gtopt
