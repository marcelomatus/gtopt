// SPDX-License-Identifier: BSD-3-Clause
//
// End-to-end integration test: a loaded boundary cut must produce
// a clean, finite SDDP solve and propagate to the policy estimate.
// Audit item F from `support/lp_audit_fix_plan_2026-04-29.md`.
//
// The earlier P0-1 / P2-2 / R4 tests verify loader correctness at the
// LP-row level (RHS, coefficients, class disambiguation, replay).
// This test closes the loop by running the full SDDPMethod::solve()
// pipeline with a boundary-cut file wired through `boundary_cuts_file`.
//
// Two invariants:
//
//   1. Baseline solve (no boundary cuts) and cut-loaded solve both
//      produce finite, positive lower + upper bounds — no crashes,
//      no NaN, no infinity propagation from a malformed cut row.
//
//   2. Loading a binding cut at the horizon shifts the SDDP **lower
//      bound** (the policy's view of future cost from each state).
//      A boundary cut on α at the last phase doesn't appear in the
//      forward-pass opex (which drives the UB) — instead the SDDP
//      backward pass folds the boundary cut into intermediate-phase
//      α, raising the LB that the policy would simulate.  If the
//      cut row is dropped before reaching the LP (the family of
//      bugs P0-1/P2-2/R4 each produced in their own way), the
//      cut-loaded LB equals the baseline LB.
//
// Uses the small `make_3phase_hydro_planning` fixture so each SDDP
// solve completes in <100ms.

#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_method.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE(  // NOLINT
    "load_boundary_cuts_csv — full SDDP solve with boundary-cut file")
{
  // ── Phase 1: baseline SDDP solve, no boundary cuts ────────────
  double baseline_ub = 0.0;
  double baseline_lb = 0.0;
  {
    auto planning = make_3phase_hydro_planning();
    planning.options.method = MethodType::sddp;
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 5;
    sddp_opts.convergence_tol = 1e-6;
    sddp_opts.enable_api = false;

    SDDPMethod sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    baseline_ub = results->back().upper_bound;
    baseline_lb = results->back().lower_bound;
    CAPTURE(baseline_ub);
    CAPTURE(baseline_lb);
    CHECK(std::isfinite(baseline_ub));
    CHECK(std::isfinite(baseline_lb));
    CHECK(baseline_ub > 0.0);
  }

  // ── Phase 2: re-solve with a binding boundary cut ─────────────
  // Pick phys_rhs well above baseline UB so the cut binds at the
  // horizon and propagates back through the SDDP backward pass to
  // raise the policy LB.  10× baseline + 1000 is comfortably
  // discriminative: a no-op load (P0-1/P2-2/R4 family of bugs)
  // leaves baseline_lb unchanged; a working load shifts it.
  const double phys_rhs = (10.0 * baseline_ub) + 1000.0;
  CAPTURE(phys_rhs);

  const auto cuts_file = (std::filesystem::temp_directory_path()
                          / "gtopt_test_bdr_solve_effect.csv")
                             .string();
  {
    std::ofstream ofs(cuts_file);
    // Bare reservoir name "rsv1" maps via name_to_class_uid to
    // ("Reservoir", 1) post-P2-2; class-aware inner match resolves
    // to Reservoir:1:efin.  Coefficient is zero so the cut is purely
    // α >= phys_rhs at the horizon, independent of state.
    ofs << "name,iteration,scene,rhs,rsv1\n";
    ofs << "force_alpha,1,1," << phys_rhs << ",0.0\n";
  }

  double cut_ub = 0.0;
  double cut_lb = 0.0;
  {
    auto planning = make_3phase_hydro_planning();
    planning.options.method = MethodType::sddp;
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 5;
    sddp_opts.convergence_tol = 1e-6;
    sddp_opts.enable_api = false;
    // Wire the boundary-cut file through the SDDP solve loop.
    // SDDPMethod calls `load_boundary_cuts_csv` when
    // `boundary_cuts_file` is set and mode != noload.
    sddp_opts.boundary_cuts_file = cuts_file;
    sddp_opts.boundary_cuts_mode = BoundaryCutsMode::combined;

    SDDPMethod sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    cut_ub = results->back().upper_bound;
    cut_lb = results->back().lower_bound;
    CAPTURE(cut_ub);
    CAPTURE(cut_lb);
    // Invariant 1: clean solve — no NaN, no infinities propagated
    // from a malformed cut row.
    CHECK(std::isfinite(cut_ub));
    CHECK(std::isfinite(cut_lb));
    CHECK(cut_ub > 0.0);
  }

  // ── Invariant 2: the cut shifts the LB ───────────────────────
  // The boundary cut at the horizon constrains the future-cost
  // function and propagates back through the SDDP backward pass to
  // raise the lower bound.  With phys_rhs = 10× baseline_ub a
  // working load produces cut_lb >> baseline_lb; a silently dropped
  // cut leaves cut_lb ≈ baseline_lb.  The threshold of 0.5 × phys_rhs
  // is comfortably above any numerical noise while staying loose
  // enough to absorb solver-specific propagation behaviour.
  CHECK(cut_lb > baseline_lb);
  CHECK(cut_lb > 0.5 * phys_rhs);

  std::filesystem::remove(cuts_file);
}
