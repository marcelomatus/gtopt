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

#include <limits>

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

  [[nodiscard]] constexpr auto&& storage(this auto&& self) noexcept
  {
    // Forward the object() call with same value category as self
    return std::forward_like<decltype(self)>(self.object());
  }

  template<typename ObjectT>
  explicit StorageLP(ObjectT&& pstorage,
                     const InputContext& ic,
                     const LPClassName& cname)
      : Object(std::forward<ObjectT>(pstorage), ic, cname)
      , vmin(ic, cname.full_name(), id(), std::move(storage().vmin))
      , vmax(ic, cname.full_name(), id(), std::move(storage().vmax))
      , vcost(ic, cname.full_name(), id(), std::move(storage().vcost))
      , annual_loss(
            ic, cname.full_name(), id(), std::move(storage().annual_loss))
  {
  }

  [[nodiscard]] constexpr auto vfin_col_at(const ScenarioLP& scenario,
                                           const StageLP& stage) const
  {
    return vfin_cols.at({scenario.uid(), stage.uid()});
  }

  [[nodiscard]] constexpr auto vini_col_at(const ScenarioLP& scenario,
                                           const StageLP& stage) const
  {
    return vini_cols.at({scenario.uid(), stage.uid()});
  }

  template<typename SystemContextT>
  bool add_to_lp(const std::string_view& cname,
                 const SystemContextT& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp,
                 const double flow_conversion_rate,
                 const BIndexHolder<ColIndex>& finp_cols,
                 const double finp_efficiency,
                 const BIndexHolder<ColIndex>& fout_cols,
                 const double fout_efficiency,
                 const double stage_capacity,
                 const std::optional<ColIndex> capacity_col = {},
                 const std::optional<Real> drain_cost = {},
                 const std::optional<Real> drain_capacity = {})
  {
    if (!is_active(stage)) {
      return true;
    }

    const auto is_last_stage =
        stage.uid() == sc.simulation().stages().back().uid();
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
        ? vfin_col_at(scenario, *prev_stage)
        : lp.add_col({
              .name = sc.lp_label(scenario, stage, cname, "vini", uid()),
              .lowb = storage().vini.value_or(stage_vmin),
              .uppb = storage().vini.value_or(stage_vmax),
          });

    const auto& blocks = stage.blocks();

    BIndexHolder<ColIndex> vcols;
    BIndexHolder<ColIndex> dcols;
    BIndexHolder<RowIndex> vrows;
    BIndexHolder<RowIndex> crows;
    vcols.reserve(blocks.size());
    vrows.reserve(blocks.size());
    crows.reserve(blocks.size());
    dcols.reserve(blocks.size());

    auto prev_vc = vicol;
    for (const auto& block : blocks) {
      const auto buid = block.uid();
      const auto is_last_block = is_last_stage && (buid == blocks.back().uid());

      auto vrow =
          SparseRow {
              .name = sc.lp_label(scenario, stage, block, cname, "vol", uid()),
          }
              .equal(0);

      const auto vc = lp.add_col({
          .name = vrow.name,
          .lowb =
              !is_last_block ? stage_vmin : storage().vfin.value_or(stage_vmin),
          .uppb = stage_vmax,
          .cost = stage_vcost,
      });

      vcols[buid] = vc;

      vrow[prev_vc] = -(1 - (hour_loss * block.duration()));
      vrow[vc] = 1;

      const auto fout_col = fout_cols.at(buid);
      const auto finp_col = finp_cols.at(buid);
      vrow[fout_col] =
          +(flow_conversion_rate / fout_efficiency) * block.duration();

      // if the input and output are the same, we only need one entry
      if (fout_col != finp_col) {
        vrow[finp_col] =
            -(flow_conversion_rate * finp_efficiency) * block.duration();
      }

      if (drain_cost) {
        const auto dcol = lp.add_col({
            .name = sc.lp_label(scenario, stage, block, cname, "drain", uid()),
            .lowb = 0,
            .uppb = drain_capacity.value_or(LinearProblem::DblMax),
            .cost = sc.block_ecost(scenario, stage, block, *drain_cost),
        });

        dcols[buid] = dcol;
        vrow[dcol] = flow_conversion_rate * block.duration();
      }

      vrows[buid] = lp.add_row(std::move(vrow));

      // adding the capacity constraint
      if (capacity_col) {
        auto crow =
            SparseRow {
                .name =
                    sc.lp_label(scenario, stage, block, cname, "cap", uid()),
            }
                .greater_equal(0);
        crow[*capacity_col] = 1;
        crow[vc] = -1;

        crows[buid] = lp.add_row(std::move(crow));
      }

      prev_vc = vc;
    }

    // storing the indices for this scenario and stage
    const auto st_key = std::pair {scenario.uid(), stage.uid()};
    vini_cols[st_key] = vicol;
    vfin_cols[st_key] = prev_vc;
    volumen_rows[st_key] = std::move(vrows);
    volumen_cols[st_key] = std::move(vcols);
    if (drain_cost) {
      drain_cols[st_key] = std::move(dcols);
    }

    if (!crows.empty()) {
      capacity_rows[st_key] = std::move(crows);
    }

    return true;
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

    out.add_col_sol(cname, "drain", pid, drain_cols);
    out.add_col_cost(cname, "drain", pid, drain_cols);

    return true;
  }

private:
  OptTRealSched vmin;
  OptTRealSched vmax;
  OptTRealSched vcost;

  OptTRealSched annual_loss;

  STBIndexHolder<ColIndex> volumen_cols;
  STBIndexHolder<ColIndex> drain_cols;
  STBIndexHolder<RowIndex> volumen_rows;
  STBIndexHolder<RowIndex> capacity_rows;

  STIndexHolder<ColIndex> vini_cols;
  STIndexHolder<ColIndex> vfin_cols;
};

}  // namespace gtopt
