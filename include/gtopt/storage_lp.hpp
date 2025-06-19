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

#include <gtopt/index_holder.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/scenario_lp.hpp>

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

  constexpr auto vfin_col_at(const ScenarioUid scenario,
                             const StageUid stage) const
  {
    return vfin_cols.at({scenario, stage});
  }

  constexpr auto vini_col_at(const ScenarioUid scenario,
                             const StageUid stage) const
  {
    return vini_cols.at({scenario, stage});
  }

  template<typename SystemContextT>
  bool add_to_lp(const SystemContextT& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp,
                 const std::string_view cname,
                 const BIndexHolder& rcols,
                 double stage_capacity,
                 std::optional<Index> capacity_col = {})
  {
    if (!is_active(stage)) {
      return true;
    }

    const auto [prev_stage, prev_phase] = sc.prev_stage(stage);

    const auto stage_vcost = sc.scenario_stage_ecost(  //
                                 scenario,
                                 stage,
                                 vcost.at(stage.uid()).value_or(0.0))
        / stage.duration();

    const auto hour_loss =
        annual_loss.at(stage.uid()).value_or(0.0) / hours_per_year;

    const auto [stage_vmax, stage_vmin] =
        sc.stage_maxmin_at(stage, vmax, vmin, stage_capacity);

    const auto vicol = prev_stage
        ? vfin_col_at(scenario.uid(), prev_stage->uid())
        : lp.add_col(
              {.name = sc.st_label(scenario, stage, cname, "vini", uid()),
               .lowb = storage().vini.value_or(stage_vmin),
               .uppb = storage().vini.value_or(stage_vmax)});

    const auto& blocks = stage.blocks();

    BIndexHolder vcols;
    vcols.reserve(blocks.size());
    BIndexHolder vrows;
    vrows.reserve(blocks.size());
    BIndexHolder crows;
    crows.reserve(blocks.size());

    auto prev_vc = vicol;
    for (const auto& block : blocks) {
      const auto buid = block.uid();
      const auto is_last = buid == blocks.back().uid();

      auto vrow = SparseRow {.name = sc.stb_label(
                                 scenario, stage, block, cname, "vol", uid())}
                      .equal(0);

      const auto vc = lp.add_col(
          {.name = vrow.name,
           .lowb = !is_last ? stage_vmin : storage().vfin.value_or(stage_vmin),
           .uppb = stage_vmax,
           .cost = stage_vcost});

      vcols[buid] = vc;

      vrow[prev_vc] = -(1 - (hour_loss * block.duration()));
      vrow[vc] = 1;

      vrow[rcols.at(buid)] = block.duration();

      vrows[buid] = lp.add_row(std::move(vrow));

      // adding the capacity constraint
      if (capacity_col) {
        auto crow = SparseRow {.name = sc.stb_label(
                                   scenario, stage, block, cname, "cap", uid())}
                        .greater_equal(0);
        crow[*capacity_col] = 1;
        crow[vc] = -1;

        crows[buid] = lp.add_row(std::move(crow));
      }

      prev_vc = vc;
    }

    return (crows.empty()
            || emplace_bholder(scenario, stage, capacity_rows, std::move(crows))
                   .second)
        && emplace_value(scenario, stage, vini_cols, vicol).second
        && emplace_value(scenario, stage, vfin_cols, prev_vc).second
        && emplace_bholder(scenario, stage, volumen_rows, std::move(vrows))
               .second
        && emplace_bholder(scenario, stage, volumen_cols, std::move(vcols))
               .second;
  }

  template<typename OutputContext>
  bool add_to_output(OutputContext& out, std::string_view cname) const
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
