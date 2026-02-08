/**
 * @file      scene_lp.hpp
 * @brief     Header for SceneLP class that represents a logical scene with LP
 * scenario elements
 * @date      Wed Mar 26 12:10:25 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides a logical view of a Scene combined with its associated
 * ScenarioLP elements, enabling efficient access and management of scene
 * components for linear programming planning.
 */

#pragma once

#include <span>

#include <gtopt/basic_types.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/simulation.hpp>

namespace gtopt
{

namespace detail
{
[[nodiscard]] constexpr auto create_scenario_array(
    std::span<const Scenario> scenario_array,
    const Scene& scene,
    SceneIndex scene_index)
{
  auto scenarios =
      scenario_array.subspan(scene.first_scenario, scene.count_scenario);

  return enumerate_active<ScenarioIndex>(scenarios)
      | std::ranges::views::transform(
             [scene_index](const auto& is)
             {
               auto&& [scenario_index, scenario] = is;
               return ScenarioLP {scenario, scenario_index, scene_index};
             })
      | std::ranges::to<std::vector>();
}

}  // namespace detail

/**
 * @class SceneLP
 * @brief Represents a logical scene with linear programming scenario elements
 *
 * This class combines a Scene with its associated ScenarioLP elements,
 * providing a unified interface for scene management in linear programming
 * contexts.
 */
class SceneLP
{
public:
  SceneLP() = default;

  /**
   * @brief Construct a SceneLP from a Scene and a collection of ScenarioLP
   * elements
   *
   * @tparam Scenarios Container type for ScenarioLP elements
   * @param pscene The Scene object
   * @param pscenarios Collection of ScenarioLP elements
   *
   * Initializes the SceneLP with the given Scene and extracts the relevant
   * ScenarioLP elements based on the Scene's first_scenario and count_scenario.
   * Also initializes scenario indexes with sequential values.
   */
  explicit SceneLP(Scene scene,
                   std::span<const Scenario> scenarios,
                   SceneIndex index = SceneIndex {unknown_index})
      : m_scene_(std::move(scene))
      , m_scenarios_(detail::create_scenario_array(scenarios, m_scene_, index))
      , m_index_(index)
  {
  }

  explicit SceneLP(Scene scene,
                   const Simulation& simulation,
                   SceneIndex index = SceneIndex {unknown_index})
      : SceneLP(std::move(scene), simulation.scenario_array, index)
  {
  }

  /**
   * @brief Check if the scene is active
   * @return true if the scene is active, false otherwise
   */
  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return m_scene_.active.value_or(true);
  }

  /**
   * @brief Get the unique identifier of the scene
   * @return The scene's unique identifier
   */
  [[nodiscard]] constexpr auto uid() const noexcept { return SceneUid {m_scene_.uid}; }

  /**
   * @brief Get all scenario elements associated with this scene
   * @return Span of ScenarioLP elements
   */
  [[nodiscard]] constexpr auto scenarios() const
      -> const std::vector<ScenarioLP>&
  {
    return m_scenarios_;
  }

  [[nodiscard]] constexpr auto first_scenario() const noexcept
  {
    return ScenarioIndex {static_cast<Index>(m_scene_.first_scenario)};
  }

  [[nodiscard]] constexpr auto count_scenario() const noexcept
  {
    return static_cast<Index>(m_scene_.count_scenario);
  }

  /// @return Index of this scene in parent container
  [[nodiscard]] constexpr auto index() const noexcept { return m_index_; }

private:
  Scene m_scene_;  ///< The underlying scene
  std::vector<ScenarioLP> m_scenarios_;  ///< Span of ScenarioLP elements
  SceneIndex m_index_ {unknown_index};
};

}  // namespace gtopt
