/**
 * @file      scene_lp.hpp
 * @brief     Header for SceneLP class that represents a logical scene with LP
 * scenery elements
 * @date      Wed Mar 26 12:10:25 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides a logical view of a Scene combined with its associated
 * SceneryLP elements, enabling efficient access and management of scene
 * components for linear programming optimization.
 */

#pragma once

#include <functional>
#include <numeric>
#include <span>

#include <gtopt/basic_types.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/scenery.hpp>
#include <gtopt/scenery_lp.hpp>

namespace gtopt
{

/**
 * @class SceneLP
 * @brief Represents a logical scene with linear programming scenery elements
 *
 * This class combines a Scene with its associated SceneryLP elements,
 * providing a unified interface for scene management in linear programming
 * contexts.
 */
class SceneLP
{
public:
  using ScenerySpan =
      std::span<const SceneryLP>;  ///< Span of SceneryLP elements
  using SceneryIndexes =
      std::vector<SceneryIndex>;  ///< Vector of scenery indices
  using SceneryIndexSpan =
      std::span<const SceneryIndex>;  ///< Span of scenery indices

  /** @brief Default constructor */
  SceneLP() = default;

  /**
   * @brief Construct a SceneLP from a Scene and a collection of SceneryLP
   * elements
   *
   * @tparam Scenerys Container type for SceneryLP elements
   * @param pscene The Scene object
   * @param pscenerys Collection of SceneryLP elements
   *
   * Initializes the SceneLP with the given Scene and extracts the relevant
   * SceneryLP elements based on the Scene's first_scenery and count_scenery.
   * Also initializes scenery indexes with sequential values.
   */
  template<class Scenerys>
  explicit SceneLP(Scene pscene, const Scenerys& pscenerys)
      : scene(std::move(pscene))
      , scenery_span(std::span(pscenerys).subspan(scene.first_scenery,
                                                  scene.count_scenery))
      , scenery_indexes(scenery_span.size())
  {
    std::iota(  // NOLINT
        scenery_indexes.begin(),
        scenery_indexes.end(),
        SceneryIndex {});
  }

  /**
   * @brief Check if the scene is active
   * @return true if the scene is active, false otherwise
   */
  [[nodiscard]] constexpr auto is_active() const
  {
    return scene.active.value_or(true);
  }

  /**
   * @brief Get the unique identifier of the scene
   * @return The scene's unique identifier
   */
  [[nodiscard]] constexpr auto uid() const { return SceneUid {scene.uid}; }

  /**
   * @brief Get all scenery elements associated with this scene
   * @return Span of SceneryLP elements
   */
  [[nodiscard]] constexpr auto&& sceneries() const { return scenery_span; }

  /**
   * @brief Get the indexes of all scenery elements
   * @return Span of scenery indexes
   */
  [[nodiscard]] constexpr auto indexes() const
  {
    return SceneryIndexSpan {scenery_indexes};
  }

private:
  Scene scene;  ///< The underlying scene
  ScenerySpan scenery_span;  ///< Span of SceneryLP elements for this scene
  SceneryIndexes scenery_indexes;  ///< Indexed access to scenery elements
};

}  // namespace gtopt
