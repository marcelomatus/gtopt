/**
 * @file      right_bound_rule.hpp
 * @brief     Volume-dependent piecewise-linear bound rule for water rights
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the RightBoundRule structure for dynamically computing
 * FlowRight/VolumeRight upper bounds based on a referenced reservoir's
 * current volume.  Used by FlowRightLP::update_lp and
 * VolumeRightLP::update_lp to implement PLP-style cushion zone logic
 * (Laja 4-zone, Maule 3-zone).
 *
 * The piecewise-linear function maps reservoir volume to a max bound:
 *   bound = constant_i + slope_i * V   (for active segment i)
 *
 * Segment format matches ReservoirSeepageSegment: each segment's
 * `constant` is the y-intercept at V=0, with continuity at breakpoints.
 *
 * ### Laja cushion zone example
 *
 * ```text
 * Rights_irr = 570 + 0.00*min(V,1200) + 0.40*min(max(V-1200,0),700)
 *            + 0.25*max(V-1900,0)
 * ```
 *
 * maps to segments:
 *
 * ```json
 * [
 *   {"volume": 0,    "slope": 0.00, "constant": 570},
 *   {"volume": 1200, "slope": 0.40, "constant": 90},
 *   {"volume": 1900, "slope": 0.25, "constant": 375}
 * ]
 * ```
 *
 * with `cap = 5000`.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/stage_enums.hpp>

namespace gtopt
{

/**
 * @brief The driving axis for a `RightBoundRule` piecewise function.
 *
 * - `reservoir_volume` (default): the rule input is a referenced
 *   reservoir's current volume in `hm³`.  This preserves the
 *   Maule/Laja cushion-zone behaviour and is the only axis required by
 *   existing test fixtures.
 * - `stage_month`: the rule input is the calendar month of the active
 *   stage (1=Jan..12=Dec).  Allows a single FlowRight/VolumeRight to
 *   carry a 12-segment monthly modulation table directly, without
 *   precomputing per-stage schedules.  Future: per-block axes such as
 *   `demand_minus_inflow` for La Invernada conditional rules.
 */
enum class BoundRuleAxis : uint8_t
{
  reservoir_volume = 0,
  stage_month = 1,
};

inline constexpr auto bound_rule_axis_entries =
    std::to_array<EnumEntry<BoundRuleAxis>>({
        {.name = "reservoir_volume", .value = BoundRuleAxis::reservoir_volume},
        {.name = "stage_month", .value = BoundRuleAxis::stage_month},
    });

constexpr auto enum_entries(BoundRuleAxis /*tag*/) noexcept
{
  return std::span {bound_rule_axis_entries};
}

/**
 * @brief One segment of a piecewise-linear bound function
 *
 * Each segment defines a linear function `bound = constant + slope * V`
 * that is active when the reservoir volume is in range
 * `[volume, next_segment.volume)`.  `constant` is the **y-intercept**
 * at V = 0 (same convention as ReservoirSeepageSegment).
 *
 * For a continuous piecewise function the constants are chosen so that
 * adjacent segments agree at the breakpoint:
 * `constant_i = constant_{i-1} + slope_{i-1} * volume_i - slope_i * volume_i`
 */
struct RightBoundSegment
{
  Real volume {
      0.0,
  };  ///< Volume breakpoint [hm3] -- segment active when V >= volume
  Real slope {0.0};  ///< d(bound)/d(V)
  Real constant {
      0.0,
  };  ///< Y-intercept: bound at V=0 for this segment's linear equation
};

/**
 * @brief Volume-dependent bound rule for water rights
 *
 * Defines a piecewise-linear function from a reservoir's volume to
 * the maximum allowed flow/extraction bound.  When attached to a
 * FlowRight or VolumeRight, the LP column upper bound is dynamically
 * updated via `update_lp` using this rule.
 */
struct RightBoundRule
{
  /// Reference reservoir whose volume drives the bound computation.
  /// Required when `axis == BoundRuleAxis::reservoir_volume` (the
  /// default).  Ignored when the axis does not consume reservoir state.
  SingleId reservoir {unknown_uid};

  /// Piecewise-linear segments (sorted ascending by the axis input).
  /// The `volume` field is the segment's lower breakpoint regardless
  /// of which axis is in use — it is the input value at which the
  /// segment becomes active, not necessarily a reservoir volume.
  /// When empty, the bound rule has no effect.
  std::vector<RightBoundSegment> segments {};

