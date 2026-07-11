// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_elastic_feasibility_literature.cpp
 * @brief     Literature-imported LP cases with known feasibility-cut
 *            answers, driven end-to-end through the real elastic filter
 *            (`elastic_filter_solve`) and the two cut builders
 *            (`build_feasibility_cut_physical`, `build_multi_cuts`).
 *
 * Each fixture is a textbook / paper example whose induced-feasibility
 * region (and hence the correct feasibility cut) is known in closed
 * form:
 *
 *  1. Murphy, "Benders, Nested Benders and Stochastic Programming"
 *     (Tech. Report 675 / arXiv:1312.3158, eqs. 31-33) — 1-D state,
 *     feasible iff -2 <= y <= 2, pins the sign convention end-to-end.
 *  2. Birge & Louveaux, *Introduction to Stochastic Programming*,
 *     §5.1 induced-constraints example — 2-D state, known certificate.
 *  3. Diniz et al., "Assessment of Lagrangian relaxation with variable
 *     splitting for hydrothermal scheduling" / feasibility-facet
 *     two-area example (optimization-online 5186, Table 1) — 2
 *     reservoirs, 2 periods; deficit-free region has facets
 *     x1 >= 45, x2 >= 10, x1 + x2 >= 95.
 *  4. PLP-style mini-cascade single-reservoir recursion (cf. SDDP.jl
 *     theory_intro with a thermal cap) — stage feasible iff
 *     v_prev >= 50, PLP emits `cf: vf 1.0 / rhsi: 50`.
 *
 * The elastic phase-1 LP prices every slack at unit cost (PLP
 * `osicallsc.cpp:658` parity), so fixing-row duals are +/-1 and the
 * cuts come out as single-variable bounds `pi * source >= pi * bound`
 * — the same shape PLP's AgrElastici produces.
 */

#include <utility>
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
constexpr double kCutCoeffEps = 1e-8;
/// PLP parity: unit slack cost (osicallsc.cpp:658 `objs = 1.0`).
constexpr double kUnitPenalty = 1.0;

using Point = std::vector<std::pair<ColIndex, double>>;

/// LHS of a (>=-sense) cut row evaluated at a state point.
[[nodiscard]] double cut_lhs(const SparseRow& cut, const Point& point)
{
  double lhs = 0.0;
  for (const auto& [col, val] : point) {
    const auto it = cut.cmap.find(col);
    if (it != cut.cmap.end()) {
      lhs += it->second * val;
    }
  }
  return lhs;
}

/// True when the cut excludes the point (strictly violated).
[[nodiscard]] bool cuts_off(const SparseRow& cut, const Point& point)
{
  return cut_lhs(cut, point) < cut.lowb - 1e-6;
}

/// True when the point satisfies the cut (boundary counts as kept).
[[nodiscard]] bool keeps(const SparseRow& cut, const Point& point)
{
  return cut_lhs(cut, point) >= cut.lowb - 1e-6;
}

/// StateVariable carrier so the cut builders can read
/// `col_sol_physical()` as the trial value v-hat.
[[nodiscard]] StateVariable make_state_var(ColIndex source_col)
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

// ---------------------------------------------------------------------------
// 1. Murphy TR.675 (arXiv:1312.3158) eqs. 31-33 — 1-D subproblem
//
//    max v   s.t.  v >= 1,  v <= 3 + y,  v <= 3 - y
//
// feasible iff -2 <= y <= 2.  The state y is pinned at the trial value;
// the elastic phase-1 LP must move it to the nearest feasible point.
// ---------------------------------------------------------------------------
struct MurphyFixture
{
  LinearInterface li {};
  ColIndex y {unknown_index};
  ColIndex v {unknown_index};
  StateVariable svar {make_state_var(ColIndex {99})};
  std::vector<StateVarLink> links {};

