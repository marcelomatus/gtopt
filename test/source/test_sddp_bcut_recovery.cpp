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

using namespace gtopt;

TEST_CASE(  // NOLINT
    "bcut recovery: StateVariable::reduced_cost survives add/delete rows")
{
  using namespace gtopt;

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
  using namespace gtopt;

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
  using namespace gtopt;

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

// ─── Gap C1 — recovery branch consistency under compress mode ───────
//
// Pins the contract from commit 6cf57176: when
// `install_aperture_backward_cut` falls into the recovery branch it
// must call `delete_rows(to_delete)` followed by
// `record_cut_deletion(to_delete)`.  A subsequent
// `clone_from_flat(with_replay=true)` must NOT re-inject the deleted
// cut into the throwaway aperture clone — i.e. the replay buffer must
// reflect the deletion, otherwise the next backward-pass aperture
// would see a stale cut and produce inconsistent reduced costs.
TEST_CASE(  // NOLINT
    "bcut recovery: delete_rows + record_cut_deletion drops cut from "
    "subsequent clone_from_flat(with_replay)")
{
  using namespace gtopt;

  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();

  // Build a small 3-col / 3-row LP source.  Use feature-style mixed
  // bounds so the LP is genuinely solvable after the recovery
  // sequence (a no-op LP would hide many failure modes).
  LinearProblem lp {
      "bcut_recovery_compress",
  };
  std::ignore = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 1.0,
      .class_name = "Gen",
      .variable_name = "g0",
      .variable_uid = 0,
  });
  std::ignore = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 2.0,
      .class_name = "Gen",
      .variable_name = "g1",
      .variable_uid = 1,
  });
  std::ignore = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 3.0,
      .class_name = "Gen",
      .variable_name = "g2",
      .variable_uid = 2,
  });
  SparseRow bal {
      .lowb = 5.0,
      .uppb = 5.0,
      .class_name = "Bus",
      .constraint_name = "bal",
      .variable_uid = 0,
  };
  bal[ColIndex {0}] = 1.0;
  bal[ColIndex {1}] = 1.0;
  bal[ColIndex {2}] = 1.0;
  std::ignore = lp.add_row(std::move(bal));
  SparseRow lim {
      .lowb = -LinearProblem::DblMax,
      .uppb = 8.0,
      .class_name = "Lim",
      .constraint_name = "cap",
      .variable_uid = 0,
  };
  lim[ColIndex {0}] = 1.0;
  lim[ColIndex {1}] = 1.0;
  std::ignore = lp.add_row(std::move(lim));
  SparseRow lim2 {
      .lowb = -LinearProblem::DblMax,
      .uppb = 8.0,
      .class_name = "Lim",
      .constraint_name = "cap",
      .variable_uid = 1,
  };
  lim2[ColIndex {1}] = 1.0;
  lim2[ColIndex {2}] = 1.0;
  std::ignore = lp.add_row(std::move(lim2));
  auto flat = lp.flatten({});

  LinearInterface src(reg.default_solver());
  src.set_low_memory(LowMemoryMode::compress, CompressionCodec::lz4);
  src.load_flat(flat);
  src.save_snapshot(std::move(flat));
  src.save_base_numrows();
  const auto base = static_cast<int>(src.get_numrows());

  // Add α col post-freeze (auto-records into m_replay_ since mode !=
  // off).  The α col itself is dynamic, not a cut — it must survive
  // the cut-deletion path.
  const auto alpha = src.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1.0e30,
      .cost = 10.0,
      .class_name = "Sddp",
      .variable_name = "alpha",
      .variable_uid = 0,
  });

  // Add two cut rows via add_row + record_cut_row (the canonical pair
  // the install_aperture_backward_cut path uses).
  SparseRow cut0;
  cut0[alpha] = 1.0;
  cut0[ColIndex {1}] = 0.5;
  cut0.lowb = 1.0;
  cut0.uppb = LinearProblem::DblMax;
  cut0.class_name = "Sddp";
  cut0.constraint_name = "optcut";
  cut0.variable_uid = 100;
  const auto cut0_row = src.add_row(cut0);
  src.record_cut_row(cut0);

  SparseRow cut1;
  cut1[alpha] = 1.0;
  cut1[ColIndex {2}] = 0.25;
  cut1.lowb = 2.0;
  cut1.uppb = LinearProblem::DblMax;
  cut1.class_name = "Sddp";
  cut1.constraint_name = "optcut";
  cut1.variable_uid = 101;
  std::ignore = src.add_row(cut1);
  src.record_cut_row(cut1);

  REQUIRE(src.replay_buf().active_cuts_size() == 2);
  REQUIRE(src.replay_buf().dynamic_cols_size() == 1);

  // Pre-condition: clone_from_flat(with_replay=true) must show both
  // cuts (base + alpha-col is structural; cuts add 2 rows).
  {
    auto pre_clone = src.clone_from_flat(LinearInterface::CloneKind::shallow);
    CHECK(pre_clone.get_numrows() == base + 2);
  }

  // Simulate the recovery branch: delete cut0, record the deletion.
  // The combined call sequence is what
  // `install_aperture_backward_cut` does after a non-optimal solve.
  const std::array<int, 1> to_delete {static_cast<int>(cut0_row)};
  src.delete_rows(to_delete);
  src.record_cut_deletion(to_delete);

  // Source's replay buffer now lists exactly 1 active cut (cut1).
  CHECK(src.replay_buf().active_cuts_size() == 1);
  CHECK(src.replay_buf().dynamic_cols_size() == 1);

  // Post-condition: a fresh clone_from_flat(with_replay=true) must
  // show ONLY cut1 (base + 1 cut, no re-injection of cut0).
  auto post_clone = src.clone_from_flat(LinearInterface::CloneKind::shallow);
  CHECK(post_clone.get_numrows() == base + 1);

  // And the clone must remain solvable — the recovery sequence
  // mustn't corrupt the LP structure.
  const SolverOptions opts {};
  REQUIRE(post_clone.resolve(opts).has_value());
  CHECK(post_clone.is_optimal());
}
