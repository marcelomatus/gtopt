// SPDX-License-Identifier: BSD-3-Clause
//
// `boundary_cuts_mode = phi_expectation` — the PLP-literal terminal FCF.
//
// PLP's last-stage LP carries NVarPhi φ_j columns, one per PLANE
// HYDROLOGY j of plpplem2.dat, each priced 1/NVarPhi, each bounded
// below by the cuts of ITS hydrology only (`leeplaem.f`,
// `defprbpd.f`).  Terminal FCF = (1/NVarPhi)·Σ_j max_cuts_j(V_end)
// — the expectation over plane hydrologies of per-hydrology maxima,
// independent of the run's scene count.
//
// Fixture: 2 scenes (probabilities 0.7 / 0.3) and NVarPhi = 3 plane
// hydrologies whose CSV `scene` uids (5, 7, 9) deliberately match NO
// gtopt scene uid — under `separated`/`multicut` every cut would be
// dropped; `phi_expectation` must key them as plane indices instead.
// Every cut is state-INDEPENDENT (zero rsv1 coefficient), so
//
//     Φ = (1/3) Σ_j max_j = (1800 + 900 + 2400) / 3 = 1700
//
// exactly, at any terminal volume, and the dispatch argmin is
// untouched (α columns couple to no state), giving closed-form pins:
//
//   * per-scene UB:  UB_s(phi) − UB_s(base) = p_s · Φ — the forward
//     pass must NOT strip the terminal φ term (PLP ZSPFAdd
//     semantics) and each φ_j must be priced p_s/NVarPhi so the
//     scene probability composes exactly once;
//   * total UB:      UB(phi) − UB(base) = Φ   (Σ p_s = 1);
//   * total LB:      LB(phi) − LB(base) = Φ — the backward pass
//     folds the terminal expectation into the master through the α
//     recursion, so both bounds carry the SAME terminal term.
//
// NVarPhi (3) > scene count (2) also exercises the α-registry walk
// beyond `scene_count()` (the pre-fix cap would leave φ_2's bootstrap
// pin `[0,0]` in place, dropping plane 9 from the expectation — the
// per-scene pin below would then read p_s·900 instead of p_s·1700).
//
// See docs/formulation/sddp-cut-validity.md §9 "phi_expectation".

#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_method.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;

namespace
{
struct SolveOutcome
{
  double ub {};
  double lb {};
  std::vector<double> scene_ubs;
};

SolveOutcome run_sddp(const std::string& cuts_file, BoundaryCutsMode mode)
{
  auto planning = make_2scene_3phase_hydro_planning(/*prob1=*/0.7,
                                                    /*prob2=*/0.3);
  planning.options.method = MethodType::sddp;
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 6;
  sddp_opts.convergence_tol = 1e-8;
  sddp_opts.enable_api = false;
  if (!cuts_file.empty()) {
    sddp_opts.boundary_cuts_file = cuts_file;
    sddp_opts.boundary_cuts_mode = mode;
  }

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());
  return SolveOutcome {
      .ub = results->back().upper_bound,
      .lb = results->back().lower_bound,
      .scene_ubs = results->back().scene_upper_bounds,
  };
}
}  // namespace

TEST_CASE(  // NOLINT
    "boundary_cuts_mode=phi_expectation — E_j[max_j] terminal FCF in UB & LB")
{
  // ── Plane-hydrology cut file: NVarPhi = 3, 2 cuts per plane ────
  // Per-plane maxima: plane 5 → 1800, plane 7 → 900, plane 9 → 2400.
  const double expected_fcf = (1800.0 + 900.0 + 2400.0) / 3.0;  // 1700

  const auto cuts_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_bdr_phi_exp.csv")
          .string();
  {
    std::ofstream ofs(cuts_file);
    ofs << "iteration,scene,rhs,rsv1\n";
    ofs << "1,5,600.0,0.0\n";
    ofs << "2,5,1800.0,0.0\n";
    ofs << "1,7,900.0,0.0\n";
    ofs << "2,7,300.0,0.0\n";
    ofs << "1,9,2400.0,0.0\n";
    ofs << "2,9,1200.0,0.0\n";
  }

  // The pre-scan the solver uses to size the terminal φ layout.
  CHECK(boundary_cut_scene_count(cuts_file) == 3);

  // ── Baseline (no boundary cuts) vs phi_expectation ─────────────
  const auto base = run_sddp("", BoundaryCutsMode::separated);
  const auto phi = run_sddp(cuts_file, BoundaryCutsMode::phi_expectation);

  CHECK(std::isfinite(base.ub));
  CHECK(std::isfinite(phi.ub));
  REQUIRE(base.scene_ubs.size() == 2);
  REQUIRE(phi.scene_ubs.size() == 2);

  // Per-scene UB pin: the terminal future term rides the UB at weight
  // p_s (the p_s/NVarPhi column price × the NVarPhi per-plane maxima).
  // The state-independent cuts leave the dispatch untouched, so the
  // difference is EXACTLY p_s·Φ.
  CHECK(phi.scene_ubs[0] - base.scene_ubs[0]
        == doctest::Approx(0.7 * expected_fcf).epsilon(1e-6));
  CHECK(phi.scene_ubs[1] - base.scene_ubs[1]
        == doctest::Approx(0.3 * expected_fcf).epsilon(1e-6));

  // Total UB pin: Σ_s p_s·Φ = Φ — the UB accounting carries the full
  // expectation over plane hydrologies (PLP ZSPF / ZSPFAdd).
  CHECK(phi.ub - base.ub == doctest::Approx(expected_fcf).epsilon(1e-6));

  // LB pin: the backward pass propagates the same terminal expectation
  // into the phase-0 master, so LB rises by the same Φ — UB and LB
  // price one objective and the gap stays meaningful.
  CHECK(phi.lb - base.lb == doctest::Approx(expected_fcf).epsilon(1e-6));

  std::filesystem::remove(cuts_file);
}

TEST_CASE(  // NOLINT
    "phi_expectation rejects boundary_cut_sharing=multicut")
{
  const auto cuts_file = (std::filesystem::temp_directory_path()
                          / "gtopt_test_bdr_phi_exp_reject.csv")
                             .string();
  {
    std::ofstream ofs(cuts_file);
    ofs << "iteration,scene,rhs,rsv1\n";
    ofs << "1,1,100.0,0.0\n";
  }

  auto planning = make_2scene_3phase_hydro_planning();
  planning.options.method = MethodType::sddp;
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;
  sddp_opts.enable_api = false;
  sddp_opts.boundary_cuts_file = cuts_file;
  sddp_opts.boundary_cuts_mode = BoundaryCutsMode::phi_expectation;
  sddp_opts.boundary_cut_sharing = BoundaryCutSharingMode::multicut;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  CHECK_FALSE(results.has_value());

  std::filesystem::remove(cuts_file);
}