  /// Maximum cap on the computed bound (e.g., 5000 m3/s for Laja
  /// irrigation rights).  When unset, no upper cap is applied.
  OptReal cap {};

  /// Minimum floor on the computed bound.  When unset, defaults to 0.
  OptReal floor {};

  /// Axis driving the rule.  Defaults to `reservoir_volume` so that
  /// existing fixtures (Laja, Maule) continue to use the source
  /// reservoir's volume without explicit opt-in.
  BoundRuleAxis axis {BoundRuleAxis::reservoir_volume};
};

// -- Piecewise-linear evaluation ----------------------------------------

/**
 * @brief Find the active segment for a given reservoir volume
 *
 * Returns a pointer to the last segment whose volume breakpoint
 * is <= `volume` (range selection).  Returns the first segment when
 * `volume` is below all breakpoints.
 *
 * @pre `segments` must be non-empty and sorted ascending by volume.
 */
[[nodiscard]] inline auto find_active_bound_segment(
    const std::vector<RightBoundSegment>& segments, Real volume) noexcept
    -> const RightBoundSegment*
{
  const RightBoundSegment* active = &segments.front();
  for (const auto& seg : segments) {
    if (seg.volume <= volume) {
      active = &seg;
    } else {
      break;
    }
  }
  return active;
}

/**
 * @brief Evaluate the piecewise-linear bound function
 *
 * Computes: `result = constant_i + slope_i * volume` for the active
 * segment, then clamps to [floor, cap].
 *
 * @param rule The bound rule with segments, cap, and floor
 * @param volume Current reservoir volume [hm3]
 * @return The computed bound value (clamped)
 */
[[nodiscard]] inline auto evaluate_bound_rule(const RightBoundRule& rule,
                                              Real volume) noexcept -> Real
{
  if (rule.segments.empty()) {
    return rule.cap.value_or(0.0);
  }

  const auto* active = find_active_bound_segment(rule.segments, volume);
  auto result = active->constant + (active->slope * volume);

  const auto floor_val = rule.floor.value_or(0.0);
  result = std::max(result, floor_val);

  if (rule.cap.has_value()) {
    result = std::min(result, *rule.cap);
  }

  return result;
}

/**
 * @brief Resolve the rule's input value by axis dispatch.
 *
 * Each axis is sourced differently and the call sites in
 * `FlowRightLP` / `VolumeRightLP` should not have to know about the
 * dispatch table.  The reservoir-volume axis pulls from a caller-supplied
 * callable so this header stays free of `SystemContext` coupling.
 *
 * @param rule              The configured rule.
 * @param stage_month       Active stage's calendar month, when set.
 * @param volume_getter     Callable returning the reservoir volume in
 *                          `hm³`.  Invoked only when the axis is
 *                          `reservoir_volume`, so the caller can defer
 *                          any expensive lookup work behind it.
 * @return The numeric input to feed into `evaluate_bound_rule`.  When
 *         the active axis has no value (e.g. `stage_month` on a stage
 *         without a month assignment) returns `0.0`, mirroring the
 *         `value_or(0.0)` fallbacks already used elsewhere.
 */
template<typename VolumeGetter>
[[nodiscard]] inline auto resolve_bound_rule_axis_value(
    const RightBoundRule& rule,
    const std::optional<MonthType>& stage_month,
    VolumeGetter&& volume_getter) -> Real
{
  switch (rule.axis) {
    case BoundRuleAxis::stage_month:
      return stage_month.has_value()
          ? static_cast<Real>(std::to_underlying(*stage_month))
          : Real {0.0};
    case BoundRuleAxis::reservoir_volume:
      return std::forward<VolumeGetter>(volume_getter)();
  }
  return Real {0.0};
}

/**
 * @brief Whether this axis depends on a reservoir reference.
 *
 * Call sites use this to skip the reservoir element lookup entirely
 * when the configured axis does not consume reservoir state, so the
 * `reservoir` field can be left unset on stage-month rules.
 */
[[nodiscard]] inline constexpr auto axis_uses_reservoir(
    BoundRuleAxis axis) noexcept -> bool
{
  return axis == BoundRuleAxis::reservoir_volume;
}

}  // namespace gtopt
