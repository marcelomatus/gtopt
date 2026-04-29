// SPDX-License-Identifier: BSD-3-Clause
//
// Round-trip regression test for boundary-cut LP-row composition.
//
// Audit P0-1 from `support/lp_audit_fix_plan_2026-04-29.md`:
// `sddp_boundary_cuts.cpp` was pre-dividing the row's RHS and
// coefficients by `scale_objective` (and pre-multiplying the
// state-variable coefficient by `col_scale`).  The same row then
// flowed through `LinearInterface::add_cut_row` →
// `LinearInterface::compose_physical`, which applies BOTH the
// `× col_scale` and the `÷ scale_objective` AGAIN
// (`linear_interface.cpp` step 1 + step 1b), giving a double-scaled
// readback.
//
// This file pins the documented round-trip invariants
// (`compose_physical` block comment, `linear_interface.cpp:1142-1146`):
//
//   get_row_low[i] = raw_lb × composite_scale = phys_lb           ✓
//   get_coeff(i, c) = raw_coeff × col_scale × row_scale = phys ✓
//
// Coverage:
//   * RHS round-trip under (scale_obj ∈ {1, 1000}) × (col_scale ∈ {1, 5})
//   * State-variable physical coefficient round-trip on the same grid
//
// Tolerance choice: ``cut_coeff_eps`` (1e-8 by default) is the
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

/// Resolve the LP column for the first registered state variable
/// matching (uid, col_name == "efin").  Mirrors the loader's own
/// inner lookup at `sddp_boundary_cuts.cpp` (which keys on uid only,
/// not class_name) so we end up at the same column the loader
/// chose.  In the 3-phase hydro fixture the only uid=1 state var is
/// the reservoir's; the loader's outer name_to_class_uid["j_up"]
/// hands us Junction's uid=1, and the inner loop matches the
/// reservoir's efin since both share that uid.  This helper picks
/// the same column either way.
[[nodiscard]] auto find_efin_state_col_by_uid(const PlanningLP& planning_lp,
                                              SceneIndex scene_index,
                                              PhaseIndex phase_index,
                                              Uid uid)
    -> std::optional<ColIndex>
{
  const auto& svar_map =
      planning_lp.simulation().state_variables(scene_index, phase_index);
  for (const auto& [key, svar] : svar_map) {
    if (key.uid == uid && key.col_name == std::string_view {"efin"}) {
      return svar.col();
    }
  }
  return std::nullopt;
}

