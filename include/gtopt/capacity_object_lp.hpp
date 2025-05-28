/**
 * @file      capacity_object_lp.hpp
 * @brief     Linear programming representation of capacity-constrained objects
 * @date      Thu Mar 27 10:50:18 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * @details This module defines the CapacityObjectLP template class which
 * provides:
 * - Modeling of capacity constraints in linear programming problems
 * - Support for capacity expansion with associated costs
 * - Time-phased capacity tracking across stages
 * - Integration with the GT optimization framework
 *
 * Key features:
 * - Handles both fixed and expandable capacities
 * - Tracks capacity installation and costs separately
 * - Supports derating factors for capacity degradation
 * - Provides output capabilities for solution analysis
 *
 * The class is designed to work with the GT optimization system's:
 * - LinearProblem interface
 * - Input/Output contexts
 * - Stage and scenario indexing
 *
 * @note All capacity values are in the same units as defined by the Object type
 * @see ObjectLP for the base class functionality
 */
#pragma once

#include <gtopt/capacity.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/state_variable.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{
/**
 * @brief A linear programming representation of an object with capacity
 * constraints
 *
 * @tparam Object The type of object being modeled, must provide
 * capacity-related attributes
 *
 * This class extends ObjectLP to handle capacity constraints, expansion
 * capabilities, and associated costs in a linear programming formulation.
 */
template<typename Object>
struct CapacityObjectLP : public ObjectLP<Object>
{
  using Base = ObjectLP<Object>;
  using Base::id;
  using Base::is_active;
  using Base::object;
  using Base::uid;

  /**
   * @brief Construct a new CapacityObjectLP object
   * @tparam ObjectT Type of object being wrapped (deduced)
   * @param ic Input context providing stage/scenario information
   * @param ClassName Name of the class for labeling columns/rows
   * @param pobject The object to wrap, will be moved if rvalue
   * @throws None This constructor is noexcept
   *
   * @details Initializes all capacity-related schedules from the wrapped
   * object:
   * - Base capacity
   * - Expansion capacity
   * - Maximum capacity
   * - Expansion model
   * - Annual capacity costs
   * - Annual derating factors
   */
  template<typename ObjectT>
  constexpr explicit CapacityObjectLP(const InputContext& ic,
                                      std::string_view ClassName,
                                      ObjectT&& pobject) noexcept
      : ObjectLP<Object>(std::forward<ObjectT>(pobject))
      , capacity(ic, ClassName, id(), std::move(object().capacity))
      , expcap(ic, ClassName, id(), std::move(object().expcap))
      , capmax(ic, ClassName, id(), std::move(object().capmax))
      , expmod(ic, ClassName, id(), std::move(object().expmod))
      , annual_capcost(ic, ClassName, id(), std::move(object().annual_capcost))
      , annual_derating(
            ic, ClassName, id(), std::move(object().annual_derating))
  {
  }

  /**
   * @brief Get capacity value and optional column index for a stage
   * @tparam Out Return type (defaults to pair<double, optional<Index>>)
   * @param stage_index The stage to query
   * @param lp Linear problem reference to check column bounds
   * @return Pair containing:
   *   - First: Capacity value (upper bound if column exists, else schedule
   * value)
   *   - Second: Optional column index if exists
   */
  template<typename Out = std::pair<double, std::optional<Index>>>
  [[nodiscard]] constexpr auto capacity_and_col(const StageIndex& stage_index,
                                                LinearProblem& lp) const -> Out
  {
    auto&& capacity_col = capacity_col_at(stage_index);
    if (capacity_col.has_value()) {
      return {lp.get_col_uppb(capacity_col.value()), capacity_col};
    }

    return {capacity_at(stage_index), {}};
  }

  /**
   * @brief Get the capacity at a specific stage
   * @param stage_index The stage to query capacity for
   * @param def_capacity Default value if capacity not specified (default:
   * unlimited)
   * @return The capacity at given stage or default if not specified
   * @throws None This function is noexcept
   */
  [[nodiscard]] constexpr auto capacity_at(
      StageIndex stage_index,
      double def_capacity = std::numeric_limits<double>::max()) const
  {
    return capacity.at(stage_index).value_or(def_capacity);
  }

  /**
   * @brief Get the capacity at an optional stage
   * @param stage_index Optional stage to query (returns default if nullopt)
   * @param def_capacity Default value if capacity not specified (default:
   * unlimited)
   * @return The capacity at given stage or default if not specified/available
   */
  [[nodiscard]] constexpr auto capacity_at(
      const std::optional<StageIndex>& stage_index,
      double def_capacity = std::numeric_limits<double>::max()) const
  {
    return stage_index.has_value()
        ? capacity_at(stage_index.value(), def_capacity)
        : def_capacity;
  }

