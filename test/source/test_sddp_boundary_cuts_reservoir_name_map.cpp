// SPDX-License-Identifier: BSD-3-Clause
//
// Regression test for the missing-`reservoir_array` entry in
// `sddp_boundary_cuts.cpp`'s `name_to_class_uid` map.  The audit at
// `support/lp_audit_fix_plan_2026-04-29.md` (P2-2) noted that the
// element-name → (class, uid) lookup was built from `junction_array`
// and `battery_array` only, silently dropping every PLP-sourced
// boundary cut whose state variables sit on hydro reservoirs — the
// dominant CEN case.
//
// This test pins the contract:
//
//   * Header references a reservoir by ``Reservoir:<name>``.
//   * ``MissingCutVarMode::skip_cut`` makes a missing lookup
//     observable as a dropped cut (cuts_loaded -> 0).
//   * After the fix (reservoir_array added to the lookup map),
//     ``cuts_loaded == 1``.
//
// The 3-phase fixture has a Junction "j_up" (uid=1) and a Reservoir
// "rsv1" (uid=1).  The two share a uid intentionally — the lookup
// must use the class prefix on the header to disambiguate, otherwise
// a bare "rsv1" miss is a false discriminator (it's missing because
// the *name* is absent, not because the *class* is absent).
//
// The test uses ``Reservoir:rsv1`` as the header so the class
// prefix forces an exact lookup; that misses today, succeeds after
// the fix.

#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

[[nodiscard]] auto make_scene_phase_states_reservoir_map(
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
    "load_boundary_cuts_csv — reservoir state variables resolve via "
    "name_to_class_uid")
{
  // Fixture: 3-phase hydro with Reservoir{uid=1, name=\"rsv1\"} and
  // Junction{uid=1, name=\"j_up\"}.  Both register efin state vars at
  // the last phase (uid=1 each, distinct class).
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_bdr_reservoir_name_map.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    // Header column "Reservoir:rsv1" — class-prefixed so the lookup
    // is unambiguous about which class we're asking for.  Pre-fix:
    // name_to_class_uid only has Junction + Battery entries, so the
    // ``rsv1`` name lookup MISSES regardless of the class prefix.
    // Post-fix: the entry exists, the prefix matches the stored
    // class, and the cut loads cleanly.
    ofs << "name,iteration,scene,rhs,Reservoir:rsv1\n";
    ofs << "rsv_cut,1,0,42.0,5.0\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;
  // skip_cut makes a missing state-var lookup with nonzero coefficient
  // observable as a dropped row (count == 0).  Pre-fix: count == 0;
  // post-fix: count == 1.  This is the smallest discriminator for the
  // bug — under the default skip_coeff mode the loader silently zeros
  // the coefficient and the cut still adds (count == 1 either way).
  opts.missing_cut_var_mode = MissingCutVarMode::skip_cut;

  const LabelMaker label_maker {LpNamesLevel::none};
  auto states = make_scene_phase_states_reservoir_map(planning_lp);

  auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);

  REQUIRE(result.has_value());
  // Post-fix: the reservoir is found in name_to_class_uid → cut row
  // assembled → count == 1.  Pre-fix: lookup miss → row treated as
  // missing → skip_cut path increments cuts_skipped, leaving
  // cuts_loaded at 0 → count == 0.
  CHECK(result->count == 1);

  std::filesystem::remove(tmp_file);
}
