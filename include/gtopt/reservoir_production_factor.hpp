/**
 * @file      reservoir_production_factor.hpp
 * @brief     Piecewise-linear turbine efficiency as a function of reservoir
 *            volume (hydraulic head)
 * @date      Mon Mar 10 17:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Models the PLP "rendimiento" concept: each reservoir may provide a
 * piecewise-linear concave function that maps current reservoir volume
 * [hm³] to turbine conversion rate [MW·s/m³].
 *
 * ### Piecewise-linear evaluation (matches PLP Fortran `FRendimientos`)
 *
 * The efficiency is the **minimum** over all segments (concave envelope):
 * ```
 * efficiency(V) = min_i { constant_i + slope_i × (V − volume_i) }
 * ```
 * Here `constant_i` is the efficiency **at the breakpoint** `volume_i`
 * (point-slope form).  This matches the PLP Fortran function
 * `FRendimientos` in `plp-frendim.f`:
 * ```fortran
 * ValFRendimientos = MIN(ValFRendimientos,
 *     Constantes(i) + Pendientes(i) * (Vol - Bordes(i)))
 * ```
 *
 * **Note**: This differs from `ReservoirSeepageSegment` where `constant` is
 * the y-intercept at V = 0.  For efficiency, `constant` is the value
 * **at** the breakpoint (point-slope form), not the y-intercept.
 *
 * Slopes must be given in **decreasing** order so the function is concave.
 *
 * ### Volume used for SDDP updates
 *
 * During SDDP iterations, the conversion-rate LP coefficient is updated
 * using the **average volume** `vavg = (vini + vfin) / 2` from the
 * previous LP solve, providing a better linearization point than using
 * only the initial volume.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "eff_colbun",
 *   "turbine": "COLBUN",
 *   "reservoir": "COLBUN",
 *   "mean_production_factor": 1.53,
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
 * over all segments (matching PLP Fortran `FRendimientos`).
 *
 * **Note**: Unlike `ReservoirSeepageSegment` where `constant` is the
 * y-intercept at V = 0, here `constant` is the efficiency value
 * **at the breakpoint** `volume` (point-slope form).
 */
struct ProductionFactorSegment
{
  Real volume {0.0};  ///< Volume breakpoint [hm³] (Fortran `Bordes`)
  Real slope {
      0.0,
  };  ///< Slope at this breakpoint [efficiency per hm³] (`Pendientes`)
  Real constant {0.0};  ///< Efficiency at breakpoint [MW·s/m³] (`Constantes`)
};

/**
 * @brief Reservoir-dependent turbine efficiency (PLP "rendimiento")
 *
 * Associates a turbine with a reservoir and provides a piecewise-linear
 * concave function mapping reservoir volume to turbine conversion rate.
 * When used with the SDDP solver, the conversion-rate LP coefficient is
 * updated at each forward-pass iteration based on the average reservoir
 * volume `vavg = (vini + vfin) / 2` from the previous LP solve.
 *
 * @see Turbine for the turbine whose conversion_rate is modulated
 * @see Reservoir for the reservoir whose volume drives the function
 * @see TurbineLP for the LP coefficient update mechanism
 */
struct ReservoirProductionFactor
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  SingleId turbine {unknown_uid};  ///< ID of the related turbine
  SingleId reservoir {unknown_uid};  ///< ID of the related reservoir

  Real mean_production_factor {1.0};  ///< Fallback / average efficiency value

  std::vector<ProductionFactorSegment>
      segments {};  ///< Piecewise-linear segments (slopes in decreasing order)
};

/**
 * @brief Evaluate the piecewise-linear concave efficiency function
 *
 * Implements the PLP `FRendimientos` function (plp-frendim.f):
 * ```
 * result = min over all segments of
 *          { constant_i + slope_i × (volume − volume_breakpoint_i) }
 * ```
 * Here `constant_i` is the efficiency **at** breakpoint_i (point-slope
 * form).  This is the concave-envelope minimum, matching the Fortran:
 * ```fortran
 * ValFRendimientos = MIN(ValFRendimientos,
 *     Constantes(i) + Pendientes(i) * (Vol - Bordes(i)))
 * ```
 *
 * **Difference from seepage**: `ReservoirSeepageSegment.constant` is the
 * y-intercept (value at V = 0), while `ProductionFactorSegment.constant` is
 * the value at the breakpoint.  ReservoirSeepage uses range-based segment
 * selection; efficiency uses concave-envelope minimum.
 *
 * Returns at least 0.0 (efficiency cannot be negative).
 *
 * @param segments The piecewise-linear segments
 * @param volume Current reservoir volume [hm³]
 * @return Efficiency value (non-negative)
 */
[[nodiscard]] inline auto evaluate_production_factor(
    const std::vector<ProductionFactorSegment>& segments, Real volume) noexcept
    -> Real
{
  if (segments.empty()) {
    return 1.0;
  }
  auto result = std::numeric_limits<Real>::max();
  for (const auto& seg : segments) {
    const auto val = seg.constant + (seg.slope * (volume - seg.volume));
    result = std::min(result, val);
  }
  return std::max(result, 0.0);
}

}  // namespace gtopt
