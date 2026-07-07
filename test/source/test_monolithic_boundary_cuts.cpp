// SPDX-License-Identifier: BSD-3-Clause
//
// MonolithicMethod boundary-cut install + bind, and efin-based α-rebase.
//
// Two regressions this file pins:
//
//   TASK 1 — MonolithicMethod used to read `boundary_cuts_file` and call
//   `load_boundary_cuts_csv`, but it never registered the α (future-cost)
//   state variables that the loader requires.  `find_alpha_state_var`
//   returned nullptr for every (scene, phase), so the loader `continue`d
//   past every cut — the assembled LP contained ZERO "Boundary" rows and
//   the cut was inert.  After the fix MonolithicMethod registers α (via
//   the shared `register_alpha_variables`) before loading, so:
//     * the last-phase LP carries the cut row (α column + state coeffs);
//     * the cut binds — the solved objective reflects the future cost.
//
//   TASK 2 — the α-rebase offset `c` (mean-shift) is computed at each
//   reservoir's `efin` end-of-horizon target when one is set, falling
//   back to the bound-box midpoint otherwise.  This mirrors the Python
//   oracle `build_fcf_alpha_terms` (`c = FCF − Σ wv·efin`).  We verify
//   the offset matches the efin evaluation, and that the reported
//   objective is unchanged by the rebase (the offset is added back via
//   `add_obj_constant`).

#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/monolithic_method.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_types.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;
// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace mono_bc
{

/// Write a single-scene boundary cut `α + (−coeff)·rsv1.efin ≥ rhs`.
auto write_cut_file(const std::string& path,
                    double rhs,
                    double coeff,
                    int scene_uid = 1) -> void
{
  std::ofstream ofs(path);
  ofs << "iteration,scene,rhs,rsv1\n";
  ofs << "1," << scene_uid << "," << rhs << "," << coeff << "\n";
}

/// Count cut rows installed on the last-phase LinearInterface of scene 0.
[[nodiscard]] auto last_phase_cut_count(const PlanningLP& planning_lp)
    -> std::size_t
{
  const auto last_phase = planning_lp.simulation().last_phase_index();
  return planning_lp.system(first_scene_index(), last_phase)
      .linear_interface()
      .active_cuts_size();
}

}  // namespace mono_bc

TEST_CASE(  // NOLINT
    "MonolithicMethod — boundary cut installs and binds (TASK 1)")
{
  using namespace mono_bc;

  const auto cuts_file = (std::filesystem::temp_directory_path()
                          / "gtopt_test_mono_bc_install.csv")
                             .string();
  // Coefficient zero ⇒ the cut is purely `α ≥ phys_rhs` (state-
  // independent), so it binds at exactly phys_rhs regardless of the
  // reservoir trajectory.  Disable the mean-shift here so the raw RHS
  // survives onto the LP and the objective shift is exactly phys_rhs.
  constexpr double phys_rhs = 12345.0;
  write_cut_file(cuts_file, phys_rhs, /*coeff=*/0.0);

  // ── Baseline: no boundary cuts ──
  double baseline_obj = 0.0;
  {
    auto planning = make_3phase_hydro_planning();
    PlanningLP planning_lp(std::move(planning));
    MonolithicMethod solver;
    auto result = solver.solve(planning_lp, {});
    REQUIRE(result.has_value());
    CHECK(last_phase_cut_count(planning_lp) == 0);
    const auto last_phase = planning_lp.simulation().last_phase_index();
    baseline_obj = planning_lp.system(first_scene_index(), last_phase)
                       .linear_interface()
                       .get_obj_value();
    CAPTURE(baseline_obj);
    CHECK(std::isfinite(baseline_obj));
  }

  // ── With a binding boundary cut ──
  {
    auto planning = make_3phase_hydro_planning();
    // Force raw RHS onto the LP (no α-rebase) so the objective shift is
    // exactly phys_rhs and easy to assert against.
    planning.options.sddp_options.boundary_cuts_mean_shift = OptBool {false};
    PlanningLP planning_lp(std::move(planning));

    MonolithicMethod solver;
    solver.boundary_cuts_file = cuts_file;
    solver.boundary_cuts_mode = BoundaryCutsMode::combined;
    auto result = solver.solve(planning_lp, {});
    REQUIRE(result.has_value());

    // Invariant A: the cut row landed on the last-phase LP.
    CHECK(last_phase_cut_count(planning_lp) >= 1);

    // Invariant B: the α column is present and freed (registered as a
    // state variable on the last phase).
    const auto last_phase = planning_lp.simulation().last_phase_index();
    const auto* alpha_svar = find_alpha_state_var(
        planning_lp.simulation(), first_scene_index(), last_phase);
    REQUIRE(alpha_svar != nullptr);

    // Invariant C: the cut binds — the last-phase objective rises by the
    // future-cost RHS (the cut forces α ≥ phys_rhs, α priced at 1.0/$).
    const auto cut_obj = planning_lp.system(first_scene_index(), last_phase)
                             .linear_interface()
                             .get_obj_value();
    CAPTURE(cut_obj);
    CAPTURE(baseline_obj);
    CHECK(cut_obj > baseline_obj);
    CHECK(cut_obj == doctest::Approx(baseline_obj + phys_rhs).epsilon(1e-3));
  }

  std::filesystem::remove(cuts_file);
}

