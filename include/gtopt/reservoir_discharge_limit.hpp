/**
 * @file      reservoir_discharge_limit.hpp
 * @brief     Piecewise-linear volume-dependent discharge limit for reservoirs
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the ReservoirDischargeLimit structure modelling a volume-dependent
 * maximum discharge rate for a reservoir.  This is a safety/environmental
 * constraint that limits the hourly-average discharge as a piecewise-linear
 * function of the reservoir volume (e.g. to prevent landslides or excessive
 * drawdown).
 *
 * The constraint per stage is:
 * ```text
 * qeh ≤ a(seg) × V_avg + b(seg)
 * ```
 * where `qeh` is the stage-average hourly discharge [m³/s], `V_avg` is the
 * average reservoir volume `(eini + efin) / 2` [hm³], and `a`/`b` are the
 * slope and intercept of the active piecewise-linear segment.
 *
 * When `segments` has more than one entry, `update_lp()` selects the active
 * segment based on the reservoir's current volume (from the previous LP
 * solve) and dynamically updates the LP constraint coefficients.
 *
 * ### PLP origin
 * This element generalises the PLP "Ralco" constraint (`plpralco.dat`,
 * `genpdralco.f`).
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "ddl_ralco",
 *   "reservoir": "RALCO",
 *   "segments": [
 *     { "volume": 0.0,   "slope": 0.069868, "intercept": 15.787 },
 *     { "volume": 757.0, "slope": 0.13985,  "intercept": 57.454 }
 *   ]
 * }
 * ```
 */

#pragma once

#include <vector>

#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

/**
 * @brief One segment of the piecewise-linear drawdown limit curve
 *
 * Each segment defines a linear function
 * `max_discharge = slope × V + intercept`
 * active when `V ≥ volume` (and `V < next_segment.volume`).
 */
struct ReservoirDischargeLimitSegment
{
  Real volume {
      0.0,
  };  ///< Volume breakpoint [hm³]
  Real slope {0.0};  ///< Slope [m³/s per hm³] (Fortran ARalco × 1000)
  Real intercept {
      0.0,
  };  ///< Y-intercept [m³/s] (Fortran BRalco)
};

/**
 * @brief Volume-dependent discharge limit for a reservoir
 *
 * Models a piecewise-linear upper bound on the stage-average hourly
 * discharge as a function of reservoir volume.  The LP formulation
 * creates one `qeh` variable per stage and links it to block-level
 * waterway flows via an averaging constraint.
 *
 * @see ReservoirDischargeLimitLP for the LP formulation
 * @see Reservoir for the storage element
 */
struct ReservoirDischargeLimit
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `ReservoirDischargeLimitLP` exposes no separate `ClassName` member;
  /// callers reach the constant via `ReservoirDischargeLimit::class_name`
  /// directly (or `ReservoirDischargeLimitLP::Element::class_name` in
  /// generic contexts).
  static constexpr LPClassName class_name {"ReservoirDischargeLimit"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Operational status (default: active)

  SingleId waterway {
      unknown_uid};  ///< ID of the waterway whose flow is limited
  SingleId reservoir {unknown_uid};  ///< ID of the reservoir (volume source)

  /// Piecewise-linear segments (sorted ascending by volume breakpoint).
  /// At least one segment is required.
  std::vector<ReservoirDischargeLimitSegment> segments {};
};

// ─── Piecewise-linear evaluation ────────────────────────────────────────────

/**
 * @brief Find the active segment for a given reservoir volume
 *
 * Returns a pointer to the last segment whose volume breakpoint is ≤ `volume`.
 *
 * **Precondition**: `segments` must be non-empty and sorted ascending by
 * volume.
 */
[[nodiscard]] inline auto find_active_rdl_segment(
    const std::vector<ReservoirDischargeLimitSegment>& segments,
    Real volume) noexcept -> const ReservoirDischargeLimitSegment*
{
  const ReservoirDischargeLimitSegment* active = &segments.front();
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
 * @brief LP constraint coefficients for the drawdown limit
 *
 * The stage-level constraint is:
 *
 * ```text
 * qeh - slope × ScaleVol × 0.5 × eini
 *     - slope × ScaleVol × 0.5 × efin ≤ intercept
 * ```
 */
struct ReservoirDischargeLimitCoeffs
{
  Real slope {0.0};  ///< Coefficient on V_avg [m³/s per hm³]
  Real intercept {0.0};  ///< RHS upper bound [m³/s]
};

/**
 * @brief Select the active segment and return LP coefficients
 */
[[nodiscard]] inline auto select_rdl_coeffs(
    const std::vector<ReservoirDischargeLimitSegment>& segments,
    Real volume) noexcept -> ReservoirDischargeLimitCoeffs
{
  if (segments.empty()) {
    return {};
  }

  const auto* active = find_active_rdl_segment(segments, volume);

  return {
      .slope = active->slope,
      .intercept = active->intercept,
  };
}

}  // namespace gtopt
