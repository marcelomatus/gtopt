/**
 * @file      simple_commitment.hpp
 * @brief     Simplified commitment data for generator min-output enforcement
 * @date      2026-04-13
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the SimpleCommitment structure which specifies a simplified
 * unit commitment constraint for a generator: if dispatched (u=1), the
 * generator must produce at least dispatch_pmin MW.  Unlike the full
 * Commitment model, SimpleCommitment:
 * - Uses only one binary variable u (no startup/shutdown tracking)
 * - Does not require chronological stages
 * - Has no no-load cost, ramp, or minimum up/down time constraints
 *
 * Constraints:
 * - p <= Pmax * u  (upper generation limit)
 * - p >= dispatch_pmin * u  (minimum output when dispatched)
 *
 * When `relax` is true, u is continuous in [0,1], providing the LP
 * relaxation used by PLP-style inertia formulations.
 *
 * @see SimpleCommitmentLP for the LP formulation
 * @see Generator for the linked generation unit
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct SimpleCommitment
 * @brief Simplified commitment parameters for a generator
 *
 * Links to a Generator and creates a single binary variable u per block:
 * - u = 1: generator is dispatched, p in [dispatch_pmin, Pmax]
 * - u = 0: generator is off, p = 0
 */
struct SimpleCommitment
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `SimpleCommitmentLP` exposes no separate `ClassName` member;
  /// callers reach the constant via `SimpleCommitment::class_name`
  /// directly (or `SimpleCommitmentLP::Element::class_name` in generic
  /// contexts).
  static constexpr LPClassName class_name {"SimpleCommitment"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  SingleId generator {unknown_uid};  ///< FK to the Generator

  OptTBRealFieldSched dispatch_pmin {};  ///< Min output when dispatched [MW]
  OptBool relax {};  ///< LP relaxation: u continuous in [0,1]
  OptBool must_run {};  ///< Force committed: u = 1 always
};

}  // namespace gtopt