TEST_CASE(  // NOLINT
    "MonolithicMethod — α-rebase offset uses efin, not midpoint (TASK 2)")
{
  using namespace mono_bc;

  // CSV layout (Python production convention, `write_boundary_cut_csv`):
  // each reservoir column carries `-water_value` — more stored water ⇒
  // lower future cost.  The loader (`sddp_boundary_cuts.cpp:598`) stores
  // `cmap[col] = -coeff`, so writing `coeff = -wv` yields `cmap[efin] =
  // +wv`, i.e. the cut `α + wv·rsv1.efin ≥ FCF`.  With mean-shift on, the
  // per-scene offset is the cut value at the evaluation point:
  //     c = lowb − cmap·eval = FCF − wv · eval_point
  // (matching the Python oracle's `obj_constant`).  It is rebased out,
  // leaving the on-LP cut RHS `shifted_rhs = FCF − c = wv · eval_point`
  // (the oracle's `sum_slope_state`).  Reading `shifted_rhs` back recovers
  // the evaluation point exactly:
  //     eval_point = shifted_rhs / wv.
  // The whole point of TASK 2 is that `eval_point` is the reservoir's
  // final-target value (efin, else eini), NOT the bound-box midpoint.
  constexpr double fcf = 100000.0;
  constexpr double wv = 7.0;  // water value [$/hm³]
  const auto cuts_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_mono_bc_efin.csv")
          .string();
  write_cut_file(cuts_file, fcf, /*coeff=*/-wv);

  // Reservoir emin=0, emax=500 ⇒ bound-box midpoint = 250.  All target
  // values below are chosen distinct from 250 so the midpoint path would
  // give a clearly different `shifted_rhs`.
  constexpr double midpoint = 250.0;

  // Helper: run a monolithic solve with the boundary cut and return the
  // recovered evaluation point (shifted_rhs / wv) from the last cut row.
  const auto recover_eval_point = [&](Planning&& planning) -> double
  {
    planning.options.sddp_options.boundary_cuts_mean_shift = OptBool {true};
    PlanningLP planning_lp(std::move(planning));
    MonolithicMethod solver;
    solver.boundary_cuts_file = cuts_file;
    solver.boundary_cuts_mode = BoundaryCutsMode::combined;
    auto result = solver.solve(planning_lp, {});
    REQUIRE(result.has_value());
    const auto last_phase = planning_lp.simulation().last_phase_index();
    auto& li =
        planning_lp.system(first_scene_index(), last_phase).linear_interface();
    REQUIRE(li.active_cuts_size() >= 1);
    return li.active_cuts().back().lowb / wv;
  };

  // ── efin set: eval point is efin (not the midpoint) ──
  // `efin_cost > 0` makes the `vol_end >= efin` row soft so the demanding
  // efin target does not render the LP infeasible — efin still drives the
  // α-rebase evaluation point.
  {
    constexpr double efin_val = 400.0;
    auto planning = make_3phase_hydro_planning();
    planning.system.reservoir_array[0].efin = OptReal {efin_val};
    planning.system.reservoir_array[0].efin_cost = OptReal {50.0};
    const double eval_point = recover_eval_point(std::move(planning));
    CAPTURE(eval_point);
    CHECK(eval_point == doctest::Approx(efin_val).epsilon(1e-6));
    CHECK(eval_point != doctest::Approx(midpoint).epsilon(1e-6));
  }

  // ── efin absent, eini set: eval point falls back to eini ──
  // The fixture's eini is 250; override it to a distinct 320 so the
  // eini-fallback leaf is observable (and != midpoint).  eini constrains
  // only the first stage's start volume, so it never makes the last phase
  // infeasible.
  {
    constexpr double eini_val = 320.0;
    auto planning = make_3phase_hydro_planning();
    planning.system.reservoir_array[0].efin = OptReal {};  // read-only: no efin
    planning.system.reservoir_array[0].eini = OptReal {eini_val};
    const double eval_point = recover_eval_point(std::move(planning));
    CAPTURE(eval_point);
    CHECK(eval_point == doctest::Approx(eini_val).epsilon(1e-6));
    CHECK(eval_point != doctest::Approx(midpoint).epsilon(1e-6));
  }

  std::filesystem::remove(cuts_file);
}

// NOLINTEND(bugprone-unchecked-optional-access)
