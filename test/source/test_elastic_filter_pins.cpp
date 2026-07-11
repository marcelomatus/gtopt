// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_elastic_filter_pins.cpp
 * @brief     Behavior pins for the elastic filter + feasibility-cut
 *            builders, from the 2026-07 code review:
 *
 *  - P0-A: an all-upper-degenerate cut family (every contributing link
 *    clamps at source_upp) must result in NO installed cut.
 *    `build_multi_cuts` implements the drop (family cleared);
 *    `build_feasibility_cut_physical` only WARNs and still returns the
 *    degenerate row — that pin is marked may_fail with FIXME(P0-A).
 *  - Partial degeneracy must NOT drop the family: interior-landing
 *    links keep their (correct) cuts.
 *  - Single-link cut arithmetic: source >= elastic-optimal dep value.
 *  - Zero-activation robustness: feasible pinned trials produce no
 *    cut and no crash (the PLP dx filter guards both builders against
 *    degenerate dual noise on inactive fixing rows).
 *
 * NaN-dual injection (review item P0 NaN guard) is NOT testable from
 * here without production plumbing: `build_multi_cuts` /
 * `build_feasibility_cut_physical` read duals straight from the solved
 * clone's `get_row_dual()` and `LinearInterface` exposes no dual
 * setter, so there is no seam to inject a NaN through (>30 lines of
 * backend mocking would be required).  The NaN guard is exercised
 * indirectly by the production warn-and-skip paths
 * (source/benders_cut.cpp:379, :1484).
 */

#include <cmath>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/benders_cut.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>
#include <gtopt/state_variable.hpp>

using namespace gtopt;

namespace
{

/// Production default (PlanningOptionsLP::sddp_cut_coeff_eps).
constexpr double kPinCutCoeffEps = 1e-8;
/// PLP parity: unit slack cost (osicallsc.cpp:658 `objs = 1.0`).
constexpr double kPinUnitPenalty = 1.0;

[[nodiscard]] StateVariable make_pin_state_var(ColIndex source_col)
{
  return StateVariable {
      StateVariable::LPKey {
          .scene_index = first_scene_index(),
          .phase_index = PhaseIndex {0},
      },
      source_col,
      /*scost=*/0.0,
      /*var_scale=*/1.0,
      LpContext {},
  };
}

// Two state columns s1, s2 with physical box [0, 100] each, pinned at
// low trials, plus a joint demand row `s1 + s2 >= demand`.  With
// demand = 200 the row is satisfiable ONLY at s1 = s2 = 100 =
// source_upp — the all-upper-degenerate family signature.
struct TwoLinkDemandFixture
{
  LinearInterface li {};
  ColIndex s1 {unknown_index};
  ColIndex s2 {unknown_index};
  StateVariable sv1 {make_pin_state_var(ColIndex {99})};
  StateVariable sv2 {make_pin_state_var(ColIndex {100})};
  std::vector<StateVarLink> links {};

  TwoLinkDemandFixture(double trial1, double trial2, double demand)
  {
    s1 = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 100.0,
    });
    s2 = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 100.0,
    });

    SparseRow row;  // s1 + s2 >= demand
    row[s1] = 1.0;
    row[s2] = 1.0;
    row.lowb = demand;
    row.uppb = LinearProblem::DblMax;
    (void)li.add_row(row);  // NOLINT

    auto res = li.initial_solve();
    REQUIRE(res.has_value());
    REQUIRE(li.is_optimal());

    li.set_col(s1, trial1);
    li.set_col(s2, trial2);
    sv1.set_col_sol(trial1);
    sv2.set_col_sol(trial2);

    links = {
        {
            .source_col = ColIndex {99},
            .dependent_col = s1,
            .target_phase_index = PhaseIndex {1},
            .trial_value = trial1,
            .source_low = 0.0,
            .source_upp = 100.0,
            .state_var = &sv1,
            .name = "res_a",
        },
        {
            .source_col = ColIndex {100},
            .dependent_col = s2,
            .target_phase_index = PhaseIndex {1},
            .trial_value = trial2,
            .source_low = 0.0,
            .source_upp = 100.0,
            .state_var = &sv2,
            .name = "res_b",
        },
    };
  }
};

// Two INDEPENDENT state columns with per-column push-up rows:
//   sA >= need_a   (need_a = 100 = source_upp -> A saturates)
//   sB >= need_b   (need_b = 50 -> B lands mid-box)
struct TwoLinkSplitFixture
{
  LinearInterface li {};
  ColIndex sa {unknown_index};
  ColIndex sb {unknown_index};
  StateVariable sva {make_pin_state_var(ColIndex {99})};
  StateVariable svb {make_pin_state_var(ColIndex {100})};
  std::vector<StateVarLink> links {};

