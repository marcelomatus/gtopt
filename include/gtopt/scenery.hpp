/**
 * @file      scenery.hpp
 * @brief     Header of
 * @date      Wed Mar 26 12:12:32 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/basic_types.hpp>

namespace gtopt
{

struct Scenery
{
  Uid uid {};
  OptName name {};
  OptBool active {};

  OptReal probability_factor {1};

  static constexpr std::string_view class_name = "scenery";
};

using SceneryUid = StrongUidType<struct Scenery>;
using SceneryIndex = StrongIndexType<Scenery>;

}  // namespace gtopt
