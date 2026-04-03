/**
 * @file      user_param.hpp
 * @brief     User-defined named parameters for constraint expressions
 * @date      Wed Apr  2 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the UserParam struct, which stores a named constant or
 * monthly-indexed parameter for use in pseudo-AMPL constraint scripts.
 *
 * Parameters can be defined in a pseudo-AMPL script file:
 *
 * ```text
 * # Scalar parameter
 * param pct_elec = 35;
 *
 * # Monthly-indexed parameter (jan..dec)
 * param irr_seasonal[month] = [0, 0, 0, 100, 100, 100, 100, 100, 100, 100, 0,
 * 0];
 * ```
 *
 * Or in JSON:
 *
 * ```json
 * {"name": "pct_elec", "value": 35.0}
 * {"name": "irr_seasonal", "monthly": [0,0,0,100,100,100,100,100,100,100,0,0]}
 * ```
 *
 * When a monthly-indexed parameter is referenced in a constraint,
 * the value is automatically resolved using the stage's calendar month.
 */

#pragma once

#include <optional>

#include <gtopt/basic_types.hpp>

namespace gtopt
{

/// Named parameter for user constraint expressions.
///
/// Either `value` (scalar) or `monthly` (12-element array) must be set.
/// When `monthly` is set, the parameter is resolved using the stage's
/// calendar month (0=january, 11=december).
struct UserParam
{
  Name name {};  ///< Parameter name (referenced in expressions)
  OptReal value {};  ///< Scalar value (mutually exclusive with monthly)
  std::optional<std::vector<Real>>
      monthly {};  ///< Monthly values [jan=0 .. dec=11], must have 12 elements
};

/// A collection of named parameters, keyed by name for fast lookup.
using UserParamMap = std::unordered_map<Name, UserParam>;

}  // namespace gtopt
