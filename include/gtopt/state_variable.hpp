/**
 * @file      state_variable.hpp
 * @brief     State variable representation for optimization problems
 * @author    marcelo
 * @date      Fri May  9 18:31:14 2025
 * @copyright BSD-3-Clause
 *
 * Defines the StateVariable class representing decision variables in
 * optimization models. Each variable is associated with specific phases and
 * stages in the power system model and tracks its position in the optimization
 * matrix.
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/stage.hpp>

namespace gtopt
{

class StateVariable
{
public:
  struct Key
  {
    ScenarioUid scenario_uid;
    StageUid stage_uid;
    Uid uid;
    std::string_view class_name;
    std::string_view col_name;

    auto operator<=>(const Key&) const = default;
  };

  [[nodiscard]]
  static auto key(std::string_view class_name,
                  Uid uid,
                  std::string_view col_name,
                  StageUid stage_uid = StageUid {unknown_uid},
                  ScenarioUid scenario_uid = ScenarioUid {unknown_uid}) -> Key
  {
    return {.scenario_uid = scenario_uid,
            .stage_uid = stage_uid,
            .uid = uid,
            .class_name = class_name,
            .col_name = col_name};
  }

  template<typename Element>
  [[nodiscard]]
  static auto key(const Element& element,
                  std::string_view col_name,
                  StageUid stage_uid = StageUid {unknown_uid},
                  ScenarioUid scenario_uid = ScenarioUid {unknown_uid}) -> Key
  {
    return key(
        element.class_name(), element.uid(), col_name, stage_uid, scenario_uid);
  }

  constexpr explicit StateVariable(SceneIndex scene_index,
                                   PhaseIndex phase_index,
                                   Index col) noexcept
      : m_scene_index_(scene_index)
      , m_phase_index_(phase_index)
      , m_col_(col)
  {
  }

  [[nodiscard]] constexpr Index col() const noexcept { return m_col_; }

  struct DependentVariable
  {
    SceneIndex scene_index {unknown_index};
    PhaseIndex phase_index {unknown_index};
    Index col {unknown_index};
  };

  using dependent_variable_t = DependentVariable;

  constexpr auto&& add_dependent_variable(SceneIndex scene_index,
                                          PhaseIndex phase_index,
                                          Index col) noexcept
  {
    return m_dependent_variables_.emplace_back(scene_index, phase_index, col);
  }

  template<typename ScenarioLP, typename StageLP>
  constexpr auto&& add_dependent_variable(const ScenarioLP& scenario,
                                          const StageLP& stage,
                                          Index col) noexcept
  {
    return add_dependent_variable(
        scenario.scene_index(), stage.phase_index(), col);
  }

  [[nodiscard]]
  constexpr const auto& dependent_variables() const
  {
    return m_dependent_variables_;
  }

  [[nodiscard]] constexpr SceneIndex scene_index() const noexcept
  {
    return m_scene_index_;
  }

  [[nodiscard]] constexpr PhaseIndex phase_index() const noexcept
  {
    return m_phase_index_;
  }

private:
  SceneIndex m_scene_index_ {unknown_index};
  PhaseIndex m_phase_index_ {unknown_index};
  Index m_col_ {unknown_index};

  std::vector<dependent_variable_t> m_dependent_variables_;
};

}  // namespace gtopt
