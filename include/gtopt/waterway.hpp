/**
 * @file      waterway.hpp
 * @brief     Header for waterway components in hydro power systems
 * @date      Wed Jul 30 11:40:33 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Waterway structure representing a water channel
 * connecting two hydraulic junctions. Waterways carry water flow between
 * junctions and are the analogue of transmission lines in the hydraulic
 * network.
 *
 * Flow units: **m³/s** (cubic metres per second).
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "w1_2",
 *   "junction_a": "j1",
 *   "junction_b": "j2",
 *   "fmin": 0,
 *   "fmax": 300
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 2-D inline array indexed by `[stage][block]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Waterway/`
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

/**
 * @struct Waterway
 * @brief Water channel connecting two hydraulic junctions
 *
 * A waterway carries water from an upstream junction (`junction_a`) to a
 * downstream junction (`junction_b`).  Flow is constrained between `fmin`
 * and `fmax`.  An optional `lossfactor` models seepage or evaporation in
 * transit.  Turbines are attached to waterways to generate electricity.
 *
 * @see Junction for hydraulic node definitions
 * @see Turbine for the power-generation coupling
 * @see WaterwayLP for the LP formulation
 */
struct Waterway
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `WaterwayLP` exposes no separate `ClassName` member; callers reach
  /// the constant via `Waterway::class_name` directly (or
  /// `WaterwayLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"Waterway"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable waterway name
  OptActive active {};  ///< Activation status (default: active)

  SingleId junction_a {unknown_uid};  ///< Upstream junction ID
  SingleId junction_b {unknown_uid};  ///< Downstream junction ID

  OptTRealFieldSched capacity {};  ///< Maximum flow capacity [m³/s]
  OptTRealFieldSched lossfactor {0.0};  ///< Transit loss coefficient [p.u.]

  OptTBRealFieldSched fmin {0.0};  ///< Minimum required water flow [m³/s]
  OptTBRealFieldSched fmax {300'000.0};  ///< Maximum allowed water flow [m³/s]

  OptTRealFieldSched fcost {};  ///< Per-flow cost on `waterway_flow` column
                                ///< [$/(m³/s)/h] — applied via
                                ///< CostHelper::block_ecost(...) so the LP
                                ///< pays `fcost · q · duration` per block.
                                ///< Used to model PLP `qrb`-style spillway
                                ///< penalties on `_ver` arcs (rebalse cost
                                ///< from plpvrebemb.dat).
};

}  // namespace gtopt
