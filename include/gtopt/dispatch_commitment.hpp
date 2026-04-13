/**
 * @file      dispatch_commitment.hpp
 * @brief     Simplified dispatch commitment data for generator min-output
 * @date      2025
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Defines the DispatchCommitment structure which specifies a simplified
 * unit commitment constraint for a generator: if dispatched (u=1), the
 * generator must produce at least dispatch_pmin MW.  Unlike the full
 * Commitment model, DispatchCommitment:
 * - Uses only one binary variable u (no startup/shutdown tracking)
 * - Does not require chronological stages
 * - Has no no-load cost, ramp, or minimum up/down time constraints
 *
 * Constraints:
 * - p ≤ Pmax · u  (upper generation limit)
 * - p ≥ dispatch_pmin · u  (minimum output when dispatched)
 *
 * @see DispatchCommitmentLP for the LP formulation
 * @see Generator for the linked generation unit
 */

#pragma once

#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct DispatchCommitment
 * @brief Simplified dispatch commitment parameters for a generator
 *
 * Links to a Generator and creates a single binary variable u per block:
 * - u = 1: generator is dispatched, p ∈ [dispatch_pmin, Pmax]
 * - u = 0: generator is off, p = 0
 */
struct DispatchCommitment
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  SingleId generator {unknown_uid};  ///< FK to the Generator

  OptTBRealFieldSched dispatch_pmin {};  ///< Min output when dispatched [MW]
  OptBool relax {};  ///< LP relaxation: u continuous in [0,1]
  OptBool must_run {};  ///< Force committed: u = 1 always
};

}  // namespace gtopt
