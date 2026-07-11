// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_plp_feasibility_cuts.cpp
 * @brief     Correctness pins for the PLP-exact feasibility-cut mode
 *            (`elastic_filter_mode = state_repair`, alias plp):
 *            `build_plp_feasibility_cuts`
 *            + `ElasticCostPolicy::Model::plp_unit_rc_tilt`.
 *
 * Reproduces PLP's `plp-agrespd.f::AgrElastici` (FOneFeasRay = FALSE)
 * against small hand-solvable LPs.  The CANUTILLAR fixture mirrors the
 * empirical CEN65 case (stage 25, sim 16): a maintenance emin floor
 * unreachable from the trial state, whose PLP cut is a MID-BOX single
 * variable floor `vf ≥ 532.547` — NOT a clamp to the box top.
 *
 * Fixture pattern follows test_elastic_clone_dump.cpp: build a labelled
 * LP, `initial_solve`, pin the dependent column to the trial value, run
 * `elastic_filter_solve` with the plp cost policy, then extract cuts.
 */

#include <cmath>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/benders_cut.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/state_variable.hpp>

using namespace gtopt;
// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace
{

/// The plp cost policy every test uses: unit slack costs + 0.01·rc tilt.
constexpr ElasticCostPolicy plp_policy()
{
  return ElasticCostPolicy {
      .model = ElasticCostPolicy::Model::plp_unit_rc_tilt,
      .rc_tilt_factor = 0.01,
      .scale_objective = 1.0,
  };
}

/// CANUTILLAR-pattern fixture (CEN65 stage 25, sim 16 arithmetic):
///   dep  in [0, 1000], pinned at trial = 528.705 by the forward pass
///   vf   in [596.152, 1000]   (the maintenance emin floor)
///   row: vf − dep = 63.605    (the stage inflow)
/// Infeasible at the trial: vf = 528.705 + 63.605 = 592.31 < 596.152.
/// Minimal repair: dep = 596.152 − 63.605 = 532.547 (+3.842, mid-box).
struct CanutillarFixture
{
  LinearInterface li;
  ColIndex dep {};
  std::vector<StateVarLink> links;

  static constexpr double kTrial = 528.705;
  static constexpr double kInflow = 63.605;
  static constexpr double kEminFloor = 596.152;
  static constexpr double kRepairedDep = kEminFloor - kInflow;  // 532.547
  static constexpr ColIndex kSourceCol {7};

  explicit CanutillarFixture(double source_upp = 1000.0)
  {
    dep = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1000.0,
        .class_name = "Reservoir",
        .variable_name = "eini",
        .variable_uid = 64,
    });
    const auto vf = li.add_col(SparseCol {
        .lowb = kEminFloor,
        .uppb = 1000.0,
        .class_name = "Reservoir",
        .variable_name = "efin",
        .variable_uid = 64,
    });

    SparseRow balance {
        .lowb = kInflow,
        .uppb = kInflow,
        .class_name = "Reservoir",
        .constraint_name = "balance",
        .variable_uid = 64,
    };
    balance[vf] = 1.0;
    balance[dep] = -1.0;
    std::ignore = li.add_row(balance);

    // Solve once while feasible so the backend holds a warm state,
    // then pin the dependent column at the (infeasible) trial value.
    std::ignore = li.initial_solve();
    REQUIRE(li.is_optimal());
    li.set_col(dep, kTrial);

    links = {
        {
            .source_col = kSourceCol,
            .dependent_col = dep,
            .target_phase_index = PhaseIndex {1},
            .trial_value = kTrial,
            .source_low = 0.0,
            .source_upp = source_upp,
            .uid = Uid {64},
        },
    };
  }
};

}  // namespace

TEST_CASE("plp fcut: single shortfall emits one mid-box cut")  // NOLINT
{
  CanutillarFixture fx;

  auto elastic = elastic_filter_solve(
      fx.li, fx.links, 1.0, SolverOptions {}, plp_policy());
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);

  constexpr double kFactEps = 1e-8;
  auto res =
      build_plp_feasibility_cuts(*elastic, fx.links, LpContext {}, kFactEps);

  CHECK(res.status == PlpCutStatus::cuts_added);
  REQUIRE(res.cuts.size() == 1);

  const auto& cut = res.cuts.front();
  REQUIRE(cut.cmap.contains(CanutillarFixture::kSourceCol));
  const double coeff = cut.cmap.at(CanutillarFixture::kSourceCol);

  // Unit slack cost, sdn strictly interior at the elastic optimum
  // ⇒ ray = +1 exactly (PLP's typical ±1 coefficient).
  CHECK(std::abs(coeff) == doctest::Approx(1.0));

  // The implied bound is the MINIMAL repaired state (mid-box), with
  // the outward FactEPS margin — matches PLP's `vf64(t−1) ≥ 532.547`.
  const double implied = cut.lowb / coeff;
  CHECK(implied
        == doctest::Approx(CanutillarFixture::kRepairedDep * (1.0 + kFactEps))
               .epsilon(1e-6));

  // Strictly below the previous-phase upper bound: PLP never clamps
  // the RHS to the box top (bound-consistency holds by construction).
  CHECK(implied < 1000.0);
  CHECK(implied > CanutillarFixture::kTrial);
}

