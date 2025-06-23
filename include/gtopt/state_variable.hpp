/**
 * @file      state_variable.hpp
 * @brief     State variables and their dependencies for linear programming
 * problems
 * @date      Mon Jun 23 11:56:29 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the StateVariable class which represents variables in a
 * linear programming problem that may have dependencies across different scenes
 * and phases of the optimization.
 *
 *
 */

#pragma once

#include <span>
#include <string_view>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/stage.hpp>

namespace gtopt
{
struct LPKey
{
  SceneIndex scene_index {unknown_index};
  PhaseIndex phase_index {unknown_index};

  constexpr auto operator<=>(const LPKey&) const noexcept = default;
};

struct LPVariable
{
  constexpr explicit LPVariable(LPKey lp_key, Index col) noexcept
      : m_lp_key_ {lp_key}
      , m_col_ {col}
  {
  }

  [[nodiscard]]
  constexpr Index col() const noexcept
  {
    return m_col_;
  }

  [[nodiscard]]
  constexpr const LPKey& lp_key() const noexcept
  {
    return m_lp_key_;
  }

  [[nodiscard]]
  constexpr SceneIndex scene_index() const noexcept
  {
    return m_lp_key_.scene_index;
  }

  [[nodiscard]]
  constexpr PhaseIndex phase_index() const noexcept
  {
    return m_lp_key_.phase_index;
  }

private:
  LPKey m_lp_key_;
  Index m_col_ {unknown_index};
};

class StateVariable : public LPVariable
{
public:
  using LPKey = gtopt::LPKey;

  struct Key
  {
    ScenarioUid scenario_uid {unknown_uid};
    StageUid stage_uid {unknown_uid};
    Uid uid {unknown_uid};
    std::string_view col_name;
    std::string_view class_name;
    LPKey lp_key;

    constexpr auto operator<=>(const Key&) const noexcept = default;
  };

  [[nodiscard]] static constexpr auto key(
      std::string_view class_name,
      Uid uid,
      std::string_view col_name,
      PhaseIndex phase_index,
      StageUid stage_uid,
      SceneIndex scene_index = SceneIndex {unknown_index},
      ScenarioUid scenario_uid = ScenarioUid {unknown_uid}) noexcept -> Key
  {
    return {.scenario_uid = scenario_uid,
            .stage_uid = stage_uid,
            .uid = uid,
            .col_name = col_name,
            .class_name = class_name,
            .lp_key = {.scene_index = scene_index, .phase_index = phase_index}};
  }

  template<typename ScenarioLP, typename StageLP>
  [[nodiscard]]
  static constexpr auto key(const ScenarioLP& scenario,
                            const StageLP& stage,
                            std::string_view class_name,
                            Uid element_uid,
                            std::string_view col_name) noexcept -> Key
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
  static constexpr auto key(const StageLP& stage,
                            std::string_view class_name,
                            Uid element_uid,
                            std::string_view col_name) noexcept -> Key
  {
    return key(
        class_name, element_uid, col_name, stage.phase_index(), stage.uid());
  }

  constexpr explicit StateVariable(LPKey lp_key, Index col) noexcept
      : LPVariable(lp_key, col)
  {
  }

  using DependentVariable = LPVariable;

  [[nodiscard]]
  constexpr std::span<const DependentVariable> dependent_variables()
      const noexcept
  {
    return m_dependent_variables_;
  }

  constexpr auto add_dependent_variable(LPKey lp_key, Index col) noexcept
      -> const DependentVariable&
  {
    return m_dependent_variables_.emplace_back(lp_key, col);
  }

  template<typename ScenarioLP, typename StageLP>
  constexpr auto add_dependent_variable(const ScenarioLP& scenario,
                                        const StageLP& stage,
                                        Index col) noexcept
      -> const DependentVariable&
  {
    return add_dependent_variable(LPKey {.scene_index = scenario.scene_index(),
                                         .phase_index = stage.phase_index()},
                                  col);
  }

private:
  std::vector<DependentVariable> m_dependent_variables_;
};

}  // namespace gtopt
