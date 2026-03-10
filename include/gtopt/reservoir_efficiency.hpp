/**
 * @file      reservoir_efficiency.hpp
 * @brief     Piecewise-linear turbine efficiency as a function of reservoir
 *            volume (hydraulic head)
 * @date      Mon Mar 10 17:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Models the PLP "rendimiento" concept: each reservoir may provide a
 * piecewise-linear concave function that maps current reservoir volume
 * [dam³] to turbine conversion rate [MW·s/m³].
 *
 * ### Piecewise-linear evaluation
 * The efficiency is the **minimum** over all segments:
 * ```
 * efficiency(V) = min_i { constant_i + slope_i × (V − volume_i) }
 * ```
 * Slopes must be given in **decreasing** order so the function is concave.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "eff_colbun",
 *   "turbine": "COLBUN",
 *   "reservoir": "COLBUN",
 *   "mean_efficiency": 1.53,
 *   "segments": [
 *     { "volume": 0.0, "slope": 0.0002294, "constant": 1.2558 }
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
 * @brief One segment of the piecewise-linear efficiency curve
 *
 * Each segment contributes `constant + slope × (V − volume)` to the
 * concave envelope.  The overall efficiency at volume V is the minimum
 * over all segments.
 */
struct EfficiencySegment
{
  Real volume {0.0};  ///< Volume breakpoint [dam³]
  Real slope {0.0};  ///< Slope at this breakpoint [efficiency/dam³]
  Real constant {0.0};  ///< Intercept at this breakpoint [efficiency]
};

/**
 * @brief Reservoir-dependent turbine efficiency (PLP "rendimiento")
 *
 * Associates a turbine with a reservoir and provides a piecewise-linear
 * concave function mapping reservoir volume to turbine conversion rate.
 * When used with the SDDP solver, the conversion-rate LP coefficient is
 * updated at each forward-pass iteration based on the current reservoir
 * volume (the initial volume of the current phase).
 *
 * @see Turbine for the turbine whose conversion_rate is modulated
 * @see Reservoir for the reservoir whose volume drives the function
 * @see TurbineLP for the LP coefficient update mechanism
 */
struct ReservoirEfficiency
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  SingleId turbine {unknown_uid};  ///< ID of the related turbine
  SingleId reservoir {unknown_uid};  ///< ID of the related reservoir

  Real mean_efficiency {1.0};  ///< Fallback / average efficiency value

  std::vector<EfficiencySegment>
      segments {};  ///< Piecewise-linear segments (slopes in decreasing order)

  /// Per-element override for the number of SDDP iterations to skip between
  /// efficiency coefficient updates.  When not set, falls back to the global
  /// `sddp_options.sddp_efficiency_update_skip` option.
  OptInt sddp_efficiency_update_skip {};
};

/**
 * @brief Evaluate the piecewise-linear concave efficiency function
 *
 * Implements the PLP FRendimientos function:
 * ```
 * result = min over all segments of
 *          { constant_i + slope_i × (volume − volume_breakpoint_i) }
 * ```
 * Returns at least 0.0 (efficiency cannot be negative).
 *
 * @param segments The piecewise-linear segments
 * @param volume Current reservoir volume [dam³]
 * @return Efficiency value (non-negative)
 */
[[nodiscard]] inline auto evaluate_efficiency(
    const std::vector<EfficiencySegment>& segments, Real volume) noexcept
    -> Real
{
  if (segments.empty()) {
    return 1.0;
  }
  auto result = std::numeric_limits<Real>::max();
  for (const auto& seg : segments) {
    const auto val = seg.constant + seg.slope * (volume - seg.volume);
    result = std::min(result, val);
  }
  return std::max(result, 0.0);
}

}  // namespace gtopt