TEST_CASE("plp fcut: rc tilt picks the cheap reservoir")  // NOLINT
{
  // Two identical reservoirs, either can repair the shortfall
  // `dep_a + dep_b ≥ 70` from trial (30, 30).  The prev-basis reduced
  // cost of A (50 → tilt 0.5 → sdn cost 1.5) makes A more expensive
  // to raise than B (rc 0 → sdn cost 1.0), so the elastic optimum
  // raises B only and the single emitted cut lands on B's source col.
  LinearInterface li;
  const auto dep_a = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .class_name = "Reservoir",
      .variable_name = "eini",
      .variable_uid = 1,
  });
  const auto dep_b = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .class_name = "Reservoir",
      .variable_name = "eini",
      .variable_uid = 2,
  });

  SparseRow demand {
      .lowb = 70.0,
      .uppb = LinearProblem::DblMax,
      .class_name = "Demand",
      .constraint_name = "min",
      .variable_uid = 1,
  };
  demand[dep_a] = 1.0;
  demand[dep_b] = 1.0;
  std::ignore = li.add_row(demand);

  std::ignore = li.initial_solve();
  REQUIRE(li.is_optimal());
  li.set_col(dep_a, 30.0);
  li.set_col(dep_b, 30.0);

  const StateVariable sv_a {LPKey {}, dep_a, 0.0, 1.0, LpContext {}};
  sv_a.set_col_sol(30.0);
  sv_a.set_source_reduced_cost(50.0);  // tilt = 0.01 × 50 = 0.5
  const StateVariable sv_b {LPKey {}, dep_b, 0.0, 1.0, LpContext {}};
  sv_b.set_col_sol(30.0);
  sv_b.set_source_reduced_cost(0.0);

  const ColIndex src_a {10};
  const ColIndex src_b {11};
  const std::vector<StateVarLink> links = {
      {
          .source_col = src_a,
          .dependent_col = dep_a,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 30.0,
          .source_low = 0.0,
          .source_upp = 100.0,
          .state_var = &sv_a,
          .uid = Uid {1},
      },
      {
          .source_col = src_b,
          .dependent_col = dep_b,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 30.0,
          .source_low = 0.0,
          .source_upp = 100.0,
          .state_var = &sv_b,
          .uid = Uid {2},
      },
  };

  auto elastic =
      elastic_filter_solve(li, links, 1.0, SolverOptions {}, plp_policy());
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);

  auto res = build_plp_feasibility_cuts(*elastic, links, LpContext {}, 1e-8);

  CHECK(res.status == PlpCutStatus::cuts_added);
  REQUIRE(res.cuts.size() == 1);
  const auto& cut = res.cuts.front();
  // The cut must land on the CHEAP reservoir (B) — the A link is
  // zeroed by the dx filter (its slack never activated).
  CHECK(cut.cmap.contains(src_b));
  CHECK(!cut.cmap.contains(src_a));
  const double coeff = cut.cmap.at(src_b);
  // B was raised 30 → 40; implied bound = the repaired state.
  CHECK(cut.lowb / coeff == doctest::Approx(40.0).epsilon(1e-6));
}

TEST_CASE("plp fcut: holguras when repair is below dx tolerance")  // NOLINT
{
  // The infeasibility needs a repair of 1e-5 on a trial of 528.705;
  // with fact_eps = 1e-3 the dx filter threshold is
  // 1e-3 × (528.705 + 1e-8) ≈ 0.53 ≫ 1e-5, so the link is zeroed and
  // the builder reports HOLGURAS (PLP IStat = −1) with NO rows.
  LinearInterface li;
  const auto dep = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 1000.0,
      .class_name = "Reservoir",
      .variable_name = "eini",
      .variable_uid = 3,
  });

  SparseRow floor_row {
      .lowb = 528.705 + 1e-5,
      .uppb = LinearProblem::DblMax,
      .class_name = "Reservoir",
      .constraint_name = "floor",
      .variable_uid = 3,
  };
  floor_row[dep] = 1.0;
  std::ignore = li.add_row(floor_row);

  std::ignore = li.initial_solve();
  REQUIRE(li.is_optimal());
  li.set_col(dep, 528.705);

  const std::vector<StateVarLink> links = {
      {
          .source_col = ColIndex {5},
          .dependent_col = dep,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 528.705,
          .source_low = 0.0,
          .source_upp = 1000.0,
          .uid = Uid {3},
      },
  };

  auto elastic =
      elastic_filter_solve(li, links, 1.0, SolverOptions {}, plp_policy());
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);

  auto res = build_plp_feasibility_cuts(*elastic, links, LpContext {}, 1e-3);

  CHECK(res.status == PlpCutStatus::holguras);
  CHECK(res.cuts.empty());
}