  explicit MurphyFixture(double trial)
  {
    y = li.add_col(SparseCol {
        .lowb = -10.0,
        .uppb = 10.0,
    });
    v = li.add_col(SparseCol {
        .lowb = 1.0,  // v >= 1 as a column bound
        .uppb = LinearProblem::DblMax,
    });

    SparseRow r_up;  // v - y <= 3
    r_up[v] = 1.0;
    r_up[y] = -1.0;
    r_up.lowb = -LinearProblem::DblMax;
    r_up.uppb = 3.0;
    (void)li.add_row(r_up);  // NOLINT

    SparseRow r_dn;  // v + y <= 3
    r_dn[v] = 1.0;
    r_dn[y] = 1.0;
    r_dn.lowb = -LinearProblem::DblMax;
    r_dn.uppb = 3.0;
    (void)li.add_row(r_dn);  // NOLINT

    auto res = li.initial_solve();
    REQUIRE(res.has_value());
    REQUIRE(li.is_optimal());

    li.set_col(y, trial);
    svar.set_col_sol(trial);

    links = {
        {
            .source_col = ColIndex {99},
            .dependent_col = y,
            .target_phase_index = PhaseIndex {1},
            .trial_value = trial,
            .source_low = -10.0,
            .source_upp = 10.0,
            .state_var = &svar,
        },
    };
  }
};

}  // namespace

TEST_CASE(  // NOLINT
    "elastic literature — Murphy 1-D: trial y=-5 gives SINF=3 and cut y >= -2")
{
  // At y = -5: v <= 3 + y = -2 conflicts with v >= 1.  The nearest
  // feasible state is y = -2, so the minimum-slack phase-1 optimum
  // activates sdn = 3 (SINF = 3 at unit penalty) and the fixing-row
  // dual is -1 (pi = +1).  Known answer (Murphy eq. 33): cut y >= -2.
  MurphyFixture fx {-5.0};

  auto elastic =
      elastic_filter_solve(fx.li, fx.links, kUnitPenalty, SolverOptions {});
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);
  REQUIRE(elastic->link_infos.size() == 1);
  REQUIRE(elastic->link_infos[0].relaxed);

  // Phase-1 SINF: obj = unit penalty x total slack activation = 3.
  CHECK(elastic->clone.get_obj_value() == doctest::Approx(3.0));

  // The clone moved y to the feasibility boundary y = -2.
  CHECK(elastic->clone.get_col_sol_raw()[fx.y] == doctest::Approx(-2.0));

  SUBCASE("aggregated feasibility cut == y >= -2")
  {
    auto cut = build_feasibility_cut_physical(
        fx.links, elastic->link_infos, elastic->clone, kCutCoeffEps, 0);

    REQUIRE(cut.cmap.contains(ColIndex {99}));
    const double pi = cut.cmap.at(ColIndex {99});
    // Unit slack cost -> unit dual (PLP "duals ~ +/-1" property).
    CHECK(pi == doctest::Approx(1.0));
    CHECK(pi > 0.0);
    // Up to positive scaling the cut is y >= -2: rhs / pi = -2.
    CHECK(cut.lowb / pi == doctest::Approx(-2.0));

    // Validity: cuts off the trial, keeps the known feasible set.
    CHECK(cuts_off(cut, {{ColIndex {99}, -5.0}}));
    CHECK(keeps(cut, {{ColIndex {99}, -2.0}}));  // boundary
    CHECK(keeps(cut, {{ColIndex {99}, 0.0}}));
    CHECK(keeps(cut, {{ColIndex {99}, 2.0}}));
  }

  SUBCASE("per-link multi-cut agrees with the aggregated cut")
  {
    auto cuts =
        build_multi_cuts(*elastic, fx.links, {}, kCutCoeffEps, /*niter=*/0);
    REQUIRE(cuts.size() == 1);
    const auto& cut = cuts.front();
    REQUIRE(cut.cmap.contains(ColIndex {99}));
    const double pi = cut.cmap.at(ColIndex {99});
    CHECK(pi > 0.0);
    CHECK(cut.lowb / pi == doctest::Approx(-2.0));
  }
}

