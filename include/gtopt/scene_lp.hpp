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

namespace gtopt
{

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
  using ScenarioSpan =
      std::span<const ScenarioLP>;  ///< Span of ScenarioLP elements
  using ScenarioIndexes =
      std::vector<ScenarioIndex>;  ///< Vector of scenario indices
  using ScenarioIndexSpan =
      std::span<const ScenarioIndex>;  ///< Span of scenario indices

  /** @brief Default constructor */
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
  template<class Scenarios>
  explicit SceneLP(Scene pscene, const Scenarios& pscenarios)
      : scene(std::move(pscene))
      , scenario_span(std::span(pscenarios)
                          .subspan(scene.first_scenario, scene.count_scenario))
  {
  }

  /**
   * @brief Check if the scene is active
   * @return true if the scene is active, false otherwise
   */
  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return scene.active.value_or(true);
  }

  /**
   * @brief Get the unique identifier of the scene
   * @return The scene's unique identifier
   */
  [[nodiscard]] constexpr auto uid() const { return SceneUid {scene.uid}; }

  /**
   * @brief Get all scenario elements associated with this scene
   * @return Span of ScenarioLP elements
   */
  [[nodiscard]] constexpr auto&& scenarios() const { return scenario_span; }

  [[nodiscard]] auto first_scenario() const
  {
    return ScenarioIndex {static_cast<Index>(scene.first_scenario)};
  }

  [[nodiscard]] auto count_scenario() const
  {
    return static_cast<Index>(scene.count_scenario);
  }

private:
  Scene scene;  ///< The underlying scene
  ScenarioSpan scenario_span;  ///< Span of ScenarioLP elements for this scene
};

}  // namespace gtopt
