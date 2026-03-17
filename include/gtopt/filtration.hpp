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
 * seepage [m³/s] = slope_i × avg_volume [dam³] + constant_i [m³/s]
 * ```
 *
 * When `segments` is non-empty, the active segment i is the one whose
 * `volume` breakpoint is the largest that is ≤ the current reservoir volume
 * (matches PLP Fortran `FFiltracionesi`):
 *
 * ### Piecewise-linear evaluation (PLP formula)
 * ```
 * Find segment i such that: segments[i].volume ≤ V < segments[i+1].volume
 * filtration(V) = constant_i + slope_i × V
 * ```
 * Here `constant_i` is the **y-intercept** (filtration at V = 0 for segment i's
 * linear equation), matching the PLP file format (plpfilemb.dat `Constante`
 * field and `FiltParam(PFiltConst)` in Fortran).  This is **not** the value
 * at the breakpoint — the conversion `constant_at_breakpoint = constant_y +
 * slope × volume` applies when comparing with the legacy gtopt representation.
 *
 * ### Per-stage slope/constant schedules (PLP plpmanfi.dat)
 *
 * `slope` and `constant` accept the same "number | array | filename" syntax
 * used by other schedule fields:
 * - **Scalar** – same value for all stages (legacy behaviour).
 * - **Array** – one value per stage index.
 * - **Filename** – Parquet/CSV table in `input_directory/Filtration/`.
 *
 * When per-stage schedules are used, the LP matrix coefficients for each
 * stage are set to the stage-specific values directly in the LP (not via
 * LP bounds), analogous to how `ReservoirEfficiency` updates the turbine
 * conversion-rate coefficient.  If `segments` is also present, the
 * piecewise-linear volume-dependent update takes precedence (via
 * `FiltrationLP::update_lp`).
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
 *
 * Per-stage schedule (from plpmanfi.dat):
 * ```json
 * { "uid": 1, "name": "filt1", "waterway": 1, "reservoir": 1,
 *   "slope": "slope", "constant": "constant" }
 * ```
 */

#pragma once

#include <algorithm>
#include <vector>

#include <gtopt/field_sched.hpp>
#include <gtopt/object.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

/**
 * @brief One segment of the piecewise-linear filtration curve
 *
 * Each segment defines a linear function `filtration = constant + slope × V`
 * that is active when the reservoir volume is in range
 * `[volume, next_segment.volume)`.  `constant` is the **y-intercept** at
 * V = 0, matching the PLP file format (`plpfilemb.dat` `Constante` field /
 * Fortran `FiltParam(PFiltConst)`).  For a continuous piecewise function the
 * constants are chosen so that adjacent segments agree at the breakpoint:
 * `constant_i = constant_{i-1} + slope_{i-1} × volume_i - slope_i × volume_i`.
 */
struct FiltrationSegment
{
  Real volume {
      0.0,
  };  ///< Volume breakpoint [dam³] – segment applies when V ≥ volume
  Real slope {0.0};  ///< Seepage slope [m³/s / dam³] (Fortran PFiltPend / 1000)
  Real constant {
      0.0,
  };  ///< Y-intercept [m³/s]: filtration at V=0 for this segment
};

/**
 * @brief Piecewise-linear seepage model between a waterway and a reservoir
 *
 * Models water seepage from a waterway into an adjacent reservoir.
 * The filtration flow per block is:
 *   `filtration_flow = slope × V_avg + constant`
 * where `V_avg = (eini + efin) / 2` is the average reservoir volume and
 * `constant` is the **y-intercept** (filtration at V = 0 for the active
 * segment's linear equation).  This matches the PLP Fortran formula
 * (`FiltConst + FiltPend × Vol`).
 *
 * `slope` and `constant` accept per-stage schedules (scalar, array, or
 * filename pointing to a Parquet/CSV table in `input_directory/Filtration/`).
 * This is the PLP plpmanfi.dat model: the LP matrix coefficients (slope on
 * eini/efin columns) and the RHS (constant y-intercept) are set directly in
 * the LP for each stage, not via LP bounds.
 *
 * When `segments` is non-empty, the piecewise-linear volume-dependent update
 * (via FiltrationLP::update_lp) takes precedence and overrides any per-stage
 * schedule values for the active-segment coefficient.
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

  /// Seepage slope [m³/s / dam³] – scalar, per-stage array, or filename.
  /// When `segments` is empty, this provides the LP coefficient on V_avg.
  /// Accepts the same "number | array | filename" syntax as other schedule
  /// fields (e.g. Reservoir::emin).  The default is 0.0.
  OptTRealFieldSched slope {};

  /// Y-intercept seepage rate [m³/s] – scalar, per-stage array, or filename.
  /// When `segments` is empty, this provides the LP RHS (filtration at V=0).
  /// Accepts the same "number | array | filename" syntax.  Default is 0.0.
  OptTRealFieldSched constant {};

  /// Piecewise-linear segments for volume-dependent filtration rate.
  /// When non-empty, the active segment is selected per-phase based on the
  /// current reservoir volume (range selection: largest breakpoint ≤ V)
  /// and the LP coefficients are updated dynamically (via
  /// FiltrationLP::update_lp).
  /// Each segment's `constant` is the y-intercept at V=0 (PLP
  /// `FiltParam(PFiltConst)` format, not the value at the breakpoint).
  std::vector<FiltrationSegment> segments {};
};

// ─── Piecewise-linear evaluation ────────────────────────────────────────────

/**
 * @brief Find the active segment for a given reservoir volume
 *
 * Returns a pointer to the last segment whose volume breakpoint is ≤ `volume`
 * (range selection, matching PLP Fortran `FFiltracionesi`).  Returns the
 * first segment when `volume` is below all breakpoints.
 *
 * **Precondition**: `segments` must be non-empty and sorted in ascending order
 * by `volume` breakpoint.  Results are undefined if this is violated.
 *
 * @param segments The piecewise-linear segments (sorted by volume breakpoint)
 * @param volume Current reservoir volume [dam³]
 * @return Pointer to the active FiltrationSegment (never null)
 */
[[nodiscard]] inline auto find_active_segment(
    const std::vector<FiltrationSegment>& segments, Real volume) noexcept
    -> const FiltrationSegment*
{
  // Invariant: segments is non-empty and sorted ascending by .volume.
  // Walk forward and keep advancing while the next breakpoint is still ≤
  // volume.
  const FiltrationSegment* active = &segments.front();
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
 * @brief Evaluate the piecewise-linear filtration function
 *
 * Implements the PLP filtration model (matches Fortran `FFiltracionesi`):
 * ```
 * Find the segment i where volume_i ≤ volume < volume_{i+1},
 * then compute:  result = constant_i + slope_i × volume
 * ```
 * Here `constant_i` is the **y-intercept at V = 0** for segment i's linear
 * equation, matching the PLP file format (plpfilemb.dat `Constante` field).
 *
 * Returns at least 0.0 (filtration rate cannot be negative).
 *
 * @param segments The piecewise-linear segments (sorted by volume breakpoint)
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
  const auto* active = find_active_segment(segments, volume);
  // PLP formula: filtration = constant + slope × V  (constant is y-intercept)
  const auto result = active->constant + (active->slope * volume);
  return std::max(result, 0.0);
}

/**
 * @brief LP constraint coefficients for volume-dependent filtration
 *
 * The filtration LP constraint is:
 *   `filtration_flow = slope × V_avg + constant`
 * where `V_avg = (eini + efin) / 2` and `constant` is the **y-intercept**
 * (the PLP file `Constante` field, matching Fortran `FiltParam(PFiltConst)`).
 *
 * In matrix form: `filt - slope*0.5*eini - slope*0.5*efin = constant`
 *
 * This matches PLP `Filtracv` (FILT_LINE mode): the RHS is set to the
 * raw `FiltConst` value and the volume column coefficient to `-pend *
 * ScaleVol`.
 */
struct FiltrationCoeffs
{
  Real slope {0.0};  ///< Coefficient on V_avg  [m³/s / dam³]
  Real intercept {0.0};  ///< RHS constant = y-intercept at V=0  [m³/s]
};

/**
 * @brief Select the active filtration segment and return LP coefficients
 *
 * Implements the PLP range-selection logic (matches Fortran `FFiltracionesi`):
 * finds the segment i where `segments[i].volume ≤ volume <
 * segments[i+1].volume` and returns its slope and constant (y-intercept) as LP
 * coefficients.
 *
 * The LP constraint becomes:
 *   `filt_flow - slope*0.5*eini - slope*0.5*efin = constant`
 * where `constant` is the y-intercept at V = 0 for the active segment's
 * linear equation:  `filtration = constant + slope × V`.
 *
 * @param segments The piecewise-linear segments (sorted by volume breakpoint)
 * @param volume Current reservoir volume [dam³]
 * @return FiltrationCoeffs with slope and intercept (y-intercept) for the
 *         active segment
 */
[[nodiscard]] inline auto select_filtration_coeffs(
    const std::vector<FiltrationSegment>& segments, Real volume) noexcept
    -> FiltrationCoeffs
{
  if (segments.empty()) {
    return {};
  }

  const auto* active = find_active_segment(segments, volume);

  return {
      .slope = active->slope,
      .intercept =
          active->constant,  // y-intercept: filtration = constant + slope*V
  };
}

}  // namespace gtopt