TEST_CASE(  // NOLINT
    "elastic literature — Murphy 1-D symmetric: trial y=+5 gives cut y <= 2")
{
  // Mirror case: at y = +5 the binding constraint is v <= 3 - y, the
  // nearest feasible state is y = +2 (sup = 3 activates, dx = -3) and
  // the fixing-row dual is +1 (pi = -1).  Known answer: cut y <= 2,
  // emitted in >=-form as  -1 * y >= -2.
  MurphyFixture fx {5.0};

  auto elastic =
      elastic_filter_solve(fx.li, fx.links, kUnitPenalty, SolverOptions {});
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);

  CHECK(elastic->clone.get_obj_value() == doctest::Approx(3.0));
  CHECK(elastic->clone.get_col_sol_raw()[fx.y] == doctest::Approx(2.0));

  auto cut = build_feasibility_cut_physical(
      fx.links, elastic->link_infos, elastic->clone, kCutCoeffEps, 0);

  REQUIRE(cut.cmap.contains(ColIndex {99}));
  const double pi = cut.cmap.at(ColIndex {99});
  CHECK(pi == doctest::Approx(-1.0));
  CHECK(pi < 0.0);
  // pi * y >= pi * 2  <=>  y <= 2.
  CHECK(cut.lowb / pi == doctest::Approx(2.0));

  CHECK(cuts_off(cut, {{ColIndex {99}, 5.0}}));
  CHECK(keeps(cut, {{ColIndex {99}, 2.0}}));  // boundary
  CHECK(keeps(cut, {{ColIndex {99}, 0.0}}));
  CHECK(keeps(cut, {{ColIndex {99}, -2.0}}));
}

// ---------------------------------------------------------------------------
// 2. Birge & Louveaux §5.1 induced-feasibility example
//
//    second stage:  3 y1 + 2 y2 <= x1
//                   2 y1 + 5 y2 <= x2
//                   0.8 xi_i <= y_i <= xi_i,  xi = (6, 8)
//
// At x = (0, 0) the stage is infeasible.  B&L's certificate sigma =
// (3, 1) yields the aggregated cut 3 x1 + x2 >= 123.2.  gtopt's
// elastic phase-1 LP produces the (equally valid) certificate
// sigma = (1, 1):  x1 + x2 >= 68.8, plus the per-variable projections
// x1 >= 27.2 and x2 >= 41.6 in multi-cut mode (note 3*27.2 + 41.6 =
// 123.2, recovering B&L's rhs).  Coefficient checks therefore assert
// gtopt's normalization and VALIDITY against the known region, per
// the classical-certificate equivalence.
// ---------------------------------------------------------------------------
namespace
{

struct BirgeLouveauxFixture
{
  LinearInterface li {};
  ColIndex x1 {unknown_index};
  ColIndex x2 {unknown_index};
  StateVariable sv1 {make_state_var(ColIndex {99})};
  StateVariable sv2 {make_state_var(ColIndex {100})};
  std::vector<StateVarLink> links {};

  BirgeLouveauxFixture(double trial1, double trial2)
  {
    constexpr double xi1 = 6.0;
    constexpr double xi2 = 8.0;

    x1 = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 200.0,
    });
    x2 = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 200.0,
    });
    const auto y1 = li.add_col(SparseCol {
        .lowb = 0.8 * xi1,
        .uppb = xi1,
    });
    const auto y2 = li.add_col(SparseCol {
        .lowb = 0.8 * xi2,
        .uppb = xi2,
    });

    SparseRow r1;  // 3 y1 + 2 y2 - x1 <= 0
    r1[y1] = 3.0;
    r1[y2] = 2.0;
    r1[x1] = -1.0;
    r1.lowb = -LinearProblem::DblMax;
    r1.uppb = 0.0;
    (void)li.add_row(r1);  // NOLINT

    SparseRow r2;  // 2 y1 + 5 y2 - x2 <= 0
    r2[y1] = 2.0;
    r2[y2] = 5.0;
    r2[x2] = -1.0;
    r2.lowb = -LinearProblem::DblMax;
    r2.uppb = 0.0;
    (void)li.add_row(r2);  // NOLINT

    auto res = li.initial_solve();
    REQUIRE(res.has_value());
    REQUIRE(li.is_optimal());

    li.set_col(x1, trial1);
    li.set_col(x2, trial2);
    sv1.set_col_sol(trial1);
    sv2.set_col_sol(trial2);

    links = {
        {
            .source_col = ColIndex {99},
            .dependent_col = x1,
            .target_phase_index = PhaseIndex {1},
            .trial_value = trial1,
            .source_low = 0.0,
            .source_upp = 200.0,
            .state_var = &sv1,
        },
        {
            .source_col = ColIndex {100},
            .dependent_col = x2,
            .target_phase_index = PhaseIndex {1},
            .trial_value = trial2,
            .source_low = 0.0,
            .source_upp = 200.0,
            .state_var = &sv2,
        },
    };
  }
};

}  // namespace

