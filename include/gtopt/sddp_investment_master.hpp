/**
 * @file      sddp_investment_master.hpp
 * @brief     OptGen-style investment-master Benders loop — a MIP master
 *            over here-and-now integer expansion builds coupled to the
 *            gtopt operational SDDP through capacity-space Benders cuts
 *            (deliverable 4 of the SDDiP campaign; design in
 *            docs/analysis/investigations/sddp/sddip_integer_expansion_2026-07.md
 *            §7).
 * @date      2026-07-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * ## Two-level decomposition (PSR OptGen + SDDP parity)
 *
 * Investment (here-and-now, integer) is separated from operation
 * (multistage, continuous recourse):
 *
 *   - **Master (MIP)** — variables are the per-candidate integer build
 *     counts `n_g ∈ [0, expmod]`, an accumulated capacity state `K_g`
 *     mirroring the `capainst` accounting of `capacity_object_lp.cpp`, and
 *     one epigraph variable `θ_s` per scene.  Objective = Σ_s θ_s,
 *     minimised subject to the capacity-space Benders cuts
 *     `θ_s ≥ rhs − Σ_j coeff_j · K_j` accumulated from the operational
 *     oracle.  Built directly on a `LinearInterface` (tens of columns) —
 *     no `Planning` / `PlanningLP` needed for the master itself.
 *
 *     The per-scene θ carries the FULL discounted scene cost (OPEX + the
 *     CAPEX folded into the `capacost` state), so the master needs NO
 *     separate CAPEX term: the annualised investment charge is already
 *     inside every capacity cut (design §7.3 caveat 3, per-scene θ).
 *
 *   - **Operational oracle** — the gtopt SDDP method run on a copy of the
 *     input `Planning` whose expansion candidates have `expmod` pinned to
 *     the master's proposed build `n*` (a pure-data mutation before
 *     `PlanningLP` construction — the least-invasive fixing route, design
 *     §7.3 caveat 4).  Returns (i) the expected operational cost (the
 *     converged lower bound, used as the loop UB at `n*`) and (ii) the
 *     phase-0 capacity subgradients via `extract_capacity_cuts`.
 *
 *   - **Loop** — solve master → pin capacities → run SDDP → extract +
 *     project capacity cuts onto master `K` coordinates → add to master →
 *     repeat.  LB = master objective; UB = best operational cost seen at a
 *     master proposal.  Stops on `UB − LB ≤ tol` or the iteration cap.
 *
 * ## Validity
 *
 * Operations are convex in capacity (capacities enter the operational LP
 * as bounds/RHS), so the expected operational cost Φ(K) is convex
 * piecewise-linear and each extracted cut is a valid support (Theorem
 * O1/O2 of `docs/formulation/sddp-cut-validity.md`).  Integers live ONLY
 * in the master, so the master's branch-and-bound is exact — this is
 * exactly why PSR pairs OptGen with SDDP instead of running SDDiP.
 *
 * ## v1 scope / caveats (design §7.3)
 *
 *   1. The projection to capacity coordinates is exact only when the
 *      non-capacity phase-0 states (reservoir energy) are pinned across
 *      master iterations (they are — `eini` is data).  A pure-expansion
 *      fixture (no reservoir) makes the projection exact (0 dropped
 *      coordinates) — the acceptance test uses one.
 *   2. Single here-and-now build decision per candidate is the tested
 *      path: the master aggregates a candidate's capacity across the
 *      horizon into one `K_g` column and pins the same `n*` across every
 *      stage of the operational copy.  Per-stage staggered builds are the
 *      documented remainder (design §7, §8).
 */

#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/error.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/sddp_capacity_cuts.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/solver_options.hpp>

namespace gtopt
{

/// One master-side expansion candidate: an element carrying `expcap` /
/// `expmod` in the input `Planning`, resolved to the class/uid the
/// capacity state variables (`capainst`) are registered under.
struct ExpansionCandidate
{
  std::string class_name {};  ///< "Generator" / "Demand" / …
  Uid uid {unknown_uid};  ///< Element UID
  std::string name {};  ///< Element name (diagnostics)
  double expcap {};  ///< Capacity added per module [MW]
  double max_expmod {};  ///< Upper bound on integer build count
  bool integer {true};  ///< Integer-constrain the build count
};

/// Options controlling the outer investment-master loop.
struct InvestmentMasterOptions
{
  /// Maximum number of master ↔ SDDP round trips.
  int max_iterations {20};
  /// Absolute convergence tolerance on `UB − LB`.
  double tol {1.0e-6};
  /// SDDP options for each operational oracle run (max_iterations,
  /// convergence_tol, cut_sharing = none, …).  The loop overrides
  /// nothing except forcing `enable_api = false` internally.
  SDDPOptions sddp_options {};
  /// LP/MIP solver options for both the master MIP and the operational
  /// SDDP.  The master pins a tight MIP gap onto a copy of these.
  SolverOptions solver_options {};
};

/// Per-iteration record of the outer loop.
struct InvestmentMasterIteration
{
  int iteration {};
  /// Master objective (Σ_s θ_s) — a valid lower bound on the true
  /// two-stage optimum once at least one cut is installed per scene.
  double lower_bound {};
  /// Operational cost at the master's proposed build (SDDP converged
  /// bound at pinned capacity) — a valid upper bound.
  double upper_bound {};
  /// `UB − LB` at this iteration (using the running best UB).
  double gap {};
  /// Proposed integer build count per candidate (candidate order).
  std::vector<double> builds {};
  /// Number of capacity cuts added to the master this iteration.
  std::size_t cuts_added {};
};

/// Result of running the investment-master loop to convergence (or the
/// iteration cap).
struct InvestmentMasterResult
{
  bool converged {};  ///< True when `UB − LB ≤ tol` was reached.
  double lower_bound {};  ///< Final master objective.
  double upper_bound {};  ///< Best operational cost seen.
  /// Best build vector found (the one attaining `upper_bound`).
  std::vector<double> best_builds {};
  /// The expansion candidates in the order `best_builds` indexes.
  std::vector<ExpansionCandidate> candidates {};
  /// Full per-iteration trajectory.
  std::vector<InvestmentMasterIteration> iterations {};
};

/// Discover the expansion candidates in @p planning — every generator /
/// demand element carrying a positive `expcap · expmod` (the same gate
/// `CapacityObjectBase::add_to_lp` uses to publish `capainst`).  The
/// `expmod` schedule is read at its scalar / first-stage value; the master
/// treats the build as a single here-and-now decision (v1).
[[nodiscard]] auto find_expansion_candidates(const Planning& planning)
    -> std::vector<ExpansionCandidate>;

/// Run the OptGen-style investment-master Benders loop on @p planning.
///
/// The input `Planning` supplies the operational model AND the expansion
/// candidates; it is copied for each oracle run (with `expmod` pinned to
/// the current master proposal) so the caller's object is never mutated.
///
/// @param planning  Operational planning with expansion candidates.
/// @param opts      Loop / SDDP / solver options.
/// @return The converged (or capped) master result, or an `Error` if the
///         master LP or an operational SDDP run failed.
[[nodiscard]] auto solve_investment_master(const Planning& planning,
                                           const InvestmentMasterOptions& opts)
    -> std::expected<InvestmentMasterResult, Error>;

}  // namespace gtopt
