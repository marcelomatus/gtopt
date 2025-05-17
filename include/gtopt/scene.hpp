/**
 * @file      scene.hpp
 * @brief     Header of
 * @date      Wed Mar 26 12:11:10 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <span>

#include <gtopt/basic_types.hpp>

namespace gtopt
{

struct Scene
{
  Uid uid {};
  OptName name {};
  OptBool active {};

  Size first_scenario {0};
  Size count_scenario {std::dynamic_extent};

  static constexpr std::string_view class_name = "scene";
};

using SceneUid = StrongUidType<struct Scene>;
using SceneIndex = StrongIndexType<Scene>;
using OptSceneIndex = std::optional<SceneIndex>;

}  // namespace gtopt