TEST_CASE(  // NOLINT
    "elastic literature — Birge&Louveaux induced-feasibility cut at x=(0,0)")
{
  // min over the y-box of each row's LHS: 3*4.8 + 2*6.4 = 27.2 and
  // 2*4.8 + 5*6.4 = 41.6, so the true region is {x1 >= 27.2,
  // x2 >= 41.6} and the phase-1 SINF at x = (0,0) is 27.2 + 41.6 =
  // 68.8.
  BirgeLouveauxFixture fx {0.0, 0.0};

  auto elastic =
      elastic_filter_solve(fx.li, fx.links, kUnitPenalty, SolverOptions {});
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);

  CHECK(elastic->clone.get_obj_value() == doctest::Approx(68.8));

  SUBCASE("aggregated cut: sigma=(1,1) certificate, valid vs known region")
  {
    auto cut = build_feasibility_cut_physical(
        fx.links, elastic->link_infos, elastic->clone, kCutCoeffEps, 0);

    // gtopt's unit-penalty phase-1 gives sigma = (1, 1); B&L's
    // sigma = (3, 1) with rhs 123.2 is a different valid certificate
    // of the same region.  Both coefficients must be positive.
    REQUIRE(cut.cmap.contains(ColIndex {99}));
    REQUIRE(cut.cmap.contains(ColIndex {100}));
    const double p1 = cut.cmap.at(ColIndex {99});
    const double p2 = cut.cmap.at(ColIndex {100});
    CHECK(p1 == doctest::Approx(1.0));
    CHECK(p2 == doctest::Approx(1.0));
    // rhs at gtopt's normalization: 1*27.2 + 1*41.6 = 68.8.
    CHECK(cut.lowb == doctest::Approx(68.8));

    // Validity: must cut off the infeasible trial x = (0, 0)...
    CHECK(cuts_off(cut,
                   {
                       {ColIndex {99}, 0.0},
                       {ColIndex {100}, 0.0},
                   }));
    // ...and must NOT cut off the known feasible corner (27.2, 41.6)
    // nor an interior point of the region.
    CHECK(keeps(cut,
                {
                    {ColIndex {99}, 27.2},
                    {ColIndex {100}, 41.6},
                }));
    CHECK(keeps(cut,
                {
                    {ColIndex {99}, 30.0},
                    {ColIndex {100}, 45.0},
                }));
  }

  SUBCASE("multi-cut: PLP-style per-variable projections 27.2 / 41.6")
  {
    auto cuts =
        build_multi_cuts(*elastic, fx.links, {}, kCutCoeffEps, /*niter=*/0);
    REQUIRE(cuts.size() == 2);

    const SparseRow* c1 = nullptr;
    const SparseRow* c2 = nullptr;
    for (const auto& c : cuts) {
      if (c.cmap.contains(ColIndex {99})) {
        c1 = &c;
      }
      if (c.cmap.contains(ColIndex {100})) {
        c2 = &c;
      }
    }
    REQUIRE(c1 != nullptr);
    REQUIRE(c2 != nullptr);

    // x1 >= 27.2 (implied bound = rhs / pi).
    CHECK(c1->lowb / c1->cmap.at(ColIndex {99}) == doctest::Approx(27.2));
    // x2 >= 41.6 — the single-variable PLP-style projection bound.
    CHECK(c2->lowb / c2->cmap.at(ColIndex {100}) == doctest::Approx(41.6));
  }
}

