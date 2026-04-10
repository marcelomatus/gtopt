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
#include <gtopt/object_lp.hpp>

namespace gtopt
{
/**
 * @brief Base class providing capacity constraint logic for LP objects
 *
 * This class handles capacity constraints, expansion capabilities,
 * and associated costs in a linear programming formulation.
 */
struct CapacityObjectBase
{
  static constexpr std::string_view CapainstName {"capainst"};
  static constexpr std::string_view CapacostName {"capacost"};
  static constexpr std::string_view ExpmodName {"expmod"};

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
   * @param stage The stage to get column index for
   * @return Optional containing column index if exists
   */
  [[nodiscard]] constexpr auto capacity_col_at(
      const StageLP& stage) const noexcept
  {
    return get_optvalue(capainst_cols, stage.uid());
  }

  /**
   * @brief Get the capacity at a specific stage
   * @param stage The stage to query capacity for
   * @param def_capacity Default value if capacity not specified (default:
   * unlimited)
   * @return The capacity at given stage or default if not specified
   */

  [[nodiscard]] constexpr double capacity_at(
      const StageLP& stage,
      const double def_capacity = std::numeric_limits<double>::max()) const
  {
    return m_capacity_.at(stage.uid()).value_or(def_capacity);
  }

  /// Pair of optional capacity value and optional expansion column index.
  using CapacityAndCol =
      std::pair<std::optional<double>, std::optional<ColIndex>>;

  /**
   * @brief Query capacity value and optional expansion column for a stage.
   *
   * Returns `{std::optional<double>, std::optional<ColIndex>}`.
   * The capacity is `nullopt` when no expansion column exists AND no
   * capacity schedule value is defined — i.e. when the physical capacity
   * is truly undefined.  Callers that need a numeric fallback should use
   * `value_or(default)` on the returned optional.
   *
   * @param stage The stage to query
   * @param lp    Linear problem reference (needed for expansion col bounds)
   */
  [[nodiscard]] constexpr auto capacity_and_col(const StageLP& stage,
                                                LinearProblem& lp) const
      -> CapacityAndCol
  {
    if (auto col = capacity_col_at(stage)) {
      return {lp.get_col_uppb(*col), col};
    }
    return {m_capacity_.at(stage.uid()), {}};
  }

  /**
   * @brief Add capacity constraints to the linear problem
   * @param sc System context providing stage/scenario info
   * @param scenario Current scenario
   * @param stage Current stage
   * @param lp Linear problem to modify
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
   * @param out Output context to populate
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
        scenario, stage, self.m_class_name_, self.uid(), col_name);
  }

  template<typename Self, typename SystemContext, typename... Args>
  [[nodiscard]] constexpr auto state_col_label_p(this const Self& self,
                                                 SystemContext& sc,
                                                 const StageLP& stage,
                                                 Args&&... args)
  {
    return sc.state_col_label(
        stage, self.m_class_name_, std::forward<Args>(args)..., self.uid());
  }

  std::string_view m_class_name_ = "CapacityObject";

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

/**
 * @brief A linear programming representation of an object with capacity
 * constraints
 *
 * @tparam Object The type of object being modeled, must provide
 * capacity-related attributes
 */
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
   * @param pobject The object to wrap, will be moved if rvalue
   * @param ic Input context providing stage/scenario information
   * @param cname Class name for labeling columns/rows
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
