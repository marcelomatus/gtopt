/**
 * @file      object_utils.hpp
 * @brief     Utility methods for objects in the optimization framework
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides common utility methods for objects, such as generating state
 * variable keys and labels.
 */

#pragma once

#include <gtopt/label_maker.hpp>
#include <gtopt/state_variable.hpp>

namespace gtopt
{

class SystemContext;
class StageLP;
class ScenarioLP;
class BlockLP;

/**
 * @class ObjectUtils
 * @brief Provides common utility methods for objects in the optimization
 * framework
 */
class ObjectUtils
{
public:
  /**
   * @brief Generates a state variable key for the object
   * @tparam Self CRTP self type
   * @param self Reference to the object
   * @param col_name The column name for the state variable
   * @param stage_uid Stage UID (default unknown)
   * @param scenario_uid Scenario UID (default unknown)
   * @return StateVariable::Key
   */
  template<typename Self, typename ScenarioLP, typename StageLP>
  [[nodiscard]]
  constexpr auto sv_key(this const Self& self,
                        const ScenarioLP& scenario,
                        const StageLP& stage,
                        std::string_view col_name) noexcept
  {
    return StateVariable::key(
        scenario, stage, self.short_name(), self.uid(), col_name);
  }

  template<typename Self, typename StageLP>
  [[nodiscard]]
  constexpr auto sv_key(this const Self& self,
                        const StageLP& stage,
                        std::string_view col_name) noexcept
  {
    return StateVariable::key(stage, self.short_name(), self.uid(), col_name);
  }

  /**
   * @brief Generates a label for a variable in the optimization problem
   * @tparam Self CRTP self type
   * @tparam SystemContext Type of the system context
   * @tparam StageLP Type of the stage
   * @tparam Args Types of additional arguments
   * @param self Reference to the object
   * @param sc System context
   * @param stage Stage
   * @param args Additional arguments to include in the label
   * @return Label string
   */
  template<typename Self, typename SystemContext, typename... Args>
  [[nodiscard]] constexpr auto lp_label(this const Self& self,
                                        SystemContext& sc,
                                        const StageLP& stage,
                                        Args&&... args)
  {
    return sc.lp_label(
        stage, self.short_name(), std::forward<Args>(args)..., self.uid());
  }

  template<typename Self, typename SystemContext, typename... Args>
  [[nodiscard]] constexpr auto lp_label(this const Self& self,
                                        SystemContext& sc,
                                        const ScenarioLP& scenario,
                                        const StageLP& stage,
                                        Args&&... args)
  {
    return sc.lp_label(scenario,
                       stage,
                       self.short_name(),
                       std::forward<Args>(args)...,
                       self.uid());
  }

  template<typename Self, typename SystemContext, typename... Args>
  [[nodiscard]] constexpr auto lp_label(this const Self& self,
                                        SystemContext& sc,
                                        const ScenarioLP& scenario,
                                        const StageLP& stage,
                                        const BlockLP& block,
                                        Args&&... args)
  {
    return sc.lp_label(scenario,
                       stage,
                       block,
                       self.short_name(),
                       std::forward<Args>(args)...,
                       self.uid());
  }
};

}  // namespace gtopt