  TwoLinkSplitFixture(double trial, double need_a, double need_b)
  {
    sa = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 100.0,
    });
    sb = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 100.0,
    });

    SparseRow ra;  // sA >= need_a
    ra[sa] = 1.0;
    ra.lowb = need_a;
    ra.uppb = LinearProblem::DblMax;
    (void)li.add_row(ra);  // NOLINT

    SparseRow rb;  // sB >= need_b
    rb[sb] = 1.0;
    rb.lowb = need_b;
    rb.uppb = LinearProblem::DblMax;
    (void)li.add_row(rb);  // NOLINT

    auto res = li.initial_solve();
    REQUIRE(res.has_value());
    REQUIRE(li.is_optimal());

    li.set_col(sa, trial);
    li.set_col(sb, trial);
    sva.set_col_sol(trial);
    svb.set_col_sol(trial);

    links = {
        {
            .source_col = ColIndex {99},
            .dependent_col = sa,
            .target_phase_index = PhaseIndex {1},
            .trial_value = trial,
            .source_low = 0.0,
            .source_upp = 100.0,
            .state_var = &sva,
            .name = "sat_link",
        },
        {
            .source_col = ColIndex {100},
            .dependent_col = sb,
            .target_phase_index = PhaseIndex {1},
            .trial_value = trial,
            .source_low = 0.0,
            .source_upp = 100.0,
            .state_var = &svb,
            .name = "mid_link",
        },
    };
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// 5. All-upper-degenerate family -> NO cut installed (P0-A)
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "elastic pins — all-upper-degenerate family: build_multi_cuts drops it")
{
  // Both links must climb to source_upp = 100 to satisfy
  // s1 + s2 >= 200; the family floors every state at its physical max
  // (the cascade-infeasibility signature).  build_multi_cuts implements
  // the whole-family drop: it must return EMPTY so the caller declares
  // infeasibility instead of installing a doomed retry chain.
  TwoLinkDemandFixture fx {10.0, 10.0, 200.0};

  auto elastic =
      elastic_filter_solve(fx.li, fx.links, kPinUnitPenalty, SolverOptions {});
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);
  // Minimum slack: 90 + 90 to reach (100, 100).
  CHECK(elastic->clone.get_obj_value() == doctest::Approx(180.0));

  auto cuts =
      build_multi_cuts(*elastic, fx.links, {}, kPinCutCoeffEps, /*niter=*/0);
  CHECK(cuts.empty());
}

TEST_CASE(  // NOLINT
    "elastic pins — all-upper-degenerate family: "
    "build_feasibility_cut_physical must signal no-usable-cut"
    * doctest::may_fail())
{
  // FIXME(P0-A): source/benders_cut.cpp:433 —
  // build_feasibility_cut_physical detects the all-upper-degenerate
  // family (BoxEdgeStats::all_upper_degenerate), WARNs... and then
  // STILL RETURNS the degenerate row `pi1*s1 + pi2*s2 >= pi*200`,
  // which the forward-pass fallback installs.  Correct behavior
  // (mirroring build_multi_cuts): return an empty/no-op row so no cut
  // is installed and the caller declares infeasibility directly.
  TwoLinkDemandFixture fx {10.0, 10.0, 200.0};

  auto elastic =
      elastic_filter_solve(fx.li, fx.links, kPinUnitPenalty, SolverOptions {});
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);

  auto cut = build_feasibility_cut_physical(
      fx.links, elastic->link_infos, elastic->clone, kPinCutCoeffEps, 0);

  // No usable cut: an empty coefficient map is the no-op signal
  // callers check before installing.  Currently FAILS — the row comes
  // back with both coefficients set and rhs flooring both states at
  // emax simultaneously.
  CHECK(cut.cmap.empty());
}

// ---------------------------------------------------------------------------
// 6. Partial degeneracy keeps the good cuts
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "elastic pins — partial degeneracy: interior link's cut survives")
{
  // Link A is forced to its box edge (sA >= 100 = source_upp) while
  // link B lands mid-box (sB >= 50).  The family is NOT all-upper
  // (1 upper + 1 inside), so build_multi_cuts must keep it — in
  // particular B's cut with the correct implied bound 50.
  TwoLinkSplitFixture fx {10.0, /*need_a=*/100.0, /*need_b=*/50.0};

  auto elastic =
      elastic_filter_solve(fx.li, fx.links, kPinUnitPenalty, SolverOptions {});
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);
  // Slack: A 10->100 (90) + B 10->50 (40) = 130.
  CHECK(elastic->clone.get_obj_value() == doctest::Approx(130.0));

  auto cuts =
      build_multi_cuts(*elastic, fx.links, {}, kPinCutCoeffEps, /*niter=*/0);

  // Family not dropped.
  REQUIRE_FALSE(cuts.empty());

  // B's cut survives with implied bound = 50.
  const SparseRow* cut_b = nullptr;
  for (const auto& c : cuts) {
    if (c.cmap.contains(ColIndex {100})) {
      cut_b = &c;
    }
  }
  REQUIRE(cut_b != nullptr);
  const double pi_b = cut_b->cmap.at(ColIndex {100});
  CHECK(pi_b > 0.0);
  CHECK(cut_b->lowb / pi_b == doctest::Approx(50.0));

  // A's at-edge cut is also kept at niter = 0 (the strict-`>`
  // saturation filter only drops cuts whose PRE-clamp implied bound
  // exceeded the box) — implied bound = 100 exactly.
  const SparseRow* cut_a = nullptr;
  for (const auto& c : cuts) {
    if (c.cmap.contains(ColIndex {99})) {
      cut_a = &c;
    }
  }
  REQUIRE(cut_a != nullptr);
  const double pi_a = cut_a->cmap.at(ColIndex {99});
  CHECK(pi_a > 0.0);
  CHECK(cut_a->lowb / pi_a == doctest::Approx(100.0));
}

