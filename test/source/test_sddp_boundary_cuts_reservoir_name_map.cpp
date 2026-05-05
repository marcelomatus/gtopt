// SPDX-License-Identifier: BSD-3-Clause
//
// Regression tests for the missing-`reservoir_array` entry in
// `sddp_boundary_cuts.cpp`'s `name_to_class_uid` map.  The audit at
// `docs/analysis/investigations/linear_interface/lp_audit_fix_plan_2026-04-29.md`
// (P2-2) noted that the element-name → (class, uid) lookup was built from
// `junction_array` and `battery_array` only, silently dropping every
// PLP-sourced boundary cut whose state variables sit on hydro reservoirs — the
// dominant CEN case.
//
// Two subcases pin the contract under both
// `MissingCutVarMode::skip_cut` (cut row skipped) and
// `MissingCutVarMode::skip_coeff` (cut row loaded with the missing
// coefficient zeroed) — the production default is `skip_coeff`, so
// covering both modes is necessary to catch the silent-drop variant.
//
// Each subcase verifies the post-fix behaviour by reading back the
// physical coefficient at the reservoir's LP column via
// `LinearInterface::get_coeff(row, col)`.  Pre-fix the coefficient
// is zero (skip_coeff path) or the row is absent (skip_cut path);
// post-fix the coefficient lands at the requested physical value
// modulo `cut_coeff_eps`.

#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_types.hpp>  // register_alpha_variables

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE(  // NOLINT
    "load_boundary_cuts_csv — reservoir state vars resolve via "
    "name_to_class_uid")
{
  // Fixture: 3-phase hydro with Reservoir{uid=1, name="rsv1"} and
  // Junction{uid=1, name="j_up"}.  Both register efin state vars at
  // the last phase (uid=1 each, distinct class).  Force eager backend
  // so coefficient readback via get_coeff doesn't race with low-memory
  // release/reload.
  auto planning = make_3phase_hydro_planning();
  planning.options.sddp_options.low_memory_mode = LowMemoryMode::off;
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_bdr_reservoir_name_map.csv")
                            .string();

  // Class-prefixed header — the lookup must walk through
  // name_to_class_uid["rsv1"] which pre-fix doesn't exist.  Post-fix
  // it does and resolves to ("Reservoir", 1).
  constexpr double phys_rhs = 42.0;
  constexpr double phys_coeff = 5.0;
  {
    std::ofstream ofs(tmp_file);
    ofs << "name,iteration,scene,rhs,Reservoir:rsv1\n";
    ofs << "rsv_cut,1,0," << phys_rhs << "," << phys_coeff << "\n";
  }

  // α has to be pre-registered (load_boundary_cuts_csv won't add α
  // itself; production drives this from
  // SDDPMethod::initialize_alpha_variables).
  register_alpha_variables(planning_lp, first_scene_index(), 1.0);

  const auto& sim = planning_lp.simulation();
  const auto last_phase = sim.last_phase_index();
  auto& last_li =
      planning_lp.system(first_scene_index(), last_phase).linear_interface();

  // Locate the reservoir's LP column once via the registered state
  // variables — same lookup pattern the loader itself uses, but here
  // we resolve by `(class_name, uid, col_name=='efin')` so the rsv1
  // state-var column is unambiguous regardless of which order
  // svar_map iterates.
  const auto& svar_map = sim.state_variables(first_scene_index(), last_phase);
  std::optional<ColIndex> rsv_col;
  for (const auto& [key, svar] : svar_map) {
    if (key.class_name == std::string_view {"Reservoir"} && key.uid == Uid {1}
        && key.col_name == std::string_view {"efin"})
    {
      rsv_col = svar.col();
      break;
    }
  }
  REQUIRE(rsv_col.has_value());

  SUBCASE("skip_cut — pre-fix drops the row entirely")
  {
    SDDPOptions opts;
    opts.boundary_cuts_mode = BoundaryCutsMode::separated;
    opts.missing_cut_var_mode = MissingCutVarMode::skip_cut;

    const LabelMaker label_maker {LpNamesLevel::none};
    auto states = make_default_scene_phase_states(planning_lp);

    const auto pre_load_numrows = last_li.get_numrows();

    auto result = load_boundary_cuts_csv(
        planning_lp, tmp_file, opts, label_maker, states);

    REQUIRE(result.has_value());
    // Post-fix: cut loaded.  Pre-fix: skip_cut path drops the row,
    // count == 0.
    CHECK(result->count == 1);
    CHECK(last_li.get_numrows() == pre_load_numrows + 1);

    const auto cut_row = RowIndex {static_cast<Index>(pre_load_numrows)};
    // Physical coefficient on the reservoir column should be -coeff
    // (the cut is α >= rhs - Σ coeff×x, encoded as α + Σ -coeff×x >=
    // rhs in row form).
    const double phys_coeff_in_lp = last_li.get_coeff(cut_row, *rsv_col);
    CAPTURE(phys_coeff_in_lp);
    CHECK(phys_coeff_in_lp
          == doctest::Approx(-phys_coeff)
                 .epsilon(PlanningOptionsLP::default_sddp_cut_coeff_eps));
  }

  SUBCASE("skip_coeff — pre-fix loads the row but silently zeros the coeff")
  {
    SDDPOptions opts;
    opts.boundary_cuts_mode = BoundaryCutsMode::separated;
    opts.missing_cut_var_mode = MissingCutVarMode::skip_coeff;  // default

    const LabelMaker label_maker {LpNamesLevel::none};
    auto states = make_default_scene_phase_states(planning_lp);

    const auto pre_load_numrows = last_li.get_numrows();

    auto result = load_boundary_cuts_csv(
        planning_lp, tmp_file, opts, label_maker, states);

    REQUIRE(result.has_value());
    // Both pre- and post-fix the row gets added under skip_coeff
    // (count == 1).  The pre-fix bug is silent: the missing-name
    // lookup zeros the coefficient on the reservoir column, while
    // the row's RHS still carries the physical value.  Reading the
    // matrix coefficient is the only way to discriminate.
    CHECK(result->count == 1);
    CHECK(last_li.get_numrows() == pre_load_numrows + 1);

    const auto cut_row = RowIndex {static_cast<Index>(pre_load_numrows)};
    const double phys_coeff_in_lp = last_li.get_coeff(cut_row, *rsv_col);
    CAPTURE(phys_coeff_in_lp);
    // Post-fix: -phys_coeff.  Pre-fix: 0.0 (silent drop).
    CHECK(phys_coeff_in_lp
          == doctest::Approx(-phys_coeff)
                 .epsilon(PlanningOptionsLP::default_sddp_cut_coeff_eps));
  }

  std::filesystem::remove(tmp_file);
}

