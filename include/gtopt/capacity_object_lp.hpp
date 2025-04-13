/**
 * @file      capacity_object_lp.hpp
 * @brief     Header of
 * @date      Thu Mar 27 10:50:18 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/capacity.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

template<typename Object>
struct CapacityObjectLP : public ObjectLP<Object>
{
  using Base = ObjectLP<Object>;
  using Base::id;
  using Base::is_active;
  using Base::object;
  using Base::uid;

  template<typename CAttrs>
  auto& set_attrs(const InputContext& ic,
                  const std::string_view& ClassName,
                  CAttrs& attrs)
  {
    capacity =
        decltype(capacity) {ic, ClassName, id(), std::move(attrs.capacity)};
    expcap = decltype(expcap) {ic, ClassName, id(), std::move(attrs.expcap)};
    capmax = decltype(capmax) {ic, ClassName, id(), std::move(attrs.capmax)};
    expmod = decltype(expmod) {ic, ClassName, id(), std::move(attrs.expmod)};
    annual_capcost = decltype(annual_capcost) {
        ic, ClassName, id(), std::move(attrs.annual_capcost)};
    annual_derating = decltype(annual_derating) {
        ic, ClassName, id(), std::move(attrs.annual_derating)};

    return *this;
  }

  template<typename ObjectT>
  explicit CapacityObjectLP(const InputContext& ic,
                            const std::string_view& ClassName,
                            ObjectT&& pobject)
      : ObjectLP<Object>(ic, ClassName, std::forward<ObjectT>(pobject))
  {
    set_attrs(ic, ClassName, object());
  }

  template<typename SystemContext,
           typename Out = std::pair<double, std::optional<size_t>>>
  constexpr auto capacity_and_col(const SystemContext& sc,
                                  LinearProblem& lp) const -> Out
  {
    const auto stage_index = sc.stage_index();
    auto&& capacity_col = capacity_col_at(stage_index);
    if (capacity_col.has_value()) {
      return {lp.get_col_uppb(capacity_col.value()), capacity_col};
    }

    return {capacity_at(stage_index), {}};
  }

  constexpr auto capacity_at(const StageIndex stage_index,
                             const double def_capacity = CoinDblMax) const
  {
    return capacity.at(stage_index).value_or(def_capacity);
  }

  constexpr auto capacity_at(const std::optional<StageIndex>& stage_index,
                             const double def_capacity = CoinDblMax) const
  {
    return stage_index.has_value()
        ? capacity_at(stage_index.value(), def_capacity)
        : def_capacity;
  }

  bool add_to_lp(const SystemContext& sc,
                 LinearProblem& lp,
                 const std::string_view& cname)
  {
    if (!sc.is_first_scenario()) {
      return true;
    }

    const auto stage_index = sc.stage_index();
    if (!is_active(stage_index)) {
      return true;
    }

    const auto stage_expcap = expcap.at(stage_index).value_or(0.0);
    const auto stage_capmax = capmax.at(stage_index);
    const auto stage_expmod = expmod.at(stage_index).value_or(0.0);
    const auto stage_maxexpcap = stage_expcap * stage_expmod;

    const auto prev_stage_index =
        !sc.is_first_stage() ? OptStageIndex {stage_index - 1} : std::nullopt;

    const auto prev_capainst_col =
        get_optvalue_optkey(capainst_cols, prev_stage_index);

    if (!prev_capainst_col.has_value() && stage_maxexpcap <= 0) {
      return true;
    }

    const auto stage_capacity = capacity_at(stage_index);
    const auto stage_hour_capcost =
        annual_capcost.at(stage_index).value_or(0.0) / avg_year_hours;
    const auto prev_stage_capacity =
        capacity_at(prev_stage_index, stage_capacity);
    const auto prev_capacost_col =
        get_optvalue_optkey(capacost_cols, prev_stage_index);
    const auto hour_derating =
        annual_derating.at(stage_index).value_or(0.0) / avg_year_hours;
    const auto stage_derating =
        hour_derating * sc.stage_duration(prev_stage_index);

    SparseRow capainst_row {.name = sc.t_label(cname, "capainst", uid())};
    SparseRow capacost_row {.name = sc.t_label(cname, "capacost", uid())};

    const auto capainst_lb = stage_capacity;
    const auto capainst_ub = stage_capmax.has_value()
        ? stage_capmax.value()
        : stage_maxexpcap + capainst_lb;

    const auto capainst_col = lp.add_col({// capainst variable
                                          .name = capainst_row.name,
                                          .lowb = capainst_lb,
                                          .uppb = capainst_ub});

    capainst_row[capainst_col] = -1;

    const auto capacost_col = lp.add_col({// capacost variable
                                          .name = capacost_row.name,
                                          .cost = sc.stage_cost(1.0)});

    capacost_row[capacost_col] = +1;

    if (stage_maxexpcap > 0) {
      const auto expmod_col = expmod_cols[stage_index] =
          lp.add_col({// expmod variable
                      .name = sc.t_label(cname, "expmod", uid()),
                      .uppb = stage_expmod});

      capainst_row[expmod_col] = +stage_expcap;
      capacost_row[expmod_col] = -stage_expcap * stage_hour_capcost;
    }

    if (prev_capainst_col.has_value()) {
      capainst_row[prev_capainst_col.value()] = +(1 - stage_derating);
    }

    if (prev_capacost_col.has_value()) {
      capacost_row[prev_capacost_col.value()] = -1;
    }

    const auto dcap = prev_stage_capacity - stage_capacity;

    return capainst_cols.emplace(stage_index, capainst_col).second
        && capacost_cols.emplace(stage_index, capacost_col).second
        && capainst_rows
               .emplace(stage_index,
                        lp.add_row(std::move(capainst_row.equal(dcap))))
               .second
        && capacost_rows
               .emplace(stage_index,
                        lp.add_row(std::move(capacost_row.equal(0.0))))
               .second;
  }

  bool add_to_output(OutputContext& out, std::string_view cname) const
  {
    out.add_col_sol(cname, "capainst", id(), capainst_cols);
    out.add_col_sol(cname, "capacost", id(), capacost_cols);
    out.add_col_sol(cname, "expmod", id(), expmod_cols);

    out.add_col_cost(cname, "capainst", id(), capainst_cols);
    out.add_col_cost(cname, "capacost", id(), capacost_cols);
    out.add_col_cost(cname, "expmod", id(), expmod_cols);

    out.add_row_dual(cname, "capainst", id(), capainst_rows);
    out.add_row_dual(cname, "capacost", id(), capacost_rows);

    return true;
  }

  constexpr auto capacity_col_at(const StageIndex stage_index) const
  {
    return get_optvalue(capainst_cols, stage_index);
  }

private:
  OptTRealSched capacity;
  OptTRealSched expcap;
  OptTRealSched capmax;
  OptTRealSched annual_capcost;
  OptTRealSched expmod;
  OptTRealSched annual_derating;

  TIndexHolder capainst_cols;
  TIndexHolder capacost_cols;
  TIndexHolder expmod_cols;
  TIndexHolder capainst_rows;
  TIndexHolder capacost_rows;
};

}  // namespace gtopt
