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
#include <gtopt/object_utils.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/state_variable.hpp>

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

struct CapacityObjectBase
{
  [[nodiscard]] constexpr const Id& id() const noexcept { return m_id_; }

  template<typename OF>
  constexpr explicit CapacityObjectBase(const InputContext& ic,
                                        const LPClassName cname,
                                        Id pid,
                                        OF&& capacity,
                                        OF&& expcap,
                                        OF&& capmax,
                                        OF&& expmod,
                                        OF&& annual_capcost,
                                        OF&& annual_derating)
      : m_class_name_(cname.full_name())
      , m_short_name_(cname.short_name())
      , m_id_(std::move(pid))
      , m_capacity_(ic, cname.full_name(), id(), std::forward<OF>(capacity))
      , m_expcap_(ic, cname.full_name(), id(), std::forward<OF>(expcap))
      , m_capmax_(ic, cname.full_name(), id(), std::forward<OF>(capmax))
      , m_expmod_(ic, cname.full_name(), id(), std::forward<OF>(expmod))
      , m_annual_capcost_(
            ic, cname.full_name(), id(), std::forward<OF>(annual_capcost))
      , m_annual_derating_(
            ic, cname.full_name(), id(), std::forward<OF>(annual_derating))
  {
  }

  [[nodiscard]] constexpr auto uid() const noexcept
  {
    return std::get<0>(m_id_);
  }

  /**
   * @brief Get the column index for capacity at a specific stage
   * @param stage_index The stage to get column index for
   * @return Optional containing column index if exists
   */
  [[nodiscard]] constexpr auto capacity_col_at(
      const StageLP& stage) const noexcept
  {
    return get_optvalue(capainst_cols, stage.uid());
  }

  /**
   * @brief Get the capacity at a specific stage
   * @param stage_index The stage to query capacity for
   * @param def_capacity Default value if capacity not specified (default:
   * unlimited)
   * @return The capacity at given stage or default if not specified
   * @throws None This function is noexcept
   */

  [[nodiscard]] constexpr double capacity_at(
      const StageLP& stage,
      const double def_capacity = std::numeric_limits<double>::max()) const
  {
    return m_capacity_.at(stage.uid()).value_or(def_capacity);
  }

  /**
   * @brief Get capacity value and optional column index for a stage
   * @tparam Out Return type (defaults to pair<double, optional<ColIndex>>)
   * @param stage_index The stage to query
   * @param lp Linear problem reference to check column bounds
   * @return Pair containing:
   *   - First: Capacity value (upper bound if column exists, else schedule
   * value)
   *   - Second: Optional column index if exists
   */
  template<typename Out = std::pair<double, std::optional<ColIndex>>>
  [[nodiscard]] constexpr auto capacity_and_col(const StageLP& stage,
                                                LinearProblem& lp) const -> Out
  {
    auto&& capacity_col = capacity_col_at(stage);
    if (capacity_col.has_value()) {
      return {lp.get_col_uppb(capacity_col.value()), capacity_col};
    }

    return {capacity_at(stage), {}};
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
  bool add_to_lp(SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);

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
  bool add_to_output(OutputContext& out) const;

private:
  template<typename Self, typename ScenarioLP, typename StageLP>
  [[nodiscard]]
  constexpr auto sv_key_p(this const Self& self,
                          const ScenarioLP& scenario,
                          const StageLP& stage,
                          std::string_view col_name) noexcept
  {
    return StateVariable::key(
        scenario, stage, self.m_short_name_, self.uid(), col_name);
  }

  template<typename Self, typename SystemContext, typename... Args>
  [[nodiscard]] constexpr auto lp_label_p(this const Self& self,
                                          SystemContext& sc,
                                          const StageLP& stage,
                                          Args&&... args)
  {
    return sc.lp_label(
        stage, self.m_short_name_, std::forward<Args>(args)..., self.uid());
  }

public:
private:
  std::string_view m_class_name_ = "CapacityObject";
  std::string_view m_short_name_ = "cob";

  Id m_id_;
  OptTRealSched m_capacity_;
  OptTRealSched m_expcap_;
  OptTRealSched m_capmax_;
  OptTRealSched m_expmod_;
  OptTRealSched m_annual_capcost_;
  OptTRealSched m_annual_derating_;

  TIndexHolder<ColIndex> capainst_cols;
  TIndexHolder<ColIndex> capacost_cols;
  TIndexHolder<ColIndex> expmod_cols;
  TIndexHolder<RowIndex> capainst_rows;
  TIndexHolder<RowIndex> capacost_rows;
};

template<typename Object>
struct CapacityObjectLP
    : public ObjectLP<Object>
    , public CapacityObjectBase
{
  using Base = ObjectLP<Object>;
  using Base::id;
  using Base::is_active;
  using Base::object;
  using Base::uid;

  using CapacityObjectBase::add_to_lp;
  using CapacityObjectBase::add_to_output;
  using CapacityObjectBase::capacity_and_col;

  /**
   * @brief Construct a new CapacityObjectLP object
   * @tparam ObjectT Type of object being wrapped (deduced)
   * @param ic Input context providing stage/scenario information
   * @param class_name Name of the class for labeling columns/rows
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
  constexpr explicit CapacityObjectLP(ObjectT&& pobject,
                                      const InputContext& ic,
                                      const LPClassName cname)
      : ObjectLP<Object>(std::forward<ObjectT>(pobject), ic, cname)
      , CapacityObjectBase(ic,
                           cname,
                           id(),
                           std::move(object().capacity),
                           std::move(object().expcap),
                           std::move(object().capmax),
                           std::move(object().expmod),
                           std::move(object().annual_capcost),
                           std::move(object().annual_derating))
  {
  }
};

}  // namespace gtopt
