/**
 * @file      object_utils.hpp
 * @brief     Utility methods for objects in the optimization framework
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides common utility methods for objects, such as generating state variable keys and labels.
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/state_variable.hpp>
#include <gtopt/label_maker.hpp>

namespace gtopt
{

class SystemContext;
class StageLP;

/**
 * @class ObjectUtils
 * @brief Provides common utility methods for objects in the optimization framework
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
  template<typename Self>
  [[nodiscard]]
  constexpr auto sv_key(this const Self& self,
                        std::string_view col_name,
                        StageUid stage_uid = StageUid {unknown_uid},
                        ScenarioUid scenario_uid = ScenarioUid {unknown_uid}) const noexcept
  {
    return StateVariable::key(self, col_name, stage_uid, scenario_uid);
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
  template<typename Self,
           typename SystemContext,
           typename StageLP,
           typename... Args>
  [[nodiscard]] constexpr auto t_label(this const Self& self,
                                       SystemContext& sc,
                                       const StageLP& stage,
                                       Args&&... args) const noexcept
  {
    return sc.t_label(
        stage, self.class_name(), std::forward<Args>(args)..., self.uid());
  }
};

}  // namespace gtopt
