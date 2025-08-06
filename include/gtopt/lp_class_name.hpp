/**
 * @file      lp_class_name.hpp
 * @brief     Defines the LPClassName struct
 * @date      Wed Aug 06 15:10:00 2025
 * @author    ai-developer
 * @copyright BSD-3-Clause
 *
 * This header defines the LPClassName struct used to hold both the full and
 * short names for LP object classes.
 */
#pragma once

#include <string_view>

namespace gtopt
{
struct LPClassName
{
  std::string_view name;
  std::string_view short_name;
};
}  // namespace gtopt
