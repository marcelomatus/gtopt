/**
 * @file      bus_lp.cpp
 * @brief     Implementation of BusLP class for bus linear programming
 * @date      Sat Feb  8 01:50:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the BusLP class, providing the linear programming
 * representation of electrical buses in power system optimization. It handles
 * Kirchhoff's law constraints, phase angle variables (theta), and power
 * balance equations using modern C++26 features.
 */

#include <gtopt/bus_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

[[nodiscard]]
auto BusLP::needs_kirchhoff(const SystemContext& sc) const -> bool
{
  const auto& opts = sc.options();
  return !opts.use_single_bus() && opts.use_kirchhoff()
      && bus().needs_kirchhoff(opts.kirchhoff_threshold());
}

auto BusLP::lazy_add_theta(const SystemContext& sc,
                           const ScenarioLP& scenario,
                           const StageLP& stage,
                           LinearProblem& lp,
                           std::span<const BlockLP> blocks) const
    -> const BIndexHolder<ColIndex>&
{
  static constexpr std::string_view cname = ClassName.short_name();

  BIndexHolder<ColIndex> tblocks;
  map_reserve(tblocks, blocks.size());

  const auto scale_theta = sc.options().scale_theta();

  if (stage.is_active() && needs_kirchhoff(sc)) [[likely]] {
    std::ranges::for_each(
        blocks,
        [&](const BlockLP& block)
        {
          const auto buid = block.uid();
          auto tname =
              sc.lp_label(scenario, stage, block, cname, "theta", uid());

          const auto& theta = reference_theta();
          if (theta) [[unlikely]] {
            tblocks[buid] = lp.add_col(SparseCol {
                .name = std::move(tname),
                .lowb = *theta * scale_theta,
                .uppb = *theta * scale_theta,
            });
          } else [[likely]] {
            constexpr double theta_bound =
                2 * std::numbers::pi;  // Default bound for theta
            tblocks[buid] = lp.add_col(SparseCol {
                .name = std::move(tname),
                .lowb = -theta_bound * scale_theta,
                .uppb = +theta_bound * scale_theta,
            });
          }
        });
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
  map_reserve(brows, blocks.size());

  std::ranges::for_each(
      blocks,
      [&](const BlockLP& block)
      {
        brows[block.uid()] = lp.add_row(
            {.name = sc.lp_label(scenario, stage, block, cname, "bal", uid())});
      });

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
                  [inv_scale_theta](auto&& value)
                  { return value * inv_scale_theta; });

  out.add_col_cost(cname,
                   "theta",
                   pid,
                   theta_cols,
                   [inv_scale_theta](auto&& value)
                   { return value * inv_scale_theta; });

  return true;
}

}  // namespace gtopt
