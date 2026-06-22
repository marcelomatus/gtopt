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

bool FutureCostLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();
  // α / varphi_s: one stream per α column (single layout → "alpha"; multicut →
  // "alpha_<s>").  `add_col_sol` reads `col_sol[α_col]` at the holder's
  // terminal coords, so the α column's own scene-phase context is irrelevant
  // on the read side.
  for (const auto& stream : m_alpha_streams_) {
    out.add_col_sol(cname, stream.name, id(), stream.cols);
  }
  // Per-scene α-rebase constant c̄ ($) — the offset `mean_shift` folds into the
  // objective; surfaced so `alpha + rebase` reconstructs the un-rebased FCF.
  if (!m_rebase_.empty()) {
    out.add_col_sol_values(cname, RebaseName, id(), m_rebase_);
  }
  return true;
}

}  // namespace gtopt