TEST_CASE("plp fcut: fail when relaxed clone stays infeasible")  // NOLINT
{
  // Previous-phase box capped at 500: even fully relaxed, dep ≤ 500
  // gives vf = dep + 63.605 ≤ 563.6 < 596.152 — the elastic clone is
  // itself infeasible (`solved == false`) and the builder reports
  // FAIL with no rows (caller declares the scene infeasible).
  CanutillarFixture fx {/*source_upp=*/500.0};

  auto elastic = elastic_filter_solve(
      fx.li, fx.links, 1.0, SolverOptions {}, plp_policy());
  REQUIRE(elastic.has_value());
  CHECK(!elastic->solved);

  auto res = build_plp_feasibility_cuts(*elastic, fx.links, LpContext {}, 1e-8);

  CHECK(res.status == PlpCutStatus::fail);
  CHECK(res.cuts.empty());
}

TEST_CASE("plp fcut: RHS carries the exact FactEPS margin")  // NOLINT
{
  CanutillarFixture fx;

  auto elastic = elastic_filter_solve(
      fx.li, fx.links, 1.0, SolverOptions {}, plp_policy());
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);

  constexpr double kFactEps = 1e-6;
  auto res =
      build_plp_feasibility_cuts(*elastic, fx.links, LpContext {}, kFactEps);

  CHECK(res.status == PlpCutStatus::cuts_added);
  REQUIRE(res.cuts.size() == 1);
  const auto& cut = res.cuts.front();
  const double ray = cut.cmap.at(CanutillarFixture::kSourceCol);

  // Recompute nx from the clone's own slack activations (dep_scale = 1
  // on this unscaled fixture): nx = trial + (sdn − sup).
  const auto& info = elastic->link_infos.front();
  REQUIRE(info.relaxed);
  const auto sol = elastic->clone.get_col_sol_raw();
  const double dx = sol[info.sdn_col] - sol[info.sup_col];
  const double nx = CanutillarFixture::kTrial + dx;
  const double rhsi = ray * nx;

  // rhs = rhsi + fact_eps·|rhsi| — PLP plp-agrespd.f:791, verbatim.
  CHECK(cut.lowb
        == doctest::Approx(rhsi + kFactEps * std::abs(rhsi)).epsilon(1e-12));
}

TEST_CASE("plp fcut: all-at-upper family is emitted, not dropped")  // NOLINT
{
  // Both states must walk to their box tops to repair
  // `dep_a + dep_b ≥ 300` (= 100 + 200 exactly).  multi_cut's
  // degenerate-family guard drops the whole set; plp mode emits both
  // single-variable rows — bound-consistent BY CONSTRUCTION, each
  // implied bound lands ON (never above) the previous-phase ub.
  // fact_eps = 0 isolates the no-drop semantics from the outward
  // margin (which would sit `ub × fact_eps` above the box top).
  LinearInterface li;
  const auto dep_a = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .class_name = "Reservoir",
      .variable_name = "eini",
      .variable_uid = 1,
  });
  const auto dep_b = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 200.0,
      .class_name = "Reservoir",
      .variable_name = "eini",
      .variable_uid = 2,
  });

  SparseRow demand {
      .lowb = 300.0,
      .uppb = LinearProblem::DblMax,
      .class_name = "Demand",
      .constraint_name = "min",
      .variable_uid = 1,
  };
  demand[dep_a] = 1.0;
  demand[dep_b] = 1.0;
  std::ignore = li.add_row(demand);

  std::ignore = li.initial_solve();
  REQUIRE(li.is_optimal());
  li.set_col(dep_a, 10.0);
  li.set_col(dep_b, 20.0);

  const ColIndex src_a {20};
  const ColIndex src_b {21};
  const std::vector<StateVarLink> links = {
      {
          .source_col = src_a,
          .dependent_col = dep_a,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 10.0,
          .source_low = 0.0,
          .source_upp = 100.0,
          .uid = Uid {1},
      },
      {
          .source_col = src_b,
          .dependent_col = dep_b,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 20.0,
          .source_low = 0.0,
          .source_upp = 200.0,
          .uid = Uid {2},
      },
  };

  auto elastic =
      elastic_filter_solve(li, links, 1.0, SolverOptions {}, plp_policy());
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);

  // Contrast: build_multi_cuts drops the all-upper degenerate family.
  auto mc = build_multi_cuts(*elastic, links, LpContext {}, 1e-8, 1);
  CHECK(mc.empty());

  // plp mode: both per-variable cuts are emitted, each implied bound
  // ≤ the previous-phase ub (bound-consistent, no clamp needed).
  auto res = build_plp_feasibility_cuts(*elastic, links, LpContext {}, 0.0);
  CHECK(res.status == PlpCutStatus::cuts_added);
  REQUIRE(res.cuts.size() == 2);

  for (const auto& cut : res.cuts) {
    REQUIRE(cut.cmap.size() == 1);
    const auto [col, coeff] = *cut.cmap.begin();
    const double ub = (col == src_a) ? 100.0 : 200.0;
    CHECK((col == src_a || col == src_b));
    CHECK(coeff > 0.0);
    CHECK(cut.lowb / coeff <= ub + 1e-9);
    // ... and the cut is not vacuous: it floors the state at its ub.
    CHECK(cut.lowb / coeff == doctest::Approx(ub).epsilon(1e-6));
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)