/// Run a single round-trip case and verify both the RHS and the
/// state-var coefficient survive the assembly + compose_physical
/// round trip.  Returns silently on success; doctest's CHECK macros
/// surface failures with full context capture.
void run_boundary_cut_round_trip(double scale_obj, double col_scale)
{
  CAPTURE(scale_obj);
  CAPTURE(col_scale);

  auto planning = make_3phase_hydro_planning();
  planning.options.scale_objective = OptReal {scale_obj};
  // Force eager-resident backend so `get_row_low()` / `get_coeff()`
  // reads aren't shaped by a release/reload cycle.
  planning.options.sddp_options.low_memory_mode = LowMemoryMode::off;
  // Junction efin column scale — exercises `compose_physical` step 1
  // (col_scale multiplication).  Pre-fix the assembly multiplied by
  // col_scale here AND compose_physical multiplied again, leaving
  // `get_coeff()` returning ``-coeff × col_scale`` instead of
  // ``-coeff``.  Post-fix the assembly emits ``-coeff`` and
  // get_coeff returns ``-coeff`` after the single compose multiply.
  if (col_scale != 1.0) {
    // Reservoir:efin is the only state-var column the 3-phase fixture
    // registers — Junctions don't carry efin.  Scaling that column
    // exercises compose_physical step 1 (the col_scale multiply).
    planning.options.variable_scales.push_back(VariableScale {
        .class_name = "Reservoir",
        .variable = "efin",
        .scale = col_scale,
    });
  }
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_bdr_round_trip.csv")
          .string();
  constexpr double phys_rhs = 42.0;
  constexpr double phys_coeff = 5.0;
  {
    std::ofstream ofs(tmp_file);
    // Header: bare reservoir name "rsv1" — name_to_class_uid maps to
    // ("Reservoir", 1) (the reservoir entry added by the P2-2 fix).
    // The loader's class-aware inner match now lands on
    // Reservoir:1:efin specifically.  Junction:1 has no efin state
    // var, so a `j_up` header would correctly resolve to nothing
    // under the post-R4 class-aware match.
    ofs << "name,iteration,scene,rhs,rsv1\n";
    ofs << "rt_cut,1,0," << phys_rhs << "," << phys_coeff << "\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;
  opts.boundary_max_iterations = 0;

  const LabelMaker label_maker {LpNamesLevel::none};
  auto states = make_default_scene_phase_states(planning_lp);

  // α has to be pre-registered (load_boundary_cuts_csv won't add α
  // itself; production drives this from
  // SDDPMethod::initialize_alpha_variables).
  register_alpha_variables(planning_lp, first_scene_index(), 1.0);

  const auto& sim = planning_lp.simulation();
  const auto last_phase = sim.last_phase_index();
  auto& last_li =
      planning_lp.system(first_scene_index(), last_phase).linear_interface();
  const auto pre_load_numrows = last_li.get_numrows();

  // Locate the column the loader will resolve "j_up" to.  The
  // loader matches by uid only inside svar_map, so we do the same
  // here — see find_efin_state_col_by_uid's docstring.
  const auto state_col = find_efin_state_col_by_uid(
      planning_lp, first_scene_index(), last_phase, Uid {1});
  REQUIRE(state_col.has_value());

  const auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  REQUIRE(result->count == 1);
  REQUIRE(last_li.get_numrows() == pre_load_numrows + 1);

  const auto cut_row = RowIndex {static_cast<Index>(pre_load_numrows)};

  // Round-trip 1: RHS (`compose_physical` lines 1142-1146 invariant).
  // Pre-fix: `get_row_low()[cut_row] == phys_rhs / scale_obj`
  //          (off by 1/scale_obj when scale_obj != 1).
  // Post-fix: identity round-trip to within cut_coeff_eps.
  const double row_low = last_li.get_row_low()[cut_row];
  CAPTURE(row_low);
  CAPTURE(phys_rhs);
  CHECK(row_low
        == doctest::Approx(phys_rhs).epsilon(
            PlanningOptionsLP::default_sddp_cut_coeff_eps));

  // Round-trip 2: state-variable physical coefficient.
  // `LinearInterface::get_coeff` returns
  //   raw_coeff × col_scale × row_scale
  // which under the documented round-trip equals the input physical
  // coefficient.  Pre-fix the assembly pre-multiplied by col_scale
  // (and pre-divided by scale_obj), so get_coeff returned the
  // double-composed value (≠ -phys_coeff whenever col_scale != 1
  // OR scale_obj != 1).
  const double phys_coeff_in_lp = last_li.get_coeff(cut_row, *state_col);
  CAPTURE(phys_coeff_in_lp);
  CHECK(phys_coeff_in_lp
        == doctest::Approx(-phys_coeff)
               .epsilon(PlanningOptionsLP::default_sddp_cut_coeff_eps));

  std::filesystem::remove(tmp_file);
}

}  // namespace

TEST_CASE(  // NOLINT
    "load_boundary_cuts_csv — RHS + coefficient round-trip across "
    "(scale_obj × col_scale)")
{
  // 2×2 cross-product separates the two double-scaling failure
  // modes:
  //   * scale_obj != 1 exercises compose_physical step 1b (÷ scale_obj)
  //   * col_scale  != 1 exercises compose_physical step 1  (× col_scale)
  // Both together exercise the full chain.  The (1, 1) corner is the
  // identity sanity check — must pass even under the pre-fix bug.
  SUBCASE("scale_obj=1, col_scale=1 (identity)")
  {
    run_boundary_cut_round_trip(/*scale_obj=*/1.0, /*col_scale=*/1.0);
  }
  SUBCASE("scale_obj=1000, col_scale=1 (objective scaling only)")
  {
    run_boundary_cut_round_trip(/*scale_obj=*/1000.0, /*col_scale=*/1.0);
  }
  SUBCASE("scale_obj=1, col_scale=5 (column scaling only)")
  {
    run_boundary_cut_round_trip(/*scale_obj=*/1.0, /*col_scale=*/5.0);
  }
  SUBCASE("scale_obj=1000, col_scale=5 (both scalings compounded)")
  {
    run_boundary_cut_round_trip(/*scale_obj=*/1000.0, /*col_scale=*/5.0);
  }
}