// ─── R4 — class-aware inner svar_map match ─────────────────────────────────
//
// Pre-R4 the loader's inner svar_map iteration matched by `uid` only,
// so a CSV header that resolved to a class with no efin state
// variable (e.g. `Junction:j_up` — junctions don't carry efin) would
// silently fall through to the first svar with the same uid (the
// reservoir's, in the 3-phase fixture where j_up and rsv1 share
// uid=1).  That hid intent mismatches behind a coincidental
// uid collision.
//
// Post-R4 the inner loop also matches `key.class_name == cname`, so
// the resolution either lands on the requested-class state var or
// stays nullopt — no class substitution.
//
// The skip_cut sub-case below demonstrates the discriminator: a cut
// referencing `Junction:j_up` is now correctly skipped (Junction:1
// has no efin state var), whereas pre-fix it loaded with the
// reservoir's column substituted in.

TEST_CASE(  // NOLINT
    "load_boundary_cuts_csv — class-aware inner match: Junction:j_up "
    "(no efin state var) does not silently substitute Reservoir:1")
{
  auto planning = make_3phase_hydro_planning();
  planning.options.sddp_options.low_memory_mode = LowMemoryMode::off;
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_bdr_class_aware_match.csv")
                            .string();

  {
    std::ofstream ofs(tmp_file);
    // Junction:j_up — Junction class doesn't register efin state
    // variables in the 3-phase fixture (only Reservoir does).
    ofs << "name,iteration,scene,rhs,Junction:j_up\n";
    ofs << "junc_cut,1,0,42.0,5.0\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;
  // skip_cut surfaces the class-mismatch as count == 0 because the
  // Junction:j_up state-var lookup misses → has_missing == true →
  // skip_cut path.
  opts.missing_cut_var_mode = MissingCutVarMode::skip_cut;

  const LabelMaker label_maker {LpNamesLevel::none};
  auto states = make_default_scene_phase_states(planning_lp);

  register_alpha_variables(planning_lp, first_scene_index(), 1.0);

  const auto& sim = planning_lp.simulation();
  const auto last_phase = sim.last_phase_index();
  auto& last_li =
      planning_lp.system(first_scene_index(), last_phase).linear_interface();
  const auto pre_load_numrows = last_li.get_numrows();

  const auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());

  // Post-R4: count == 0 (no Junction:1:efin → skip_cut drops the row).
  // Pre-R4: count == 1 (silent substitution onto Reservoir:1).
  CHECK(result->count == 0);
  CHECK(last_li.get_numrows() == pre_load_numrows);

  std::filesystem::remove(tmp_file);
}
