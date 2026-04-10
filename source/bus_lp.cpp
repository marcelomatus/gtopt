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
  BIndexHolder<ColIndex> tblocks;
  map_reserve(tblocks, blocks.size());

  if (stage.is_active() && needs_kirchhoff(sc)) [[likely]] {
    std::ranges::for_each(
        blocks,
        [&](const BlockLP& block)
        {
          const auto buid = block.uid();
          const auto ctx =
              make_block_context(scenario.uid(), stage.uid(), block.uid());

          const auto& theta = reference_theta();
          if (theta) [[unlikely]] {
            tblocks[buid] = lp.add_col(SparseCol {
                .lowb = *theta,
                .uppb = *theta,
                .class_name = ClassName.full_name(),
                .variable_name = ThetaName,
                .variable_uid = uid(),
                .context = ctx,
            });
          } else [[likely]] {
            constexpr double theta_bound =
                2 * std::numbers::pi;  // Default bound for theta
            tblocks[buid] = lp.add_col(SparseCol {
                .lowb = -theta_bound,
                .uppb = +theta_bound,
                .class_name = ClassName.full_name(),
                .variable_name = ThetaName,
                .variable_uid = uid(),
                .context = ctx,
            });
          }
        });
  }

  // Store the theta columns for this scenario and stage
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  return theta_cols[st_key] = std::move(tblocks);
}

bool BusLP::add_to_lp(const SystemContext& sc,
                      const ScenarioLP& scenario,
                      const StageLP& stage,
                      LinearProblem& lp)
{
  static const auto ampl_name = std::string {ClassName.snake_case()};

  // Theta columns are lazy-created by lines that need Kirchhoff; the
  // resolver retains a special-case fallback for `bus.theta`/`bus.angle`
  // via `lookup_theta_col`, so we do NOT register a per-block variable
  // here — `theta_cols` may still be empty at this point in the solve
  // sequence.  The element-name registration is hoisted into
  // `system_lp.cpp::register_all_ampl_element_names` (runs once per
  // SimulationLP via std::call_once from the SystemLP constructor).

  // F9: register filter metadata for sum(...) predicates.
  if (const auto& t = bus().type) {
    AmplElementMetadata metadata;
    metadata.emplace_back(TypeKey, *t);
    sc.register_ampl_element_metadata(ampl_name, uid(), std::move(metadata));
  }

  if (!is_active(stage)) {
    return true;
  }

  const auto& blocks = stage.blocks();

  BIndexHolder<RowIndex> brows;
  map_reserve(brows, blocks.size());

  std::ranges::for_each(blocks,
                        [&](const BlockLP& block)
                        {
                          brows[block.uid()] = lp.add_row({
                              .class_name = ClassName.full_name(),
                              .constraint_name = BalanceName,
                              .variable_uid = uid(),
                              .context = make_block_context(
                                  scenario.uid(), stage.uid(), block.uid()),
                          });
                        });

  const auto st_key = std::tuple {scenario.uid(), stage.uid()};
  balance_rows[st_key] = std::move(brows);

  return true;
}

bool BusLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = ClassName.full_name();
  const auto pid = id();

  out.add_row_dual(cname, BalanceName, pid, balance_rows);

  // Physical = LP × scale_theta, auto-descaled by LinearInterface.
  out.add_col_sol(cname, ThetaName, pid, theta_cols);
  out.add_col_cost(cname, ThetaName, pid, theta_cols);

  return true;
}

}  // namespace gtopt
