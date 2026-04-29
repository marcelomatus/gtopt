// SPDX-License-Identifier: BSD-3-Clause
//
// Round-trip regression test for boundary-cut LP-row composition.
//
// Audit P0-1 from `support/lp_audit_fix_plan_2026-04-29.md`:
// `sddp_boundary_cuts.cpp:373,401` pre-divides the row's RHS and
// coefficients by `scale_objective` (and pre-multiplies the
// state-variable coefficient by `col_scale`).  The same row then
// flows through `LinearInterface::add_cut_row` →
// `LinearInterface::compose_physical`, which applies BOTH the
// `× col_scale` and the `÷ scale_objective` AGAIN (linear_interface.cpp
// step 1 + step 1b).  The audit predicted that
// `li.get_row_low(cut_row)` would return `phys_rhs / scale_objective`
// instead of the documented physical-space round-trip value
// (`compose_physical` block comment lines 1142-1146:
// `get_row_low[i] = raw_lb × composite_scale = phys_lb`).
//
// This test pins the round-trip contract:
//
//   * Load a boundary CSV with a known physical RHS (42.0).
//   * Build the planning with ``scale_objective = 1000`` so the
//     pre-divide is observable as a ×1000 mismatch.
//   * Read back ``li.get_row_low(cut_row)`` and assert it equals
//     ``phys_rhs`` within ``cut_coeff_eps`` (the SDDP default
//     coefficient-filter tolerance, also the right absolute floor for
//     LP-row reads since both are in physical $).
//
// If the audit's analysis is correct the assertion fails today
// (``0.042 != 42.0``) and graduates to passing once the
// boundary-cut assembly stops pre-scaling.  If it passes today, the
// test still has value as a regression pin on the documented
// round-trip invariant: any future re-introduction of the
// double-scaling bug would re-fail this test.
//
// Tolerance choice: ``cut_coeff_eps`` (1e-8 by default) is a
// physical-space absolute tolerance — the same tolerance that
// `compose_physical` uses to filter near-zero coefficients via
// ``to_flat(eps)``.  Reusing it here keeps the test's pass/fail
// boundary aligned with the production filter.

#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_types.hpp>  // register_alpha_variables

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

[[nodiscard]] auto make_scene_phase_states_round_trip(
    const PlanningLP& planning_lp)
    -> StrongIndexVector<SceneIndex,
                         StrongIndexVector<PhaseIndex, PhaseStateInfo>>
{
  const auto& sim = planning_lp.simulation();
  const auto num_scenes = static_cast<Index>(sim.scenes().size());
  const auto num_phases = static_cast<Index>(sim.phases().size());

  StrongIndexVector<SceneIndex, StrongIndexVector<PhaseIndex, PhaseStateInfo>>
      result(static_cast<std::size_t>(num_scenes),
             StrongIndexVector<PhaseIndex, PhaseStateInfo>(
                 static_cast<std::size_t>(num_phases), PhaseStateInfo {}));
  return result;
}

}  // namespace