  /**
   * @brief Add capacity constraints to the linear problem
   * @tparam SystemContext Type of system context (deduced)
   * @param sc System context providing stage/scenario info
   * @param scenario_index Current scenario index
   * @param stage_index Current stage index
   * @param lp Linear problem to modify
   * @param cname Class name prefix for labeling
   * @return true if successful, false otherwise
   *
   * @details Adds capacity-related variables and constraints:
   * - Capacity installation variables
   * - Capacity cost variables
   * - Expansion model variables (if applicable)
   * - Balance equations between stages
   * - Cost tracking equations
   */
  template<typename SystemContext>
  constexpr bool add_to_lp(SystemContext& sc,
                           const ScenarioLP& scenario,
                           const StageLP& stage,
                           LinearProblem& lp,
                           const std::string_view& cname)
  {
    if (!scenario.is_first()) {
      return true;
    }

    const auto stage_index = stage.index();

    const auto& expcap_val = expcap.at(stage_index);
    const auto& capmax_val = capmax.at(stage_index);
    const auto& expmod_val = expmod.at(stage_index);
    const auto stage_expcap = expcap_val.value_or(0.0);
    const auto stage_expmod = expmod_val.value_or(0.0);
    const auto stage_maxexpcap = stage_expcap * stage_expmod;

    const auto prev_stage_index = !sc.is_first_stage(stage_index)
        ? OptStageIndex {stage_index - 1}
        : std::nullopt;

    const auto prev_capainst_col =
        get_optvalue_optkey(capainst_cols, prev_stage_index);

    if (!prev_capainst_col.has_value() && stage_maxexpcap <= 0) {
      return true;
    }

    const auto stage_capacity = capacity_at(stage_index);
    const auto stage_hour_capcost =
        annual_capcost.at(stage_index).value_or(0.0) / hours_per_year;
    const auto prev_stage_capacity =
        capacity_at(prev_stage_index, stage_capacity);
    const auto prev_capacost_col =
        get_optvalue_optkey(capacost_cols, prev_stage_index);
    const auto hour_derating =
        annual_derating.at(stage_index).value_or(0.0) / hours_per_year;
    const auto stage_derating =
        hour_derating * sc.stage_duration(prev_stage_index);

    SparseRow capainst_row {
        .name = sc.t_label(stage_index, cname, "capainst", uid())};
    SparseRow capacost_row {
        .name = sc.t_label(stage_index, cname, "capacost", uid())};

    const auto capainst_lb = stage_capacity;
    const auto capainst_ub = capmax_val.has_value()
        ? capmax_val.value()
        : stage_maxexpcap + capainst_lb;

    const auto capainst_col = lp.add_col({
        .name = capainst_row.name,
        .lowb = capainst_lb,
        .uppb = capainst_ub,
        .cost = 0.0  // Explicit initialization
    });

    // const auto capainst_col_name = as_label(cname, "capainst", uid());
    // sc.add_state_variable_col(capainst_col_name, stage_index, capainst_col);

    capainst_row[capainst_col] = -1;

    const auto capacost_col =
        lp.add_col({// capacost variable
                    .name = capacost_row.name,
                    .cost = sc.stage_ecost(stage_index, 1.0)});

    capacost_row[capacost_col] = +1;

    if (stage_maxexpcap > 0) {
      const auto expmod_col = expmod_cols[stage_index] =
          lp.add_col({// expmod variable
                      .name = sc.t_label(stage_index, cname, "expmod", uid()),
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

    const bool capainst_success =
        capainst_cols.emplace(stage_index, capainst_col).second;
    const bool capacost_success =
        capacost_cols.emplace(stage_index, capacost_col).second;
    const bool capainst_row_success =
        capainst_rows
            .emplace(stage_index,
                     lp.add_row(std::move(capainst_row.equal(dcap))))
            .second;
    const bool capacost_row_success =
        capacost_rows
            .emplace(stage_index,
                     lp.add_row(std::move(capacost_row.equal(0.0))))
            .second;

    return capainst_success && capacost_success && capainst_row_success
        && capacost_row_success;
  }

  /**
   * @brief Add capacity solution data to output context
   * @tparam OutputContext Type of output context (deduced)
   * @param out Output context to populate
   * @param cname Class name prefix for labeling
   * @return true if successful, false otherwise
   *
   * @details Adds:
   * - Solution values for capacity variables
   * - Cost values for capacity variables
   * - Dual values for capacity constraints
   */
  template<typename OutputContext>
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

  /**
   * @brief Get the column index for capacity at a specific stage
   * @param stage_index The stage to get column index for
   * @return Optional containing column index if exists
   */
  [[nodiscard]] constexpr auto capacity_col_at(
      const StageIndex& stage_index) const noexcept
  {
    return get_optvalue(capainst_cols, stage_index);
  }

private:
  [[no_unique_address]] OptTRealSched capacity;
  [[no_unique_address]] OptTRealSched expcap;
  [[no_unique_address]] OptTRealSched capmax;
  [[no_unique_address]] OptTRealSched expmod;
  [[no_unique_address]] OptTRealSched annual_capcost;
  [[no_unique_address]] OptTRealSched annual_derating;

  [[no_unique_address]] TIndexHolder capainst_cols;
  [[no_unique_address]] TIndexHolder capacost_cols;
  [[no_unique_address]] TIndexHolder expmod_cols;
  [[no_unique_address]] TIndexHolder capainst_rows;
  [[no_unique_address]] TIndexHolder capacost_rows;
};

}  // namespace gtopt
