/**
 * @file      scene.hpp
 * @brief     Scene data structure for grouping scenarios
 * @date      Wed Mar 26 12:11:10 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Scene struct, which groups a contiguous range
 * of scenarios within a simulation.
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/uid.hpp>

namespace gtopt
{

struct Scene
{
  Uid uid {unknown_uid};
  OptName name;
  OptBool active;

  Size first_scenario {0};
  Size count_scenario {std::dynamic_extent};

  static constexpr std::string_view class_name = "scene";

  [[nodiscard]] constexpr auto is_active() const noexcept
  {
    return active.value_or(true);
  }
};

using SceneUid = UidOf<Scene>;
using SceneIndex = StrongPositionIndexType<Scene>;
using OptSceneIndex = std::optional<SceneIndex>;

/// @brief First scene index — the canonical root scene used when
/// boundary cuts and column-name maps are built scene-independently.
[[nodiscard]] constexpr auto first_scene_index() noexcept -> SceneIndex
{
  return SceneIndex {0};
}

/// @brief Next scene index (scene_index + 1), preserving strong type.
[[nodiscard]] constexpr auto next(SceneIndex scene_index) noexcept -> SceneIndex
{
  return ++scene_index;
}

/// @brief Previous scene index (scene_index - 1), preserving strong type.
[[nodiscard]] constexpr auto previous(SceneIndex scene_index) noexcept
    -> SceneIndex
{
  return --scene_index;
}

}  // namespace gtopt