TEST_CASE(  // NOLINT
    "load_boundary_cuts_csv — RHS round-trip survives scale_objective")
{
  // 3-phase hydro fixture, override scale_objective to a non-trivial
  // value that exposes any extra ÷scale_obj on the boundary-cut
  // assembly path.  Default fixture has scale_objective=1.0 (no
  // discrimination).  Use 1000.0 — the gtopt production default per
  // CLAUDE.md and the value that triggered the historical
  // juan/IPLP UB-runaway suspect (boundary cuts on
  // ``scale_obj=1000`` runs).
  auto planning = make_3phase_hydro_planning();
  planning.options.scale_objective = OptReal {1000.0};
  // Force eager-resident backend so add_cut_row writes are observable
  // through `get_row_low()` immediately, without a release/reload
  // cycle that could mask numrows readbacks.
  planning.options.sddp_options.low_memory_mode = LowMemoryMode::off;
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_bdr_round_trip.csv")
          .string();

  // Known physical inputs.  We pick a value not divisible by
  // scale_obj so the comparison can't accidentally pass under the
  // "÷1000" failure mode.
  constexpr double phys_rhs = 42.0;
  constexpr double phys_coeff = 5.0;

  {
    std::ofstream ofs(tmp_file);
    // Header includes a junction state variable (always resolvable
    // via name_to_class_uid) so the cut has at least one
    // non-α coefficient, ensuring the row goes through the
    // `compose_physical` path that the audit flagged.
    ofs << "name,iteration,scene,rhs,j_up\n";
    ofs << "rt_cut,1,0," << phys_rhs << "," << phys_coeff << "\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;
  opts.boundary_max_iterations = 0;  // no filtering

  const LabelMaker label_maker {LpNamesLevel::none};
  auto states = make_scene_phase_states_round_trip(planning_lp);

  // Establish the same α-pre-registration precondition that
  // SDDPMethod::initialize_alpha_variables would set up — without it,
  // load_boundary_cuts_csv finds no α state-var on the last phase
  // and silently skips the cut at sddp_boundary_cuts.cpp:362.  The
  // production cut path always runs after alpha registration, so
  // skipping it in a unit test would test a contradicted state.
  register_alpha_variables(planning_lp, first_scene_index(), 1.0);

  // Capture the pre-load row count so we can locate the new cut row
  // without depending on absolute row numbering.
  const auto& sim = planning_lp.simulation();
  const auto last_phase = sim.last_phase_index();
  auto& last_li =
      planning_lp.system(first_scene_index(), last_phase).linear_interface();
  const auto pre_load_numrows = last_li.get_numrows();

  const auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  REQUIRE(result->count == 1);

  const auto post_load_numrows = last_li.get_numrows();
  REQUIRE(post_load_numrows == pre_load_numrows + 1);
  const auto cut_row = pre_load_numrows;  // the row we just added

  // Round-trip invariant from `compose_physical` (linear_interface.cpp
  // lines 1142-1146):
  //   get_row_low[cut_row] = raw_lb × composite_scale = phys_lb
  //
  // Pre-fix prediction (audit P0-1): assembly pre-divides by
  // scale_obj, compose_physical divides by scale_obj again, so
  // raw_lb = phys_rhs / scale_obj² and composite_scale = scale_obj
  // (× sa, but that cancels), giving
  // get_row_low = phys_rhs / scale_obj.  With scale_obj = 1000 that
  // is 0.042 — far outside the cut_coeff_eps tolerance from 42.0.
  //
  // Post-fix: the assembly emits physical-space rows, compose_physical
  // applies col_scale + ÷scale_obj exactly once, and get_row_low
  // returns the original physical RHS to within numerical noise.
  const double row_low = last_li.get_row_low()[cut_row];
  CAPTURE(row_low);
  CAPTURE(phys_rhs);
  CHECK(row_low
        == doctest::Approx(phys_rhs).epsilon(
            PlanningOptionsLP::default_sddp_cut_coeff_eps));

  std::filesystem::remove(tmp_file);
}

TEST_CASE(  // NOLINT
    "load_boundary_cuts_csv — RHS round-trip is identity under "
    "scale_objective=1")
{
  // Sanity: with scale_objective=1.0 the double-scaling factor is
  // 1.0² = 1.0, so the round-trip succeeds even with the bug.  This
  // sub-case pins the no-scaling branch and discriminates "the bug
  // is the divisor" from "the loader is fundamentally broken".
  auto planning = make_3phase_hydro_planning();
  planning.options.scale_objective = OptReal {1.0};
  planning.options.sddp_options.low_memory_mode = LowMemoryMode::off;
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_bdr_round_trip_no_scale.csv")
                            .string();
  constexpr double phys_rhs = 42.0;
  constexpr double phys_coeff = 5.0;

  {
    std::ofstream ofs(tmp_file);
    ofs << "name,iteration,scene,rhs,j_up\n";
    ofs << "rt_cut,1,0," << phys_rhs << "," << phys_coeff << "\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;
  opts.boundary_max_iterations = 0;

  const LabelMaker label_maker {LpNamesLevel::none};
  auto states = make_scene_phase_states_round_trip(planning_lp);

  register_alpha_variables(planning_lp, first_scene_index(), 1.0);

  const auto& sim = planning_lp.simulation();
  const auto last_phase = sim.last_phase_index();
  auto& last_li =
      planning_lp.system(first_scene_index(), last_phase).linear_interface();
  const auto pre_load_numrows = last_li.get_numrows();

  const auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  REQUIRE(result->count == 1);
  REQUIRE(last_li.get_numrows() == pre_load_numrows + 1);

  const auto cut_row = pre_load_numrows;
  const double row_low = last_li.get_row_low()[cut_row];
  CAPTURE(row_low);
  CHECK(row_low
        == doctest::Approx(phys_rhs).epsilon(
            PlanningOptionsLP::default_sddp_cut_coeff_eps));

  std::filesystem::remove(tmp_file);
}
