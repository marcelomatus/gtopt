/**
 * @file      demand_profile.hpp
 * @brief     Demand profile configuration and attributes
 * @date      Wed Apr  2 01:23:45 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the DemandProfile structure which provides a time-varying scaling
 * profile for a demand element. The actual maximum served load in each block
 * is `lmax × profile`, enabling typical daily/seasonal demand shapes.
 *
 * ### JSON Example — inline 24-hour load profile
 * ```json
 * {
 *   "uid": 1,
 *   "name": "d1_daily",
 *   "demand": "d1",
 *   "profile": [0.6,0.58,0.57,0.56,0.57,0.6,0.7,0.85,0.95,0.98,1.0,0.99,
 *               0.97,0.96,0.97,0.99,1.0,0.98,0.95,0.9,0.85,0.78,0.72,0.65]
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant (applies uniformly to all blocks)
 * - A 3-D inline array indexed by `[scenario][stage][block]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/DemandProfile/`
 */

#pragma once

#include <gtopt/demand.hpp>

namespace gtopt
{

/**
 * @brief Time-varying load-shape profile for a demand element
 *
 * Multiplies the demand's maximum load (`lmax`) by the profile value in
 * each block. Useful for modelling daily, weekly, and seasonal demand
 * variability without storing per-block `lmax` values in the demand itself.
 *
 * @see Demand for the load element
 * @see DemandProfileLP for the LP formulation
 */
struct DemandProfile
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  SingleId demand {unknown_uid};  ///< ID of the associated demand element
  STBRealFieldSched profile {};  ///< Load-scaling profile [p.u. of lmax]
  OptTRealFieldSched
      scost {};  ///< Short-run load-shedding cost override [$/MWh]
};

}  // namespace gtopt
