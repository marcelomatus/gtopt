/**
 * @file      test_sddp_bcut_recovery.cpp
 * @brief     Unit tests for the SDDP aperture-backward bcut recovery path
 * @date      2026-04-20
 * @copyright BSD-3-Clause
 *
 * SDDPMethod::install_aperture_backward_cut may delete a freshly-added
 * expected_cut row from src_li and rebuild a bcut using the cached
 * StateVariable::reduced_cost() values.  Two invariants must hold for
 * that to produce a valid Benders underestimator:
 *
 *   I1. `state_var->reduced_cost()` survives arbitrary row add/delete
 *       operations on the LinearInterface (nothing in the backward
 *       pass touches the cached value).
 *   I2. `build_benders_cut` (no-span overload) reads a fresh
 *       `state_var->reduced_cost()` each call, so the next forward-pass
 *       `capture_state_variable_values` refresh is visible immediately.
 */

#include <array>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/benders_cut.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/state_variable.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE(  // NOLINT
    "bcut recovery: StateVariable::reduced_cost survives add/delete rows")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Minimal LP used only as a scratch host for row add/delete.  No solve
  // required; we verify the state-variable cache is unaffected by the
  // row-matrix edits performed during the recovery branch.
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  LinearInterface li(reg.default_solver());

  const auto x = li.add_col(SparseCol {
      .uppb = 1.0,
      .cost = 0.0,
  });
  (void)x;

  // Simulate a forward-pass capture of reduced_cost onto a state var.
  const StateVariable::LPKey lp_key {
      .scene_index = SceneIndex {0},
      .phase_index = PhaseIndex {0},
  };
  const StateVariable svar {lp_key, ColIndex {0}, 0.0, 1.0, LpContext {}};
  svar.set_reduced_cost(7.25);
  REQUIRE(svar.reduced_cost() == doctest::Approx(7.25));

  // Add a couple of rows to the LP (mirror expected_cut install), then
  // delete one (mirror the recovery's delete_rows step).
  SparseRow r1;
  r1[x] = 1.0;
  r1.lowb = -LinearProblem::DblMax;
  r1.uppb = 100.0;
  const auto row1 = li.add_row(r1);

  SparseRow r2;
  r2[x] = 1.0;
  r2.lowb = -LinearProblem::DblMax;
  r2.uppb = 50.0;
  const auto row2 = li.add_row(r2);
  (void)row2;

  const std::array<int, 1> to_delete {static_cast<int>(row1)};
  li.delete_rows(to_delete);

  // I1: cached value is untouched.
  CHECK(svar.reduced_cost() == doctest::Approx(7.25));
}

TEST_CASE(  // NOLINT
    "bcut recovery: build_benders_cut reflects refreshed reduced_cost")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Same shape as the StateVarLink ctor used inside SDDP: a
  // source/dependent column pair with a back-pointer to a real
  // StateVariable.  The no-span build_benders_cut overload reads the
  // reduced cost from svar.reduced_cost() every call, so a subsequent
  // set_reduced_cost() must be reflected on the next cut build.
  const StateVariable::LPKey lp_key {
      .scene_index = SceneIndex {0},
      .phase_index = PhaseIndex {0},
  };
  const StateVariable svar {lp_key, ColIndex {1}, 0.0, 1.0, LpContext {}};
  svar.set_reduced_cost(3.0);
  // Physical trial value = col_sol * var_scale = 10 * 1 = 10.
  svar.set_col_sol(10.0);

  const std::vector<StateVarLink> links = {
      {
          .source_col =
              ColIndex {
                  1,
              },
          .dependent_col =
              ColIndex {
                  2,
              },
          .trial_value = 10.0,
          .state_var = &svar,
      },
  };
  const ColIndex alpha {
      0,
  };

  // Use scale_objective = 1 so rc_phys == rc (var_scale defaults to 1).
  // First call: rc = 3.0 → rc_phys = 3.0, v̂_phys = col_sol_physical() = 10
  // → coefficient on source_col = -3.0, lowb = 100 - 3*10 = 70.
  const auto cut1 = build_benders_cut_physical(alpha, links, 100.0, 1.0);
  CHECK(cut1.cmap.at(links.front().source_col) == doctest::Approx(-3.0));
  CHECK(cut1.lowb == doctest::Approx(70.0));

  // Simulate a subsequent forward-pass capture writing a new rc.
  svar.set_reduced_cost(-2.0);

  // Second call: rc = -2.0 → coefficient +2.0, lowb == 100 - (-2)*10 == 120.
  const auto cut2 = build_benders_cut_physical(alpha, links, 100.0, 1.0);
  CHECK(cut2.cmap.at(links.front().source_col) == doctest::Approx(2.0));
  CHECK(cut2.lowb == doctest::Approx(120.0));
}

TEST_CASE(  // NOLINT
    "bcut recovery: delete_rows({added_row}) after add_row keeps LP consistent")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // End-to-end row-accounting test mirroring the recovery path:
  //   row_idx = li.add_row(expected_cut);
  //   resolve → non-optimal;
  //   li.delete_rows({row_idx});
  //   li.add_row(bcut);
  // The final row count must match the "bcut only" state.
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  LinearInterface li(reg.default_solver());

  const auto x = li.add_col(SparseCol {
      .uppb = 1.0,
      .cost = 0.0,
  });
  const auto base_rows = li.get_numrows();

  SparseRow expected_cut;
  expected_cut[x] = 1.0;
  expected_cut.lowb = -LinearProblem::DblMax;
  expected_cut.uppb = 10.0;
  const auto expected_row = li.add_row(expected_cut);
  CHECK(li.get_numrows() == base_rows + 1);

  const std::array<int, 1> to_delete {static_cast<int>(expected_row)};
  li.delete_rows(to_delete);
  CHECK(li.get_numrows() == base_rows);

  SparseRow bcut;
  bcut[x] = 1.0;
  bcut.lowb = -LinearProblem::DblMax;
  bcut.uppb = 20.0;
  const auto bcut_row = li.add_row(bcut);
  CHECK(li.get_numrows() == base_rows + 1);
  // Row indices are not required to be stable across delete+add (backend-
  // dependent); we only require the row is installed and accessible.
  CHECK(static_cast<int>(bcut_row) >= 0);
}
