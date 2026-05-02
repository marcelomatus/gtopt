/**
 * @file      generator_profile.hpp
 * @brief     Generator profile configuration and attributes
 * @date      Tue Apr  1 21:20:35 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the GeneratorProfile structure which provides a time-varying
 * capacity-factor profile for a generator. The actual available capacity in
 * each block is `capacity × profile`, enabling renewable (solar, wind) and
 * hydro run-of-river generation modeling.
 *
 * ### JSON Example — inline 24-hour solar profile
 * ```json
 * {
 *   "uid": 1,
 *   "name": "g3_solar",
 *   "generator": "g3",
 *   "profile":
 * [0,0,0,0,0,0.1,0.4,0.7,0.9,1,1,0.95,0.9,0.85,0.8,0.6,0.3,0.1,0,0,0,0,0,0]
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant (applies uniformly to all blocks)
 * - A 3-D inline array indexed by `[scenario][stage][block]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/GeneratorProfile/`
 */

#pragma once

#include <gtopt/generator.hpp>
#include <gtopt/lp_class_name.hpp>

namespace gtopt
{

/**
 * @brief Time-varying capacity-factor profile for a generator
 *
 * Multiplies the generator's maximum active power (`pmax` or `capacity`) by
 * the profile value in each block.  Useful for solar irradiance, wind speed,
 * or hydro run-of-river availability.
 *
 * @see Generator for the generation unit
 * @see GeneratorProfileLP for the LP formulation
 */
struct GeneratorProfile
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `GeneratorProfileLP` exposes no separate `ClassName` member;
  /// callers reach the constant via `GeneratorProfile::class_name`
  /// directly (or `GeneratorProfileLP::Element::class_name` in generic
  /// contexts).
  static constexpr LPClassName class_name {"GeneratorProfile"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  SingleId generator {unknown_uid};  ///< ID of the associated generator
  STBRealFieldSched
      profile {};  ///< Capacity-factor profile [p.u. of installed capacity]
  OptTRealFieldSched scost {};  ///< Short-run generation cost override [$/MWh]
};

}  // namespace gtopt