// ---------------------------------------------------------------------------
// 3. Diniz two-area feasibility facets (optimization-online 5186, Table 1)
//
// 2 reservoirs, 2 periods, demand 50/period/area, inflows a1 = (10, 5),
// a2 = (20, 10), thermal caps 10 (area 1) and 20 (area 2), interchange
// cap 10/period.  Deficit-free region on initial storages (x1, x2):
//
//     x1 >= 45,   x2 >= 10,   x1 + x2 >= 95
//
// Derivation: area-1 2-period energy balance needs u1 total >= 60 with
// only x1 + 15 available -> x1 >= 45; area 2 symmetric -> x2 >= 10;
// system-wide hydro must cover 200 - 60 (thermal) = 140 with inflow
// total 45 -> x1 + x2 >= 95.
// ---------------------------------------------------------------------------
namespace
{

struct DinizTwoAreaFixture
{
  LinearInterface li {};
  ColIndex x1 {unknown_index};
  ColIndex x2 {unknown_index};
  StateVariable sv1 {make_state_var(ColIndex {99})};
  StateVariable sv2 {make_state_var(ColIndex {100})};
  std::vector<StateVarLink> links {};

  DinizTwoAreaFixture(double trial1, double trial2)
  {
    auto col = [this](double lo, double up)
    { return li.add_col(SparseCol {.lowb = lo, .uppb = up}); };

    x1 = col(0.0, 200.0);
    x2 = col(0.0, 200.0);
    // Hydro releases per reservoir per period.
    const auto u11 = col(0.0, LinearProblem::DblMax);
    const auto u12 = col(0.0, LinearProblem::DblMax);
    const auto u21 = col(0.0, LinearProblem::DblMax);
    const auto u22 = col(0.0, LinearProblem::DblMax);
    // Thermal: cap 10 in area 1, cap 20 in area 2.
    const auto g11 = col(0.0, 10.0);
    const auto g12 = col(0.0, 10.0);
    const auto g21 = col(0.0, 20.0);
    const auto g22 = col(0.0, 20.0);
    // Interchange area1 -> area2, cap 10 per period (bidirectional).
    const auto f1 = col(-10.0, 10.0);
    const auto f2 = col(-10.0, 10.0);
    // End-of-period storages.
    const auto s11 = col(0.0, 200.0);
    const auto s12 = col(0.0, 200.0);
    const auto s21 = col(0.0, 200.0);
    const auto s22 = col(0.0, 200.0);

    auto eq = [this](SparseRow r, double rhs)
    {
      r.lowb = rhs;
      r.uppb = rhs;
      (void)li.add_row(r);  // NOLINT
    };

    // Load balance, demand 50 per area per period.
    SparseRow b11;  // u11 + g11 - f1 = 50
    b11[u11] = 1.0;
    b11[g11] = 1.0;
    b11[f1] = -1.0;
    eq(b11, 50.0);
    SparseRow b21;  // u21 + g21 + f1 = 50
    b21[u21] = 1.0;
    b21[g21] = 1.0;
    b21[f1] = 1.0;
    eq(b21, 50.0);
    SparseRow b12;  // u12 + g12 - f2 = 50
    b12[u12] = 1.0;
    b12[g12] = 1.0;
    b12[f2] = -1.0;
    eq(b12, 50.0);
    SparseRow b22;  // u22 + g22 + f2 = 50
    b22[u22] = 1.0;
    b22[g22] = 1.0;
    b22[f2] = 1.0;
    eq(b22, 50.0);

    // Storage dynamics: s = prev + inflow - u  (no spill needed in the
    // probed range).  Written as  prev - u - s = -inflow.
    SparseRow d11;  // x1 + 10 - u11 - s11 = 0
    d11[x1] = 1.0;
    d11[u11] = -1.0;
    d11[s11] = -1.0;
    eq(d11, -10.0);
    SparseRow d12;  // s11 + 5 - u12 - s12 = 0
    d12[s11] = 1.0;
    d12[u12] = -1.0;
    d12[s12] = -1.0;
    eq(d12, -5.0);
    SparseRow d21;  // x2 + 20 - u21 - s21 = 0
    d21[x2] = 1.0;
    d21[u21] = -1.0;
    d21[s21] = -1.0;
    eq(d21, -20.0);
    SparseRow d22;  // s21 + 10 - u22 - s22 = 0
    d22[s21] = 1.0;
    d22[u22] = -1.0;
    d22[s22] = -1.0;
    eq(d22, -10.0);

    auto res = li.initial_solve();
    REQUIRE(res.has_value());
    REQUIRE(li.is_optimal());

    li.set_col(x1, trial1);
    li.set_col(x2, trial2);
    sv1.set_col_sol(trial1);
    sv2.set_col_sol(trial2);

    links = {
        {
            .source_col = ColIndex {99},
            .dependent_col = x1,
            .target_phase_index = PhaseIndex {1},
            .trial_value = trial1,
            .source_low = 0.0,
            .source_upp = 200.0,
            .state_var = &sv1,
        },
        {
            .source_col = ColIndex {100},
            .dependent_col = x2,
            .target_phase_index = PhaseIndex {1},
            .trial_value = trial2,
            .source_low = 0.0,
            .source_upp = 200.0,
            .state_var = &sv2,
        },
    };
  }
};

/// Points that must never be cut off: vertices + deep interior of the
/// true deficit-free region {x1>=45, x2>=10, x1+x2>=95}.
[[nodiscard]] std::vector<Point> diniz_feasible_points()
{
  return {
      {{ColIndex {99}, 45.0}, {ColIndex {100}, 50.0}},  // vertex 45+50=95
      {{ColIndex {99}, 85.0}, {ColIndex {100}, 10.0}},  // vertex 85+10=95
      {{ColIndex {99}, 60.0}, {ColIndex {100}, 40.0}},  // interior
      {{ColIndex {99}, 100.0}, {ColIndex {100}, 100.0}},  // deep interior
  };
}

}  // namespace

