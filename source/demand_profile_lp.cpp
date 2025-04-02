#include <ranges>

#include <gtopt/demand_profile_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

DemandProfileLP::DemandProfileLP(InputContext& ic,
                                 DemandProfile&& pdemand_profile)
    : ObjectLP<DemandProfile>(ic, ClassName, std::move(pdemand_profile))
    , scost(ic, ClassName, id(), std::move(demand_profile().scost))
    , profile(ic, ClassName, id(), std::move(demand_profile().profile))
    , demand_index(ic.make_element_index<DemandLP>(demand_profile(), demand()))
{
}

bool DemandProfileLP::add_to_lp(const SystemContext& sc, LinearProblem& lp)
{
  constexpr std::string_view cname = "dprof";
  const auto stage_index = sc.stage_index();
  if (!is_active(stage_index)) {
    return true;
  }

  const auto scenery_index = sc.scenery_index();

  auto&& demand_lp = sc.element(demand_index);
  if (!demand_lp.is_active(stage_index)) {
    return true;
  }

  auto&& load_cols = demand_lp.load_cols_at(scenery_index, stage_index);

  const auto [stage_capacity, capacity_col] =
      demand_lp.capacity_and_col(sc, lp);

  if (!capacity_col && !demand_lp.demand().capacity) {
    SPDLOG_WARN("requires that Demand defines capacity or expansion");
    return false;
  }

  const auto stage_scost = scost.optval(stage_index).value_or(0.0);

  auto&& [blocks, block_indexes] = sc.stage_blocks_and_indexes();

  BIndexHolder scols;
  scols.reserve(blocks.size());
  BIndexHolder srows;
  srows.reserve(blocks.size());
  for (auto&& [block_index, block, lcol] :
       std::ranges::views::zip(block_indexes, blocks, load_cols))
  {
    const auto block_profile =
        profile.at(scenery_index, stage_index, block_index);

    const auto block_scost = sc.block_cost(block, stage_scost);
    auto name = sc.stb_label(block, cname, "prof", uid());
    const auto scol = lp.add_col({.name = name, .cost = block_scost});
    scols.push_back(scol);

    SparseRow srow {.name = std::move(name)};
    srow[scol] = 1;
    srow[lcol] = 1;

    if (capacity_col) {
      srow[capacity_col.value()] = -block_profile;
      srows.push_back(lp.add_row(std::move(srow.greater_equal(0))));
    } else {
      const auto cprofile = stage_capacity * block_profile;
      srows.push_back(lp.add_row(std::move(srow.greater_equal(cprofile))));
    }
  }

  return sc.emplace_bholder(spillover_cols, std::move(scols)).second
      && sc.emplace_bholder(spillover_rows, std::move(srows)).second;
}

bool DemandProfileLP::add_to_output(OutputContext& out) const
{
  constexpr std::string_view cname = ClassName;

  out.add_col_sol(cname, "unserved", id(), spillover_cols);
  out.add_row_dual(cname, "unserved", id(), spillover_rows);

  return true;
}

}  // namespace gtopt
