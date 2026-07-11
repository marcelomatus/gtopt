// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      planning_options_lp.cpp
 * @brief     Out-of-line PlanningOptionsLP members.
 *
 * Holds the few PlanningOptionsLP methods whose bodies pull in
 * runtime-only dependencies (here `<cstdlib>` for the environment read),
 * keeping them out of the otherwise header-only, mostly-constexpr
 * `planning_options_lp.hpp`.
 */

#include <cstdlib>

#include <gtopt/enum_option.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/sddp_enums.hpp>

namespace gtopt
{

auto PlanningOptionsLP::sddp_low_memory() const -> LowMemoryMode
{
  // Environment override GTOPT_MEMORY_MODE (see the header docstring):
  // `normal|compress` (with aliases), precedence over the per-run option;
  // an unset or unrecognised value falls back to the option / default.
  if (const char* const v = std::getenv("GTOPT_MEMORY_MODE"); v != nullptr) {
    if (const auto forced = enum_from_name<LowMemoryMode>(v)) {
      return *forced;
    }
  }
  return m_options_.sddp_options.low_memory_mode.value_or(
      LowMemoryMode::compress);
}

}  // namespace gtopt
