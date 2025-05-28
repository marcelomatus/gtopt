#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/stage_lp.hpp>

namespace gtopt
{

class ScenarioLP
{
public:
  ScenarioLP() = default;

  template<class StageLPs = std::vector<StageLP> >
  explicit ScenarioLP(Scenario scenario,
                      ScenarioIndex index = ScenarioIndex {unknown_index},
                      SceneIndex scene_index = SceneIndex {unknown_index})
      : m_scenario_(std::move(scenario))
      , m_index_(index)
      , m_scene_index_(scene_index)
  {
  }

  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return m_scenario_.is_active();
  }

  [[nodiscard]] constexpr auto is_first() const noexcept
  {
    return m_index_ == ScenarioIndex {0};
  }

  [[nodiscard]] constexpr auto uid() const
  {
    return ScenarioUid(m_scenario_.uid);
  }

  [[nodiscard]] constexpr auto probability_factor() const
  {
    return m_scenario_.probability_factor.value_or(1.0);
  }

  [[nodiscard]] constexpr auto index() const noexcept { return m_index_; }
  [[nodiscard]] constexpr auto scene_index() const noexcept
  {
    return m_scene_index_;
  }

private:
  Scenario m_scenario_;
  ScenarioIndex m_index_ {unknown_index};
  SceneIndex m_scene_index_ {unknown_index};
};

}  // namespace gtopt
