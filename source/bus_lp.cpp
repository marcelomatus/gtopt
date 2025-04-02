#include <gtopt/bus_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

auto BusLP::needs_kirchhoff(const SystemContext& sc) const -> bool
{
  return !sc.options().use_single_bus() && sc.options().use_kirchhoff()
      && bus().needs_kirchhoff(sc.options().kirchhoff_threshold());
}

auto BusLP::lazy_add_theta(const SystemContext& sc,
                           LinearProblem& lp,
                           const BlockSpan& blocks) const -> const BIndexHolder&
{
  constexpr std::string_view cname = "bus";

  BIndexHolder tblocks;
  tblocks.reserve(blocks.size());

  const auto stage_index = sc.stage_index();
  if (is_active(stage_index) && needs_kirchhoff(sc)) {
    const auto& theta = reference_theta();

    for (auto&& block : blocks) {
      SparseCol theta_col {.name = sc.stb_label(block, cname, "theta", uid())};
      const auto tc =
          lp.add_col(theta ? std::move(theta_col.equal(theta.value()))
                           : std::move(theta_col.free()));
      tblocks.push_back(tc);
    }
  }

  constexpr bool EmptyOk = true;
  const auto [iter, inserted] =
      sc.emplace_bholder(theta_cols, std::move(tblocks), EmptyOk);

  if (inserted) [[likely]] {
    return iter->second;
  }

  const auto* const msg = "can't insert a new theta index holder";
  SPDLOG_CRITICAL(msg);
  throw std::runtime_error(msg);
}

bool BusLP::add_to_lp(const SystemContext& sc, LinearProblem& lp)
{
  constexpr std::string_view cname = "bus";
  if (!is_active(sc.stage_index())) {
    return true;
  }

  const auto& blocks = sc.stage_blocks();

  BIndexHolder brows;
  brows.reserve(blocks.size());

  const auto puid = uid();
  for (auto&& block : blocks) {
    brows.push_back(
        lp.add_row({.name = sc.stb_label(block, cname, "bal", puid)}));
  }

  return sc.emplace_bholder(balance_rows, std::move(brows)).second;
}

bool BusLP::add_to_output(OutputContext& out) const
{
  constexpr std::string_view cname = ClassName;

  out.add_row_dual(cname, "balance", id(), balance_rows);

  const auto inv_scale_theta = 1.0 / out.options().scale_theta();
  out.add_col_sol(cname,
                  "theta",
                  id(),
                  theta_cols,
                  [=](auto&& value) { return value * inv_scale_theta; });

  out.add_col_cost(cname,
                   "theta",
                   id(),
                   theta_cols,
                   [=](auto&& value) { return value * inv_scale_theta; });

  return true;
}

}  // namespace gtopt
