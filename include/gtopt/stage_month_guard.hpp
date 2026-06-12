/**
 * @file      stage_month_guard.hpp
 * @brief     Fail-fast guards for calendar-month dependent LP assembly
 * @date      Sat Apr 11 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Several LP primitives (`VolumeRightLP` reset, monthly user-constraint
 * parameters, stage-month bound rules) are only meaningful when the
 * active stage carries a calendar month.  Historically a missing
 * `stage.month` would silently degrade: the `reset_month` comparison
 * short-circuited to false, monthly parameter lookups fell back to
 * January, and `stage_month` axis rules resolved to `0.0`.  These
 * silent fallbacks produce subtly wrong LPs that are nearly impossible
 * to diagnose from the output.
 *
 * The helpers in this header convert those silent drops into loud
 * `std::runtime_error`s that tell the caller exactly which element
 * needed the month and how to fix the simulation definition.
 */

#pragma once

#include <format>
#include <optional>
#include <stdexcept>
#include <string_view>

#include <gtopt/stage.hpp>
#include <gtopt/stage_lp.hpp>

namespace gtopt
{

/**
 * @brief Assert that a stage carries a calendar month, or throw.
 *
 * Used by LP primitives whose behaviour *depends on* `stage.month`
 * (reset months, monthly parameter lookups, `stage_month` bound-rule
 * axes).  A missing month is a schema bug in the caller's simulation
 * definition; we refuse to silently degrade.
 *
 * @param stage          The active stage.
 * @param consumer_kind  Short name of the element kind requesting the
 *                       month (e.g. `"VolumeRight"`, `"UserConstraint"`).
 * @param consumer_id    Identifier of the consuming element — usually
 *                       its uid or name — included in the error for
 *                       pinpointing.
 * @param feature        Short description of the feature that needs
 *                       the month (e.g. `"reset_month"`,
 *                       `"monthly parameter lookup"`).
 * @return               The resolved `MonthType`.
 * @throws std::runtime_error if `stage.month()` is `nullopt`.
 */
[[nodiscard]] inline auto require_stage_month(const StageLP& stage,
                                              std::string_view consumer_kind,
                                              std::string_view consumer_id,
                                              std::string_view feature)
    -> MonthType
{
  if (const auto m = stage.month(); m.has_value()) {
    return *m;
  }
  throw std::runtime_error(std::format(
      "{} '{}' requires stage.month to be set on stage uid={} for '{}', "
      "but the stage has no calendar month. "
      "Fix: add `.month = MonthType::<jan..dec>` to the Stage definition "
      "in the Simulation's stage_array (or remove the {} feature from "
      "element '{}').",
      consumer_kind,
      consumer_id,
      stage.uid(),
      feature,
      feature,
      consumer_id));
}

}  // namespace gtopt