TEST_CASE(  // NOLINT
    "elastic literature — Diniz two-area: single-variable facets x1>=45, "
    "x2>=10")
{
  SUBCASE("probe (44, 95) recovers facet x1 >= 45")
  {
    DinizTwoAreaFixture fx {44.0, 95.0};
    auto elastic =
        elastic_filter_solve(fx.li, fx.links, kUnitPenalty, SolverOptions {});
    REQUIRE(elastic.has_value());
    REQUIRE(elastic->solved);
    // Minimum slack: move x1 44 -> 45, i.e. SINF = 1.
    CHECK(elastic->clone.get_obj_value() == doctest::Approx(1.0));

    auto cut = build_feasibility_cut_physical(
        fx.links, elastic->link_infos, elastic->clone, kCutCoeffEps, 0);

    // The cut is the pure x1 facet: no x2 involvement (x2's slack
    // stays at zero, so the PLP dx filter drops that link).
    REQUIRE(cut.cmap.contains(ColIndex {99}));
    CHECK_FALSE(cut.cmap.contains(ColIndex {100}));
    const double pi = cut.cmap.at(ColIndex {99});
    CHECK(pi > 0.0);
    CHECK(cut.lowb / pi == doctest::Approx(45.0));

    CHECK(cuts_off(cut,
                   {
                       {ColIndex {99}, 44.0},
                       {ColIndex {100}, 95.0},
                   }));
    for (const auto& p : diniz_feasible_points()) {
      CHECK(keeps(cut, p));
    }

    // Multi-cut form: exactly one per-link cut, same facet.
    auto cuts =
        build_multi_cuts(*elastic, fx.links, {}, kCutCoeffEps, /*niter=*/0);
    REQUIRE(cuts.size() == 1);
    CHECK(cuts.front().lowb / cuts.front().cmap.at(ColIndex {99})
          == doctest::Approx(45.0));
  }

  SUBCASE("probe (95, 9) recovers facet x2 >= 10")
  {
    DinizTwoAreaFixture fx {95.0, 9.0};
    auto elastic =
        elastic_filter_solve(fx.li, fx.links, kUnitPenalty, SolverOptions {});
    REQUIRE(elastic.has_value());
    REQUIRE(elastic->solved);
    CHECK(elastic->clone.get_obj_value() == doctest::Approx(1.0));

    auto cut = build_feasibility_cut_physical(
        fx.links, elastic->link_infos, elastic->clone, kCutCoeffEps, 0);

    REQUIRE(cut.cmap.contains(ColIndex {100}));
    CHECK_FALSE(cut.cmap.contains(ColIndex {99}));
    const double pi = cut.cmap.at(ColIndex {100});
    CHECK(pi > 0.0);
    CHECK(cut.lowb / pi == doctest::Approx(10.0));

    CHECK(cuts_off(cut,
                   {
                       {ColIndex {99}, 95.0},
                       {ColIndex {100}, 9.0},
                   }));
    for (const auto& p : diniz_feasible_points()) {
      CHECK(keeps(cut, p));
    }
  }
}

