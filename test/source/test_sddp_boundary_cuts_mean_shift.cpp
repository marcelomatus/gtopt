// SPDX-License-Identifier: BSD-3-Clause
//
// Opt-in α-rebase (mean-shift) on boundary cut load.
//
// When `SDDPOptions::boundary_cuts_mean_shift = true`, the loader
// shifts every boundary cut's RHS by the per-scene mean and folds the
// offset into `LinearInterface::add_obj_constant`.  Algebraically
// exact (α' = α − c̄), so the physical objective reported by
// `get_obj_value()` — and by extension SDDP's UB/LB — must match the
// unshifted formulation bit-for-bit.
//
// This test exercises the same scenario as
// `test_sddp_boundary_cuts_solve_effect.cpp` but with the shift
// enabled, and asserts both:
//   1. The shift binds the cut (LB rises above the baseline).
//   2. The reported LB matches the unshifted formulation, confirming
//      `obj_constant` carries the c̄ offset through the SDDP
//      machinery (forward pass, backward cuts, bound tracking).

#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_method.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;

TEST_CASE(  // NOLINT
    "SDDPOptions::boundary_cuts_mean_shift — LB matches unshifted formulation")
{
  // Single-cut scenario: c̄ = cut.rhs, so the shifted cut has
  // lowb = 0.  The full physical effect is carried entirely by the
  // obj_constant.

  // ── Phase 1: baseline SDDP solve, no boundary cuts ────────────
  double baseline_ub = 0.0;
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
    REQUIRE(std::isfinite(baseline_ub));
  }

  // ── Pick a large phys_rhs to make the cut clearly bind ───────
  const double phys_rhs = (10.0 * baseline_ub) + 1000.0;
  CAPTURE(phys_rhs);

  const auto cuts_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_bdr_mean_shift.csv")
          .string();
  {
    std::ofstream ofs(cuts_file);
    ofs << "iteration,scene,rhs,rsv1\n";
    ofs << "1,1," << phys_rhs << ",0.0\n";
  }

  // ── Phase 2: solve with shift OFF (control) ──────────────────
  double off_lb = 0.0;
  double off_ub = 0.0;
  {
    auto planning = make_3phase_hydro_planning();
    planning.options.method = MethodType::sddp;
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 5;
    sddp_opts.convergence_tol = 1e-6;
    sddp_opts.enable_api = false;
    sddp_opts.boundary_cuts_file = cuts_file;
    sddp_opts.boundary_cuts_mode = BoundaryCutsMode::combined;
    sddp_opts.boundary_cuts_mean_shift = false;  // explicit control

    SDDPMethod sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    off_ub = results->back().upper_bound;
    off_lb = results->back().lower_bound;
    REQUIRE(std::isfinite(off_ub));
    REQUIRE(std::isfinite(off_lb));
  }

  // ── Phase 3: solve with shift ON (system under test) ─────────
  double on_lb = 0.0;
  double on_ub = 0.0;
  double on_c_bar = 0.0;
  {
    auto planning = make_3phase_hydro_planning();
    planning.options.method = MethodType::sddp;
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 5;
    sddp_opts.convergence_tol = 1e-6;
    sddp_opts.enable_api = false;
    sddp_opts.boundary_cuts_file = cuts_file;
    sddp_opts.boundary_cuts_mode = BoundaryCutsMode::combined;
    sddp_opts.boundary_cuts_mean_shift = true;  // opt-in

    SDDPMethod sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    on_ub = results->back().upper_bound;
    on_lb = results->back().lower_bound;
    on_c_bar = sddp.scene_alpha_offset(SceneIndex {0});
    REQUIRE(std::isfinite(on_ub));
    REQUIRE(std::isfinite(on_lb));
  }

  // ── Invariants ───────────────────────────────────────────────
  // The whole reason for the mean-shift is to close the LB-overshoot
  // gap: under the unshifted formulation, the boundary cut raises LB
  // (cost-to-go bound) at the master LP but the forward-pass UB only
  // sums dispatch OPEX — so `LB > UB`, an artificial gap that never
  // converges.  Under the shift the SAME constant `c̄ × cost_factor`
  // is added to both ends, closing the gap.
  //
  // Sanity: control (off) really does bind the cut.  We check this
  // against off_lb (which includes the cut's α contribution from
  // first-phase obj_value), not off_ub (which is just dispatch opex).
  CHECK(off_lb > 0.5 * phys_rhs);  // off-path master LB sees the cut
  CHECK(on_lb > 0.5 * phys_rhs);  // shifted master LB also binds

  // Sharp invariant: the shift closes the gap on the on-side.
  // off_lb − off_ub  =  α_boundary  (the LB overshoot)
  // on_lb  − on_ub   ≈  0           (gap closed)
  // Empirically the converged simulation pass reports `gap=0.00%`
  // for the on-side; pin that as the strictest invariant.
  const double off_gap = off_lb - off_ub;  // strictly positive (overshoot)
  const double on_gap = on_lb - on_ub;  // ≈ 0 after the shift
  CAPTURE(off_gap);
  CAPTURE(on_gap);
  CHECK(off_gap > 0.1 * phys_rhs);  // overshoot is the original bug
  CHECK(std::abs(on_gap) < 1e-3 * phys_rhs);  // shift closes the gap

  // Symmetric side effect: on_ub ≥ off_ub because the on-side UB
  // picks up the c̄ × cost_factor offset (the off-side UB does not,
  // since `scene_alpha_offset = 0` when the shift is disabled).
  CHECK(on_ub >= off_ub);

  // Sharp reconciliation — the cleanest tripwire for the piece-2 FutureCostLP
  // migration: the on-side UB is exactly the off-side UB plus the per-scene c̄
  // the shift folds into the objective, and c̄ for this single coef-0 cut
  // equals phys_rhs at any state.  A migration that drops / double-applies the
  // obj_constant or the scene_alpha_offset trips one of these.
  CAPTURE(on_c_bar);
  CHECK(on_c_bar == doctest::Approx(phys_rhs).epsilon(1e-3));
  CHECK(on_ub == doctest::Approx(off_ub + on_c_bar).epsilon(1e-3));

  std::filesystem::remove(cuts_file);
}

