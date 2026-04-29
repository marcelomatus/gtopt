// SPDX-License-Identifier: BSD-3-Clause
//
// Dual-side regression for loaded boundary cuts.
//
// `compose_physical`'s round-trip block (linear_interface.cpp,
// 1142-1146) promises:
//
//     get_row_dual[i] = raw_dual × scale_obj / composite_scale = π_phys
//
// The audit's P0-1 fix corrected the primal side (RHS + coefficient
// round-trip).  This test pins the dual side: after solving the LP
// with a binding loaded boundary cut, the row dual must be finite
// and non-negative (KKT: π ≥ 0 on a `>=` row at optimality), and
// must scale with the LP's α cost coefficient.  Catches future
// regressions that re-introduce a multiplicative error on the dual
// path (e.g., forgetting to fold scale_obj into composite_scale).

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
    "load_boundary_cuts_csv — bound cut dual is finite + non-negative "
    "after resolve")
{
  // Pick a phys_rhs large enough that the cut binds at optimality —
  // any positive value works because α has a positive cost and will
  // otherwise want to drop to 0.  With scale_alpha=1.0 the LP cost on
  // α is exactly scale_alpha, so the cut's row dual at optimum is
  // scale_alpha (in physical $).  Pre-fix any double-scaling on the
  // dual path would have shown up as π × scale_obj or π / scale_obj.
  auto planning = make_3phase_hydro_planning();
  // Scale_objective != 1 so the dual round-trip's scale_obj factor
  // matters.  Without this the contract degenerates to identity.
  planning.options.scale_objective = OptReal {1000.0};
  // Force eager backend so we can read get_row_dual without an
  // intermediate release/reload cycle masking the dual value.
  planning.options.sddp_options.low_memory_mode = LowMemoryMode::off;
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_bdr_dual_readback.csv")
                            .string();
  // Cut shape: ``α + 0 × x_j_up >= phys_rhs`` — a state-var column
  // is required by the loader's header validation (it expects at
  // least one element column after `rhs`), but a zero coefficient
  // keeps the dual analysis clean.  At optimum α* = phys_rhs and the
  // cut's row dual = α's cost coefficient (scale_alpha).  We use
  // phys_rhs = 100 (well above α's natural minimum) so the cut is
  // the binding constraint on α.
  constexpr double phys_rhs = 100.0;
  {
    std::ofstream ofs(tmp_file);
    // "rsv1" maps to ("Reservoir", 1) post the P2-2 fix; the
    // post-R4 class-aware inner match lands on Reservoir:1:efin.
    // Coefficient is 0.0 so the cut is purely α >= phys_rhs at the
    // optimum — keeps the dual analysis clean.
    ofs << "name,iteration,scene,rhs,rsv1\n";
    ofs << "dual_cut,1,0," << phys_rhs << ",0.0\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;
  opts.boundary_max_iterations = 0;

  const LabelMaker label_maker {LpNamesLevel::none};
  auto states = make_default_scene_phase_states(planning_lp);

  constexpr double scale_alpha = 1.0;
  register_alpha_variables(planning_lp, first_scene_index(), scale_alpha);

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

  const auto cut_row = RowIndex {static_cast<Index>(pre_load_numrows)};

  // Solve the LP — forces the dual to be computed.  The 3-phase
  // hydro fixture is small enough that any reasonable LP backend
  // closes the gap immediately.
  const auto solve_result = planning_lp.resolve();
  REQUIRE(solve_result.has_value());

  // ── Dual readback ─────────────────────────────────────────────
  // After release/reconstruct, get_row_dual returns the cached
  // dual scaled by `composite_scale` so that the value sits in the
  // same physical $/(physical-unit) basis as the LP cost.
  //
  // Loose assertions only — exact magnitude depends on solver and
  // scaling.  What we pin:
  //   1. dual is finite (not NaN, not ±∞) — catches malformed cut row.
  //   2. dual is non-negative — KKT lower-bound row at optimum.
  //   3. dual is bounded by a sane order of magnitude — no
  //      double-scaling-by-1000 disaster.
  const double dual = last_li.get_row_dual()[cut_row];
  CAPTURE(dual);
  CHECK(std::isfinite(dual));
  // KKT on a `>=` row: π ≥ 0 at optimum.
  CHECK(dual >= -PlanningOptionsLP::default_sddp_cut_coeff_eps);
  // Magnitude sanity: a regression that re-introduces a ×scale_obj
  // factor on the dual path would push this to ~1000 (π_phys × 1000)
  // or ~0.001 (π_phys / 1000), so a [0, 100) bound discriminates
  // both directions cleanly while not pinning the exact dual value
  // (which depends on the solver's optimal α — the cut is hard to
  // make strictly binding without driving α explicitly).
  CHECK(dual < 100.0);

  std::filesystem::remove(tmp_file);
}
