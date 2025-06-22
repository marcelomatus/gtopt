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
  struct LPKey
  {
    SceneIndex scene_index {unknown_index};
    PhaseIndex phase_index {unknown_index};

    auto operator<=>(const LPKey&) const = default;
  };

  struct Key
  {
    LPKey lp_key;

    ScenarioUid scenario_uid {unknown_uid};
    StageUid stage_uid {unknown_uid};

    Uid uid {unknown_uid};
    std::string_view class_name;
    std::string_view col_name;

    auto operator<=>(const Key&) const = default;
  };

  [[nodiscard]]
  static auto key(const std::string_view class_name,
                  const Uid uid,
                  const std::string_view col_name,
                  const PhaseIndex phase_index,
                  const StageUid stage_uid,
                  const SceneIndex scene_index = SceneIndex {unknown_index},
                  const ScenarioUid scenario_uid = ScenarioUid {unknown_uid})
      -> Key
  {
    return {.lp_key = {.scene_index = scene_index, .phase_index = phase_index},
            .scenario_uid = scenario_uid,
            .stage_uid = stage_uid,
            .uid = uid,
            .class_name = class_name,
            .col_name = col_name};
  }

  template<typename ScenarioLP, typename StageLP>
  [[nodiscard]]
  static auto key(const ScenarioLP& scenario,
                  const StageLP& stage,
                  const std::string_view class_name,
                  const Uid element_uid,
                  const std::string_view col_name) -> Key
  {
    return key(class_name,
               element_uid,
               col_name,
               stage.phase_index(),
               stage.uid(),
               scenario.scene_index(),
               scenario.uid());
  }

  template<typename StageLP>
  [[nodiscard]]
  static auto key(const StageLP& stage,
                  const std::string_view class_name,
                  const Uid element_uid,
                  const std::string_view col_name) -> Key
  {
    return key(
        class_name, element_uid, col_name, stage.phase_index(), stage.uid());
  }

  constexpr explicit StateVariable(const LPKey& lp_key, Index col) noexcept
      : m_lp_key_(lp_key)
      , m_col_(col)
  {
  }

  [[nodiscard]]
  constexpr Index col() const noexcept
  {
    return m_col_;
  }

  struct DependentVariable
  {
    LPKey lp_key;
    Index col {unknown_index};
  };

  constexpr auto&& add_dependent_variable(SceneIndex scene_index,
                                          PhaseIndex phase_index,
                                          Index col) noexcept
  {
    return m_dependent_variables_.emplace_back(
        LPKey {.scene_index = scene_index, .phase_index = phase_index}, col);
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

  [[nodiscard]]
  constexpr const auto& lp_key() const noexcept
  {
    return m_lp_key_;
  }

private:
  LPKey m_lp_key_;
  Index m_col_ {unknown_index};

  std::vector<DependentVariable> m_dependent_variables_;
};

}  // namespace gtopt