TEST_CASE(  // NOLINT
    "elastic literature — Diniz two-area coupling facet x1 + x2 >= 95"
    * doctest::may_fail())
{
  // FIXME(coupling-facet over-cut): the probe (46, 48) violates ONLY
  // the coupling facet x1 + x2 >= 95 (sum = 94).  The elastic optimum
  // is degenerate — any split sdn1 + sdn2 = 1 is optimal — and the
  // vertex the solver picks turns the aggregated cut into a
  // single-variable projection (e.g. x2 >= 49) after the PLP dx filter
  // drops the zero-movement link.  Such a projection cuts off feasible
  // interior points like (60, 40): the single-variable PLP cut family
  // cannot represent a coupling facet (Diniz's core observation).  The
  // correct behavior asserted here is a valid cut that separates the
  // probe WITHOUT excluding any feasible point; gtopt may currently
  // fail the keeps() checks depending on the solver's vertex choice.
  DinizTwoAreaFixture fx {46.0, 48.0};
  auto elastic =
      elastic_filter_solve(fx.li, fx.links, kUnitPenalty, SolverOptions {});
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);
  // Minimum slack = distance to the coupling facet = 1.
  CHECK(elastic->clone.get_obj_value() == doctest::Approx(1.0));

  auto cut = build_feasibility_cut_physical(
      fx.links, elastic->link_infos, elastic->clone, kCutCoeffEps, 0);

  // Must cut off the probe...
  CHECK(cuts_off(cut,
                 {
                     {ColIndex {99}, 46.0},
                     {ColIndex {100}, 48.0},
                 }));
  // ...and must keep every point of the true deficit-free region.
  for (const auto& p : diniz_feasible_points()) {
    CHECK(keeps(cut, p));
  }
}

// ---------------------------------------------------------------------------
// 4. PLP-style mini-cascade backward recursion (cf. SDDP.jl
//    theory_intro, with a thermal cap): reservoir cap 200, demand
//    150/stage, thermal cap 100, inflow 0.  The stage LP
//
//        u + g = 150,  0 <= g <= 100,  v_prev - u - s_end = 0
//
//    is feasible iff v_prev >= 50.  PLP's AgrElastici would emit the
//    single-variable cut `cf: vf 1.0 / rhsi: 50`.
// ---------------------------------------------------------------------------
namespace
{

struct MiniCascadeFixture
{
  LinearInterface li {};
  ColIndex v_prev {unknown_index};
  StateVariable svar {make_state_var(ColIndex {99})};
  std::vector<StateVarLink> links {};

