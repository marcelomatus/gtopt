/**
 * @file      mip_start_scip.cpp
 * @brief     Optional SCIP repair STAGE of the MIP-start pipeline
 * @date      2026-06-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `scip_repair_candidate` is stage 3 of the MIP-start pipeline (round →
 * electric rules → **scip_repair** → solver repair + resolve).  It is OPTIONAL
 * and composable: enabled by `mip_start.scip_repair=true` on top of ANY base
 * method (lp_round / relax_fix / file) and ANY active solver (cuOpt, HiGHS,
 * CPLEX, …).
 *
 * Where the generic round + electric rules make the commitment *structurally*
 * plausible, this hands that candidate to SCIP, whose completesol / repair
 * primal heuristics turn it into a GENUINELY feasible integer solution — the
 * one thing cuOpt/HiGHS cannot do, and that even CPLEX's own repair sometimes
 * cannot crack.  The result is then injected into the active backend with the
 * configured `effort` (the "solver repair" stage), so e.g. on CPLEX it is
 * SCIP-structural-repair THEN CPLEX-numerical-repair.
 *
 * DESIGN — reuse, no SCIP in the core.  A second `LinearInterface` is built on
 * the "scip" backend straight from `ctx.flat_lp` via the SAME flat->backend
 * load path every solver uses, so SCIP column j == raw LP column j == the
 * candidate index, with no name matching and no hand-rolled matrix build.  The
 * candidate is handed to SCIP through the existing
 * `set_mip_start(.., MipStartEffort::repair)` hook — the SCIP plugin maps
 * `repair` to its completesol/repair heuristic.  ALL SCIP-specific code lives
 * in the SCIP plugin; the core needs no SCIP headers and no `#ifdef`.  When the
 * "scip" backend isn't registered (plugin not built) the stage returns
 * std::nullopt and the caller keeps the pre-SCIP candidate — a benign skip.
 *
 * STATUS — first draft; depends on the SCIP plugin (in progress) implementing
 * `set_mip_start(effort=repair)` as a completesol/repair pass.
 */

#include <optional>
#include <vector>

#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/mip_start.hpp>
#include <gtopt/solver_registry.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{
/// The sub-solver used to repair the candidate commitment.  Named (rather than
/// a bare literal) so a future option could route the repair through any MIP
/// backend with a strong feasibility heuristic (CBC, Gurobi, …).
constexpr std::string_view kRepairSolver = "scip";
}  // namespace

std::optional<std::vector<double>> scip_repair_candidate(
    MipStartContext& ctx, std::vector<double> candidate)
{
  if (candidate.empty()) {
    return std::nullopt;
  }
  if (ctx.flat_lp == nullptr) {
    spdlog::warn(
        "MIP-start[scip]: no flat LP supplied (the monolithic build must "
        "retain "
        "the flat problem); skipping the SCIP repair stage");
    return std::nullopt;
  }
  if (!SolverRegistry::instance().has_solver(kRepairSolver)) {
    spdlog::warn(
        "MIP-start[scip]: solver '{}' is not available (build the SCIP "
        "plugin); "
        "skipping the SCIP repair stage",
        kRepairSolver);
    return std::nullopt;
  }

  // Build a SCIP model from the SAME flat LP (standard load path; SCIP column
  // j == raw LP column j) and let SCIP repair the candidate commitment via its
  // completesol/repair heuristic (the plugin's effort=repair).
  LinearInterface scip_li {kRepairSolver, *ctx.flat_lp};
  if (!scip_li.set_mip_start(candidate, MipStartEffort::repair)) {
    spdlog::warn("MIP-start[scip]: SCIP backend declined the start; skipping");
    return std::nullopt;
  }
  const auto rr = scip_li.resolve(ctx.relax_opts);  // inherits time_limit, …
  // A repair pass may stop at the first feasible incumbent (not proven
  // optimal), so accept any non-infeasible result with a usable primal — do NOT
  // gate on is_optimal().  The exact status mapping is the SCIP plugin's job.
  if (!rr || scip_li.is_prim_infeasible()) {
    spdlog::warn("MIP-start[scip]: SCIP found no feasible solution; skipping");
    return std::nullopt;
  }
  const auto repaired = scip_li.get_col_sol_raw();
  if (repaired.size() != candidate.size()) {
    spdlog::warn("MIP-start[scip]: SCIP column count {} != LP {}; skipping",
                 repaired.size(),
                 candidate.size());
    return std::nullopt;
  }

  // Overlay SCIP's repaired INTEGER values onto the candidate (continuous
  // columns keep the active solver's relaxation — same contract as the round).
  std::size_t placed = 0;
  for (const int i : ctx.int_cols) {
    const auto u = static_cast<std::size_t>(i);
    candidate[u] = repaired[u];
    ++placed;
  }
  spdlog::info(
      "MIP-start[scip]: SCIP repaired {} integer columns into a feasible start",
      placed);
  return candidate;
}

}  // namespace gtopt