// ---------------------------------------------------------------------------
// 7. Valid single-link cut arithmetic
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "elastic pins — single-link arithmetic: eini pinned 30, cut source >= 40")
{
  // eini in [0, 100] pinned at 30; demand row `eini + other >= 90`
  // with other <= 50.  The elastic optimum maxes `other` and lifts
  // eini to 40 (sdn = 10).  Expected cut: pi > 0, rhs = pi * 40 —
  // i.e. source >= 40 with pi = 1 at unit slack cost.
  LinearInterface li;
  const auto eini = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
  });
  const auto other = li.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 50.0,
  });

  SparseRow row;  // eini + other >= 90
  row[eini] = 1.0;
  row[other] = 1.0;
  row.lowb = 90.0;
  row.uppb = LinearProblem::DblMax;
  (void)li.add_row(row);  // NOLINT

  auto res = li.initial_solve();
  REQUIRE(res.has_value());
  REQUIRE(li.is_optimal());

  li.set_col(eini, 30.0);
  auto svar = make_pin_state_var(ColIndex {99});
  svar.set_col_sol(30.0);

  const std::vector<StateVarLink> links = {
      {
          .source_col = ColIndex {99},
          .dependent_col = eini,
          .target_phase_index = PhaseIndex {1},
          .trial_value = 30.0,
          .source_low = 0.0,
          .source_upp = 100.0,
          .state_var = &svar,
      },
  };

  auto elastic =
      elastic_filter_solve(li, links, kPinUnitPenalty, SolverOptions {});
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);
  // SINF = 10 (eini 30 -> 40 once `other` saturates at 50).
  CHECK(elastic->clone.get_obj_value() == doctest::Approx(10.0));

  SUBCASE("aggregated cut: pi > 0, rhs = pi * 40")
  {
    auto cut = build_feasibility_cut_physical(
        links, elastic->link_infos, elastic->clone, kPinCutCoeffEps, 0);

    REQUIRE(cut.cmap.contains(ColIndex {99}));
    const double pi = cut.cmap.at(ColIndex {99});
    CHECK(pi > 0.0);
    CHECK(pi == doctest::Approx(1.0));
    CHECK(cut.lowb == doctest::Approx(pi * 40.0));
  }

  SUBCASE("multi-cut: single cut with implied bound 40")
  {
    auto cuts =
        build_multi_cuts(*elastic, links, {}, kPinCutCoeffEps, /*niter=*/0);
    REQUIRE(cuts.size() == 1);
    const auto& cut = cuts.front();
    const double pi = cut.cmap.at(ColIndex {99});
    CHECK(pi > 0.0);
    CHECK(cut.lowb / pi == doctest::Approx(40.0));
  }
}

// ---------------------------------------------------------------------------
// 8. Zero-activation / zero-dual robustness
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "elastic pins — feasible trials: zero slack, no cut, no crash")
{
  // Both pinned trials already satisfy the joint demand row
  // (60 + 60 >= 100): the elastic clone solves with zero slack
  // activation.  Whatever (possibly degenerate) duals the solver
  // returns on the inactive fixing rows, the PLP dx filter must drop
  // every link — no cut rows, no NaN/garbage coefficients, no crash.
  TwoLinkDemandFixture fx {60.0, 60.0, 100.0};

  auto elastic =
      elastic_filter_solve(fx.li, fx.links, kPinUnitPenalty, SolverOptions {});
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);
  CHECK(elastic->clone.get_obj_value() == doctest::Approx(0.0));

  auto cuts =
      build_multi_cuts(*elastic, fx.links, {}, kPinCutCoeffEps, /*niter=*/0);
  CHECK(cuts.empty());

  auto cut = build_feasibility_cut_physical(
      fx.links, elastic->link_infos, elastic->clone, kPinCutCoeffEps, 0);
  CHECK(cut.cmap.empty());
  // The no-op row must be trivially satisfiable (0 >= 0), never a
  // poisoned RHS.
  CHECK(cut.lowb == doctest::Approx(0.0));

  // All coefficients finite in both emitters (vacuous here since both
  // are empty, but guards against a future emitter writing before
  // filtering).
  for (const auto& c : cuts) {
    for (const auto& [col, coeff] : c.cmap) {
      CHECK(std::isfinite(coeff));
    }
  }
}

// NaN-dual injection pin intentionally omitted: no mockable seam —
// see the file header for the rationale.
