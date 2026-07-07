// SPDX-License-Identifier: BSD-3-Clause
//
// Boundary cuts must work with VolumeRight state variables directly — they
// are Tilmant "dummy reservoir" SDDP couplers (default
// `use_state_variable = true`) and register the same storage `efin` state
// variable as Reservoir, so a cut column `VolumeRight:<name>` has to resolve
// through `Simulation::state_variables(...)` exactly like `Reservoir:<name>`.
//
// VolumeRights (including the irrigation *economy accumulators*, which are
// resettable VolumeRights) carry NO `efin` target today, so the soft-efin
// cost derivation skips them; this test pins the part that must already
// work: the FCF cut coefficient lands on the VolumeRight's state-var column.
// Extending the VolumeRight interface with reservoir-style targets is
// tracked separately (see the GitHub issue referenced in the commit).
//
// Mirrors test_sddp_boundary_cuts_reservoir_name_map.cpp, swapping the
// Reservoir column for a VolumeRight column.

#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_types.hpp>  // register_alpha_variables

#include "sddp_helpers.hpp"

using namespace gtopt;
// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_CASE(  // NOLINT
    "load_boundary_cuts_csv — VolumeRight state var resolves via "
    "name_to_class_uid")
{
  // 3-phase hydro fixture + a VolumeRight that opts into the SDDP state
  // chain (use_state_variable=true ⇒ registers an `efin` state var at the
  // last phase, class "VolumeRight", uid=1).
  auto planning = make_3phase_hydro_planning();
  planning.system.volume_right_array = {
      VolumeRight {
          .uid = Uid {1},
          .name = "vr1",
          .emax = 500.0,
          .eini = 0.0,
          .use_state_variable = OptBool {true},
      },
  };
  planning.options.sddp_options.low_memory_mode = LowMemoryMode::off;
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_bdr_volume_right.csv")
                            .string();

  constexpr double phys_rhs = 42.0;
  constexpr double phys_coeff = 7.0;
  {
    std::ofstream ofs(tmp_file);
    // Class-prefixed header — resolves via name_to_class_uid["vr1"] →
    // ("VolumeRight", 1) → the storage `efin` state-var column.
    ofs << "iteration,scene,rhs,VolumeRight:vr1\n";
    ofs << "1,0," << phys_rhs << "," << phys_coeff << "\n";
  }

  register_alpha_variables(planning_lp, first_scene_index(), 1.0);

  const auto& sim = planning_lp.simulation();
  const auto last_phase = sim.last_phase_index();
  auto& last_li =
      planning_lp.system(first_scene_index(), last_phase).linear_interface();

  // Locate the VolumeRight's `efin` state-var column.
  const auto& svar_map = sim.state_variables(first_scene_index(), last_phase);
  std::optional<ColIndex> vr_col;
  for (const auto& [key, svar] : svar_map) {
    if (key.class_name == std::string_view {"VolumeRight"} && key.uid == Uid {1}
        && key.col_name == std::string_view {"efin"})
    {
      vr_col = svar.col();
      break;
    }
  }
  REQUIRE(vr_col.has_value());

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;
  opts.missing_cut_var_mode = MissingCutVarMode::skip_cut;

  const LabelMaker label_maker {LpNamesLevel::none};
  auto states = make_default_scene_phase_states(planning_lp);

  const auto pre_load_numrows = last_li.get_numrows();

  const auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());

  // The VolumeRight column resolved → the cut row was installed (skip_cut
  // would drop it if the lookup missed).
  CHECK(result->count == 1);
  CHECK(last_li.get_numrows() == pre_load_numrows + 1);

  // The physical coefficient lands on the VolumeRight's state-var column
  // as -coeff (row form: α + Σ -coeff·x >= rhs).
  const auto cut_row = RowIndex {static_cast<Index>(pre_load_numrows)};
  const double phys_coeff_in_lp = last_li.get_coeff(cut_row, *vr_col);
  CAPTURE(phys_coeff_in_lp);
  CHECK(phys_coeff_in_lp
        == doctest::Approx(-phys_coeff)
               .epsilon(PlanningOptionsLP::default_sddp_cut_coeff_eps));

  std::filesystem::remove(tmp_file);
}

// NOLINTEND(bugprone-unchecked-optional-access)
