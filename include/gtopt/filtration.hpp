/**
 * @file      filtration.hpp
 * @brief     Piecewise-linear water filtration (seepage) from a reservoir
 * @date      Thu Jul 31 23:22:44 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the Filtration structure modeling water seepage from a waterway
 * into an adjacent reservoir.  The seepage flow is modelled as a
 * piecewise-linear function of the reservoir's average volume:
 *
 * ```
 * seepage [m³/s] = slope × avg_volume [dam³] + constant [m³/s]
 * ```
 *
 * When `segments` is non-empty, the active (slope, constant) pair is
 * selected from the piecewise-linear concave envelope evaluated at the
 * current reservoir volume (see `select_filtration_coeffs()`).
 *
 * ### Piecewise-linear evaluation
 * ```
 * filtration(V) = min_i { constant_i + slope_i × (V − volume_i) }
 * ```
 * This is equivalent to the PLP "filtraciones" model (plpcenfi.dat).
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "filt1",
 *   "waterway": "w1_2",
 *   "reservoir": "r1",
 *   "slope": 0.02,
 *   "constant": 0.5,
 *   "segments": [
 *     { "volume": 0.0, "slope": 0.0003, "constant": 0.5 },
 *     { "volume": 500.0, "slope": 0.0001, "constant": 0.65 }
 *   ]
 * }
 * ```
 */

#pragma once

#include <algorithm>
#include <limits>
#include <vector>

#include <gtopt/object.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

/**
 * @brief One segment of the piecewise-linear filtration curve
 *
 * Each segment contributes `constant + slope × (V − volume)` to the
 * concave envelope.  The overall filtration rate at volume V is the
 * minimum over all segments (analogous to ReservoirEfficiency segments).
 */
struct FiltrationSegment
{
  Real volume {0.0};  ///< Volume breakpoint [dam³]
  Real slope {0.0};  ///< Seepage slope at this breakpoint [m³/s / dam³]
  Real constant {0.0};  ///< Seepage rate at this breakpoint [m³/s]
};

/**
 * @brief Piecewise-linear seepage model between a waterway and a reservoir
 *
 * Models water seepage from a waterway into an adjacent reservoir.
 * The filtration flow per block is:
 *   `filtration_flow = slope × V_avg + intercept`
 * where `V_avg = (eini + efin) / 2` is the average reservoir volume.
 *
 * When `segments` is empty, the static `slope` and `constant` fields are
 * used directly (backward compatible with the single-pair plpcenfi.dat
 * format).  When `segments` is non-empty, the active segment is selected
 * based on the reservoir volume and the LP coefficients are updated
 * dynamically (via FiltrationLP::update_lp).
 *
 * @see Waterway for the source channel
 * @see Reservoir for the receiving storage
 * @see FiltrationLP for the LP formulation
 */
struct Filtration
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Operational status (default: active)

  SingleId waterway {unknown_uid};  ///< ID of the source waterway
  SingleId reservoir {unknown_uid};  ///< ID of the receiving reservoir
  Real slope {0.0};  ///< Default seepage slope [m³/s / dam³] (used when
                     ///< segments is empty)
  Real constant {0.0};  ///< Default constant seepage rate [m³/s] (used when
                        ///< segments is empty)

  /// Piecewise-linear segments for volume-dependent filtration rate.
  /// When non-empty, overrides the static slope/constant fields.
  /// Slopes should be in decreasing order (concave envelope).
  std::vector<FiltrationSegment> segments {};
};

// ─── Piecewise-linear evaluation ────────────────────────────────────────────

/**
 * @brief Evaluate the piecewise-linear concave filtration function
 *
 * Implements the PLP filtration model:
 * ```
 * result = min over all segments of
 *          { constant_i + slope_i × (volume − volume_breakpoint_i) }
 * ```
 * Returns at least 0.0 (filtration rate cannot be negative).
 *
 * @param segments The piecewise-linear segments
 * @param volume Current reservoir volume [dam³]
 * @return Filtration rate (non-negative) [m³/s]
 */
[[nodiscard]] inline auto evaluate_filtration(
    const std::vector<FiltrationSegment>& segments, Real volume) noexcept
    -> Real
{
  if (segments.empty()) {
    return 0.0;
  }
  auto result = std::numeric_limits<Real>::max();
  for (const auto& seg : segments) {
    const auto val = seg.constant + (seg.slope * (volume - seg.volume));
    result = std::min(result, val);
  }
  return std::max(result, 0.0);
}

/**
 * @brief LP constraint coefficients for volume-dependent filtration
 *
 * The filtration LP constraint is:
 *   `filtration_flow = slope × V_avg + intercept`
 * where V_avg = (eini + efin) / 2.
 *
 * In matrix form: `filt - slope*0.5*eini - slope*0.5*efin = intercept`
 */
struct FiltrationCoeffs
{
  Real slope {0.0};  ///< Coefficient on V_avg (b in filt = a + b*V)
  Real intercept {0.0};  ///< RHS constant (a in filt = a + b*V)
};

/**
 * @brief Select the active filtration segment and return LP coefficients
 *
 * Given the piecewise-linear concave envelope, selects the segment that
 * achieves the minimum at the given volume and returns the (slope,
 * intercept) pair for the LP constraint.
 *
 * The LP constraint becomes:
 *   `filt_flow - slope*0.5*eini - slope*0.5*efin = intercept`
 *
 * @param segments The piecewise-linear segments
 * @param volume Current reservoir volume [dam³]
 * @return FiltrationCoeffs with slope and intercept for the active segment
 */
[[nodiscard]] inline auto select_filtration_coeffs(
    const std::vector<FiltrationSegment>& segments, Real volume) noexcept
    -> FiltrationCoeffs
{
  if (segments.empty()) {
    return {};
  }

  const FiltrationSegment* best_seg = segments.data();
  auto best_val = std::numeric_limits<Real>::max();

  for (const auto& seg : segments) {
    const auto val = seg.constant + (seg.slope * (volume - seg.volume));
    if (val < best_val) {
      best_val = val;
      best_seg = &seg;
    }
  }

  return {
      .slope = best_seg->slope,
      .intercept = best_seg->constant - (best_seg->slope * best_seg->volume),
  };
}

}  // namespace gtopt
