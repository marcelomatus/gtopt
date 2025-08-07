#include <gtopt/bus_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

[[nodiscard]]
auto BusLP::needs_kirchhoff(const SystemContext& sc) const noexcept -> bool
{
  const auto& opts = sc.options();
  return !opts.use_single_bus() && opts.use_kirchhoff()
      && bus().needs_kirchhoff(opts.kirchhoff_threshold());
}

auto BusLP::lazy_add_theta(const SystemContext& sc,
                           const ScenarioLP& scenario,
                           const StageLP& stage,
                           LinearProblem& lp,
                           const std::vector<BlockLP>& blocks) const
    -> const BIndexHolder<ColIndex>&
{
  static constexpr std::string_view cname = ClassName.short_name();

  BIndexHolder<ColIndex> tblocks;
  tblocks.reserve(blocks.size());

  if (stage.is_active() && needs_kirchhoff(sc)) {
    const auto& theta = reference_theta();

    for (auto&& block : blocks) {
      SparseCol theta_col {
          .name = sc.lp_label(scenario, stage, block, cname, "theta", uid())};
      const auto tc = lp.add_col(theta ? std::move(theta_col.equal(*theta))
                                       : std::move(theta_col.free()));
      tblocks[block.uid()] = tc;
    }
  }

  // Store the theta columns for this scenario and stage
  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  return theta_cols[st_key] = std::move(tblocks);
}

bool BusLP::add_to_lp(const SystemContext& sc,
                      const ScenarioLP& scenario,
                      const StageLP& stage,
                      LinearProblem& lp)
{
  static constexpr std::string_view cname = ClassName.short_name();

  if (!is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();

  BIndexHolder<RowIndex> brows;
  brows.reserve(blocks.size());

  for (auto&& block : blocks) {
    brows[block.uid()] = lp.add_row(
        {.name = sc.lp_label(scenario, stage, block, cname, "bal", uid())});
  }

  const auto st_key = std::pair {scenario.uid(), stage.uid()};
  balance_rows[st_key] = std::move(brows);

  return true;
}

bool BusLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();
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
