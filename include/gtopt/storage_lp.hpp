/**
 * @file      storage_lp.hpp
 * @brief     Header of
 * @date      Wed Apr  2 01:47:11 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/input_context.hpp>
#include <gtopt/object_lp.hpp>

#include "gtopt/output_context.hpp"

namespace gtopt
{

template<typename Object>
class StorageLP : public Object
{
public:
  using Object::id;
  using Object::is_active;
  using Object::object;
  using Object::uid;

  constexpr auto&& storage() { return object(); }
  constexpr auto&& storage() const { return object(); }

  template<typename ObjectT>
  explicit StorageLP(const InputContext& ic,
                     const std::string_view& ClassName,
                     ObjectT&& pstorage)
      : Object(ic, ClassName, std::forward<ObjectT>(pstorage))
      , vmin(ic, ClassName, id(), std::move(storage().vmin))
      , vmax(ic, ClassName, id(), std::move(storage().vmax))
      , vcost(ic, ClassName, id(), std::move(storage().vcost))
      , annual_loss(ic, ClassName, id(), std::move(storage().annual_loss))
  {
  }

  constexpr auto vfin_col_at(const ScenarioIndex scenario,
                             const StageIndex stage) const
  {
    return vfin_cols.at({scenario, stage});
  }

  constexpr auto vini_col_at(const ScenarioIndex scenario,
                             const StageIndex stage) const
  {
    return vini_cols.at({scenario, stage});
  }

  bool add_to_lp(const SystemContext& sc,
                 LinearProblem& lp,
                 const std::string_view& cname,
                 const BIndexHolder& rcols,
                 double stage_capacity,
                 std::optional<size_t> capacity_col = {})
  {
    const auto stage_index = sc.stage_index();
    if (!is_active(stage_index)) {
      return true;
    }

    const auto scenario_index = sc.scenario_index();

    const auto prev_stage_index = !sc.is_first_stage()
        ? OptStageIndex {stage_index - 1}
        : OptStageIndex {};

    const auto stage_vcost = sc.stage_cost(vcost.at(stage_index).value_or(0.0))
        / sc.stage_duration();

    const auto hour_loss =
        annual_loss.at(stage_index).value_or(0.0) / avg_year_hours;

    const auto [stage_vmax, stage_vmin] =
        sc.stage_maxmin_at(vmax, vmin, stage_capacity);

    const auto vicol = prev_stage_index.has_value()
        ? vfin_col_at(scenario_index, prev_stage_index.value())
        : lp.add_col({.name = sc.st_label(cname, "vini", uid()),
                      .lowb = storage().vini.value_or(stage_vmin),
                      .uppb = storage().vini.value_or(stage_vmax)});

    auto&& [blocks, block_indexes] = sc.stage_blocks_and_indexes();

    BIndexHolder vcols;
    vcols.reserve(blocks.size());
    BIndexHolder vrows;
    vrows.reserve(blocks.size());
    BIndexHolder crows;
    crows.reserve(blocks.size());

    for (size_t prev_vc = vicol; auto&& [block_index, block] :
                                 ranges::views::zip(block_indexes, blocks))
    {
      SparseRow vrow {.name = sc.stb_label(block, cname, "vol", uid())};

      const auto is_last =
          block_index == blocks.size() - 1 && sc.is_last_stage();

      const auto vc = lp.add_col(
          {.name = vrow.name,
           .lowb = !is_last ? stage_vmin : storage().vfin.value_or(stage_vmin),
           .uppb = stage_vmax,
           .cost = stage_vcost});

      vcols.push_back(vc);

      vrow[prev_vc] = -(1 - (hour_loss * block.duration()));
      vrow[vc] = 1;
      vrow[rcols[block_index]] = block.duration();

      vrows.push_back(lp.add_row(std::move(vrow.equal(0))));

      prev_vc = vc;

      // adding the capacity constraint
      if (capacity_col.has_value()) {
        SparseRow crow {.name = sc.stb_label(block, cname, "cap", uid())};
        crow[capacity_col.value()] = 1;
        crow[vc] = -1;

        crows.push_back(lp.add_row(std::move(crow.greater_equal(0))));
      }
    }

    return (crows.empty()
            || sc.emplace_bholder(capacity_rows, std::move(crows)).second)
        && sc.emplace_value(vini_cols, vicol).second
        && sc.emplace_value(vfin_cols, vcols.back()).second
        && sc.emplace_bholder(volumen_rows, std::move(vrows)).second
        && sc.emplace_bholder(volumen_cols, std::move(vcols)).second;
  }

  template<typename OutputContext>
  bool add_to_output(OutputContext& out, const std::string_view& cname) const
  {
    const auto pid = id();

    out.add_col_sol(cname, "vini", pid, vini_cols);
    out.add_col_cost(cname, "vini", pid, vini_cols);
    out.add_col_sol(cname, "vfin", pid, vfin_cols);
    out.add_col_cost(cname, "vfin", pid, vfin_cols);

    out.add_col_sol(cname, "volumen", pid, volumen_cols);
    out.add_col_cost(cname, "volumen", pid, volumen_cols);
    out.add_row_dual(cname, "volumen", pid, volumen_rows);

    out.add_row_dual(cname, "capacity", pid, capacity_rows);

    return true;
  }

private:
  OptTRealSched vmin;
  OptTRealSched vmax;
  OptTRealSched vcost;

  OptTRealSched annual_loss;

  STBIndexHolder volumen_cols;
  STBIndexHolder volumen_rows;
  STBIndexHolder capacity_rows;

  STIndexHolder vini_cols;
  STIndexHolder vfin_cols;
};

}  // namespace gtopt
