/**
 * @file      scene.hpp
 * @brief     Defines the Scene structure and related types for optimization scenarios
 * @date      Wed Mar 26 12:11:10 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Scene structure which represents a distinct scenario grouping
 * in a multi-scenario optimization problem. Each scene contains configuration for a
 * set of scenarios in the optimization.
 */

#pragma once

#include <span>

#include <gtopt/basic_types.hpp>

namespace gtopt
{

/**
 * @struct Scene
 * @brief Represents a scene (scenario grouping) in a multi-scenario optimization problem
 *
 * A scene groups together a set of scenarios in the optimization process with
 * common configuration. This allows modeling problems with distinct scenario sets
 * like different weather conditions, demand patterns, etc.
 */
struct Scene
{
  /// Unique identifier for the scene
  [[no_unique_address]] Uid uid {};

  /// Optional name for the scene (human-readable)
  [[no_unique_address]] OptName name {};

  /// Flag indicating if this scene is active in the optimization
  [[no_unique_address]] OptBool active {};

  /// Index of the first scenario belonging to this scene
  Size first_scenario {0};

  /**
   * @brief Number of scenarios in this scene
   * @note Uses std::dynamic_extent to indicate all remaining scenarios when unspecified
   */
  Size count_scenario {std::dynamic_extent};

  /// Class name constant used for serialization/deserialization
  static constexpr std::string_view class_name = "scene";
};

/// Strongly-typed unique identifier for Scene objects
using SceneUid = StrongUidType<Scene>;

/// Strongly-typed index for Scene objects in collections
using SceneIndex = StrongIndexType<Scene>;

/// Optional SceneIndex for cases where scene reference may be absent
using OptSceneIndex = std::optional<SceneIndex>;

}  // namespace gtopt
