/**
 * @file      future_cost_lp.cpp
 * @brief     LP build / output path for FutureCost (FCF / cost-to-go) elements
 * @date      Sun Jun 21 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Piece-2 step A: inert plumbing so the element type is wired end-to-end
 * (JSON → System → Collection → planning pass → output) with ZERO behavior
 * change.  The α-column registration, boundary-cut load, mean_shift rebase
 * (step D) and the alpha/rebase/approx_fcf output (step C) land in follow-ups.
 */

#include <gtopt/future_cost_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

bool FutureCostLP::add_to_global_lp(const SystemContext& /*sc*/,
                                    const SceneLP& /*scene*/,
                                    const PhaseLP& /*phase*/,
                                    LinearProblem& /*lp*/)
{
  // Piece-2 step A: inert.  α-column registration + boundary-cut load +
  // mean_shift rebase migrate here in step D.
  return true;
}

bool FutureCostLP::add_to_output(OutputContext& /*out*/) const
{
  // Piece-2 step A: inert.  Emits FutureCost/{alpha,rebase,approx_fcf} in
  // step C.
  return true;
}

}  // namespace gtopt