  explicit MiniCascadeFixture(double trial)
  {
    v_prev = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 200.0,  // reservoir cap
    });
    const auto u = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = LinearProblem::DblMax,
    });
    const auto g = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 100.0,  // thermal cap
    });
    const auto s_end = li.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 200.0,
    });

    SparseRow demand;  // u + g = 150
    demand[u] = 1.0;
    demand[g] = 1.0;
    demand.lowb = 150.0;
    demand.uppb = 150.0;
    (void)li.add_row(demand);  // NOLINT

    SparseRow water;  // v_prev - u - s_end = 0 (inflow 0)
    water[v_prev] = 1.0;
    water[u] = -1.0;
    water[s_end] = -1.0;
    water.lowb = 0.0;
    water.uppb = 0.0;
    (void)li.add_row(water);  // NOLINT

    auto res = li.initial_solve();
    REQUIRE(res.has_value());
    REQUIRE(li.is_optimal());

    li.set_col(v_prev, trial);
    svar.set_col_sol(trial);

    links = {
        {
            .source_col = ColIndex {99},
            .dependent_col = v_prev,
            .target_phase_index = PhaseIndex {1},
            .trial_value = trial,
            .source_low = 0.0,
            .source_upp = 200.0,
            .state_var = &svar,
        },
    };
  }
};

}  // namespace

TEST_CASE(  // NOLINT
    "elastic literature — mini-cascade: trial v=30 emits exactly v_prev >= 50")
{
  MiniCascadeFixture fx {30.0};

  auto elastic =
      elastic_filter_solve(fx.li, fx.links, kUnitPenalty, SolverOptions {});
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);

  // Minimum slack: v 30 -> 50 (u >= 50 forced by g <= 100), SINF = 20.
  CHECK(elastic->clone.get_obj_value() == doctest::Approx(20.0));

  SUBCASE("aggregated cut == PLP `cf: vf 1.0 / rhsi: 50`")
  {
    auto cut = build_feasibility_cut_physical(
        fx.links, elastic->link_infos, elastic->clone, kCutCoeffEps, 0);

    REQUIRE(cut.cmap.contains(ColIndex {99}));
    CHECK(cut.cmap.at(ColIndex {99}) == doctest::Approx(1.0));
    CHECK(cut.lowb == doctest::Approx(50.0));

    CHECK(cuts_off(cut, {{ColIndex {99}, 30.0}}));
    CHECK(cuts_off(cut, {{ColIndex {99}, 49.0}}));
    CHECK(keeps(cut, {{ColIndex {99}, 50.0}}));  // boundary
    CHECK(keeps(cut, {{ColIndex {99}, 200.0}}));
  }

  SUBCASE("multi-cut form: same single-variable bound")
  {
    auto cuts =
        build_multi_cuts(*elastic, fx.links, {}, kCutCoeffEps, /*niter=*/0);
    REQUIRE(cuts.size() == 1);
    const auto& cut = cuts.front();
    const double pi = cut.cmap.at(ColIndex {99});
    CHECK(pi > 0.0);
    CHECK(cut.lowb / pi == doctest::Approx(50.0));
  }
}

TEST_CASE(  // NOLINT
    "elastic literature — mini-cascade: feasible trial v=60 emits NO cut")
{
  // v_prev = 60 >= 50 is stage-feasible: the elastic clone activates
  // zero slack (SINF = 0) and both cut builders must produce nothing —
  // the PLP dx filter drops zero-activation links regardless of any
  // degenerate dual noise on the fixing row.
  MiniCascadeFixture fx {60.0};

  auto elastic =
      elastic_filter_solve(fx.li, fx.links, kUnitPenalty, SolverOptions {});
  REQUIRE(elastic.has_value());
  REQUIRE(elastic->solved);
  CHECK(elastic->clone.get_obj_value() == doctest::Approx(0.0));

  auto cut = build_feasibility_cut_physical(
      fx.links, elastic->link_infos, elastic->clone, kCutCoeffEps, 0);
  CHECK(cut.cmap.empty());
  CHECK(cut.lowb == doctest::Approx(0.0));

  auto cuts =
      build_multi_cuts(*elastic, fx.links, {}, kCutCoeffEps, /*niter=*/0);
  CHECK(cuts.empty());
}