TEST_CASE(  // NOLINT
    "boundary_cuts_mean_shift — c̄ equals cut value at midpoint state")
{
  // The shift `c̄_scene` is documented (see `source/sddp_boundary_cuts.cpp`)
  // as the per-scene mean of each cut's value AT THE MIDPOINT STATE.
  // For a single cut `α ≥ rhs + g × s_rsv1` with `g = 2.0` and
  // `rhs = 1000.0`, and the fixture's `rsv1` reservoir having
  // `emin = 0, emax = 500`, the state-variable midpoint is 250 and
  // the cut's value at midpoint is `1000 + 2.0 × 250 = 1500`.
  //
  // This test pins the c̄ formula so a future refactor (e.g. switching
  // to mean(rhs) or any other aggregation) trips the assertion.

  const auto cuts_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_bdr_c_bar.csv")
          .string();
  {
    // Cut row layout (CSV): `α ≥ rhs + Σ gᵢ × sᵢ`.  The coefficient
    // column for `rsv1` carries gᵢ; `rhs` is the intercept.  Sign
    // is preserved through the boundary-cut loader (no negation),
    // so what we write is what we expect to project against the
    // midpoint state.
    std::ofstream ofs(cuts_file);
    ofs << "iteration,scene,rhs,rsv1\n";
    ofs << "1,1,1000.0,2.0\n";
  }

  auto planning = make_3phase_hydro_planning();
  planning.options.method = MethodType::sddp;
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.convergence_tol = 1e-6;
  sddp_opts.enable_api = false;
  sddp_opts.boundary_cuts_file = cuts_file;
  sddp_opts.boundary_cuts_mode = BoundaryCutsMode::combined;
  sddp_opts.boundary_cuts_mean_shift = true;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  // Hand computation:
  //   rsv1 emin=0, emax=500  ⇒ midpoint = 250
  //   raw c̄ = rhs + g × midpoint = 1000 + 2.0 × 250 = 1500
  // (Equivalent in cmap form: cmap[s_rsv1] = −g = −2.0, and the
  //  implementation computes `cut.lowb − cmap[s_rsv1] × midpoint =
  //  1000 − (−2.0) × 250 = 1500`.)
  //
  // `scene_alpha_offset()` stores the raw c̄ unchanged.  α is
  // registered with `cost = 1.0` (physical $), so the obj_value
  // drop from the substitution α' = α − c̄ is exactly `−c̄` per
  // scene — no `cost_factor` multiplier is needed at the four
  // UB/LB display sites.  See the explanatory comment block in
  // `source/sddp_boundary_cuts.cpp` near `scene_c_bar[si] = c_bar`.
  const double expected_c_bar = 1500.0;

  CHECK(sddp.scene_alpha_offset(SceneIndex {0})
        == doctest::Approx(expected_c_bar).epsilon(1e-6));

  std::filesystem::remove(cuts_file);
}

TEST_CASE(  // NOLINT
    "boundary_cuts_mean_shift — c̄ = 0 when the option is disabled")
{
  // Sanity check: with the option OFF, `scene_alpha_offset()` must
  // return 0 even when boundary cuts are present.  Guards against a
  // future refactor that accidentally applies the shift
  // unconditionally.
  const auto cuts_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_bdr_c_bar_off.csv")
          .string();
  {
    std::ofstream ofs(cuts_file);
    ofs << "iteration,scene,rhs,rsv1\n";
    ofs << "1,1,1000.0,2.0\n";
  }

  auto planning = make_3phase_hydro_planning();
  planning.options.method = MethodType::sddp;
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.convergence_tol = 1e-6;
  sddp_opts.enable_api = false;
  sddp_opts.boundary_cuts_file = cuts_file;
  sddp_opts.boundary_cuts_mode = BoundaryCutsMode::combined;
  sddp_opts.boundary_cuts_mean_shift = false;  // explicit OFF

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  CHECK(sddp.scene_alpha_offset(SceneIndex {0}) == 0.0);

  std::filesystem::remove(cuts_file);
}
