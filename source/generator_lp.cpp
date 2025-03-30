#include <ranges>

#include <gtopt/generator_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>

namespace gtopt
{

GeneratorLP::GeneratorLP(const InputContext& ic, Generator&& pgenerator)
    : CapacityBase(ic, ClassName, std::move(pgenerator))
    , pmin(ic, ClassName, id(), std::move(generator().pmin))
    , pmax(ic, ClassName, id(), std::move(generator().pmax))
    , lossfactor(ic, ClassName, id(), std::move(generator().lossfactor))
    , gcost(ic, ClassName, id(), std::move(generator().gcost))
    , bus_index(ic.element_index(bus()))
{
}

bool GeneratorLP::add_to_lp(const SystemContext& sc, LinearProblem& lp)
{
  constexpr std::string_view cname = "gen";
  if (!CapacityBase::add_to_lp(sc, lp, cname)) {
    return false;
  }

  const auto stage_index = sc.stage_index();
  if (!is_active(stage_index)) {
    return true;
  }
  auto&& bus = sc.element(bus_index);
  if (!bus.is_active(stage_index)) {
    return true;
  }
  const auto scenery_index = sc.scenery_index();

  auto&& [stage_capacity, capacity_col] = capacity_and_col(sc, lp);

  const auto stage_gcost = gcost.optval(stage_index).value_or(0.0);
  const auto stage_lossfactor = lossfactor.optval(stage_index).value_or(0.0);

  auto&& balance_rows = bus.balance_rows_at(scenery_index, stage_index);
  auto&& [blocks, block_indexes] = sc.stage_blocks_and_indexes();

  BIndexHolder gcols;
  gcols.reserve(blocks.size());
  BIndexHolder crows;
  crows.reserve(blocks.size());

  for (auto&& [block_index, block, balance_row] :
       std::ranges::views::zip(block_indexes, blocks, balance_rows))
  {
    const auto [block_pmax, block_pmin] =
        sc.block_maxmin_at(block_index, pmax, pmin, stage_capacity);

    const auto gc =
        lp.add_col({.name = sc.stb_label(block, cname, "gen", uid()),
                    .lowb = block_pmin,
                    .uppb = block_pmax,
                    .cost = sc.block_cost(block, stage_gcost)});
    gcols.push_back(gc);

    // adding gc to the bus balance
    auto& brow = lp.row_at(balance_row);
    brow[gc] = 1 - stage_lossfactor;

    // adding the capacity constraint
    if (capacity_col.has_value()) {
      SparseRow crow {.name = sc.stb_label(block, cname, "cap", uid())};
      crow[capacity_col.value()] = 1;
      crow[gc] = -1;

      crows.push_back(lp.add_row(std::move(crow.greater_equal(0))));
    }
  }
  return sc.emplace_bholder(capacity_rows, std::move(crows)).second
      && sc.emplace_bholder(generation_cols, std::move(gcols)).second;
}

bool GeneratorLP::add_to_output(OutputContext& out) const
{
  constexpr std::string_view cname = ClassName;

  const auto pid = id();
  out.add_col_sol(cname, "generation", pid, generation_cols);
  out.add_col_cost(cname, "generation", pid, generation_cols);
  out.add_row_dual(cname, "capacity", pid, capacity_rows);

  return CapacityBase::add_to_output(out, cname);
}

}  // namespace gtopt
