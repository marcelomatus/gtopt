/**
 * @file      mip_start_scip.cpp
 * @brief     scip_repair MIP-start generator — repair a rounded commitment to
 *            feasibility with SCIP, then inject it into the ACTIVE backend
 * @date      2026-06-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `MipStartMethod::scip_repair`.  Where `lp_round` rounds the LP relaxation and
 * hopes the active backend's own heuristic mends the residual hard-constraint
 * violations (min-up/down, Pmin, user constraints) — which cuOpt/HiGHS cannot —
 * this generator hands the rounded commitment to SCIP, whose completesol /
 * repair primal heuristics turn it into a GENUINELY feasible integer solution,
 * and overlays that on the active solver's relaxation base.
 *
 * DESIGN — reuse, don't reinvent.  A second `LinearInterface` is built on the
 * "scip" backend straight from `ctx.flat_lp` via the SAME flat->backend load
 * path every solver uses (`LinearInterface(name, flat_lp)` ->
 * `SolverBackend::load_problem`), so SCIP column j == raw LP column j == the
 * dense-start index, with no name matching and no hand-rolled matrix build.
 * The rounded commitment is handed to SCIP through the existing
 * `set_mip_start(.., MipStartEffort::repair)` hook — the SCIP plugin maps
 * `repair` to its completesol/repair heuristic — then the repaired integer
 * solution is read back with `get_col_sol_raw()`.  ALL SCIP-specific code lives
 * in the SCIP plugin; the core needs no SCIP headers and no `#ifdef`.  If the
 * "scip" backend isn't registered (plugin not built), the generator declines
 * (nullopt) — a benign skip, exactly like an unknown method.
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

/// The sub-solver used to repair the rounded commitment.  Kept as a named
/// constant (rather than hard-coding the literal) so a future option could
/// route the repair through any MIP backend with a strong feasibility
/// heuristic (CBC, Gurobi, ...).
constexpr std::string_view kRepairSolver = "scip";

class ScipRepairMipStart final : public MipStartGenerator
{
public:
  [[nodiscard]] std::string_view name() const noexcept override
  {
    return "scip_repair";
  }

  [[nodiscard]] std::optional<std::vector<double>> generate(
      MipStartContext& ctx) override
  {
    if (ctx.flat_lp == nullptr) {
      spdlog::warn(
          "MIP-start[scip_repair]: no flat LP supplied (the build must retain "
          "the flat problem for this method); skipping");
      return std::nullopt;
    }
    if (!SolverRegistry::instance().has_solver(kRepairSolver)) {
      spdlog::warn(
          "MIP-start[scip_repair]: solver '{}' is not available (build the "
          "SCIP "
          "plugin); skipping",
          kRepairSolver);
      return std::nullopt;
    }

    auto& li = ctx.li;
    const auto sol = li.get_col_sol_raw();  // RAW relaxation primal = base
    if (sol.empty()) {
      return std::nullopt;
    }
    const auto lb = li.get_col_low_raw();
    const auto ub = li.get_col_upp_raw();
    const double threshold = ctx.opts.round_threshold.value_or(0.5);

    // 1. Round the integer columns + min-up/down repair → the seed commitment
    //    (also the base vector: continuous columns stay at the relaxation).
    std::vector<double> start(sol.begin(), sol.end());
    for (const int i : ctx.int_cols) {
      const auto u = static_cast<std::size_t>(i);
      const double l = (u < lb.size()) ? lb[u] : 0.0;
      const double h = (u < ub.size()) ? ub[u] : 1.0;
      start[u] = detail::round_with_threshold(sol[u], threshold, l, h);
    }
    const int flipped = detail::repair_run_lengths(start, ctx.commitments);
    if (flipped > 0) {
      spdlog::info(
          "MIP-start[scip_repair]: min-up/down repair flipped {} status values "
          "before SCIP",
          flipped);
    }

    // 2. Build a SCIP model from the SAME flat LP (standard load path; SCIP
    //    column j == raw LP column j) and let SCIP repair the seed commitment
    //    via its completesol/repair heuristic (the plugin's effort=repair).
    LinearInterface scip_li {kRepairSolver, *ctx.flat_lp};
    if (!scip_li.set_mip_start(start, MipStartEffort::repair)) {
      spdlog::warn(
          "MIP-start[scip_repair]: SCIP backend declined the start; skipping");
      return std::nullopt;
    }
    const auto rr = scip_li.resolve(ctx.relax_opts);  // inherits time_limit
    // A repair pass may stop at the first feasible incumbent (not proven
    // optimal), so accept any non-infeasible result with a usable primal —
    // do NOT gate on is_optimal().  The exact feasibility status mapping is the
    // SCIP plugin's responsibility.
    if (!rr || scip_li.is_prim_infeasible()) {
      spdlog::warn(
          "MIP-start[scip_repair]: SCIP found no feasible solution; skipping");
      return std::nullopt;
    }
    const auto repaired = scip_li.get_col_sol_raw();
    if (repaired.size() != start.size()) {
      spdlog::warn(
          "MIP-start[scip_repair]: SCIP column count {} != LP {}; skipping",
          repaired.size(),
          start.size());
      return std::nullopt;
    }

    // 3. Overlay SCIP's repaired INTEGER values onto the active solver's
    //    relaxation base (continuous columns keep this solver's relaxation —
    //    same contract as lp_round / file).
    std::size_t placed = 0;
    for (const int i : ctx.int_cols) {
      const auto u = static_cast<std::size_t>(i);
      start[u] = repaired[u];
      ++placed;
    }
    spdlog::info(
        "MIP-start[scip_repair]: SCIP repaired {} integer columns into a "
        "feasible start",
        placed);
    return start;
  }
};

}  // namespace

std::unique_ptr<MipStartGenerator> make_scip_repair_generator()
{
  // The SCIP repair runs through the generic backend interface, so this is
  // always constructible; it self-skips at run time if the "scip" plugin or the
  // flat LP is absent.
  return std::make_unique<ScipRepairMipStart>();
}

}  // namespace gtopt
