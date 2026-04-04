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
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

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
  SingleId reservoir {unknown_uid};

  /// Piecewise-linear segments (sorted ascending by volume breakpoint).
  /// When empty, the bound rule has no effect.
  std::vector<RightBoundSegment> segments {};

  /// Maximum cap on the computed bound (e.g., 5000 m3/s for Laja
  /// irrigation rights).  When unset, no upper cap is applied.
  OptReal cap {};

  /// Minimum floor on the computed bound.  When unset, defaults to 0.
  OptReal floor {};
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

}  // namespace gtopt
