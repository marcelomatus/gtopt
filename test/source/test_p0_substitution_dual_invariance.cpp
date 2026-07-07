// SPDX-License-Identifier: BSD-3-Clause
//
// P0 substitution dual invariance — proves that folding the
// `fail_col` + coupling row into the served column's cost (via
// `add_obj_constant`) preserves the row duals of the surrounding
// balance rows.
//
// Two LP shape refactors today eliminated separate `fail_col` +
// coupling row pairs and folded them into the served column's cost:
//
//  - Demand at `source/demand_lp.cpp:220-289`:
//      pre-P0  : `load_col + fail_col = lmax`,  fail_col cost = +c
//      post-P0 : drop fail_col + capacity row, load_col cost = -c,
//                add_obj_constant(+c · lmax)
//      where c = block_ecost(demand_fail_cost).
//
//  - FlowRight at `source/flow_right_lp.cpp` (P0 landed today):
//      pre-P0  : `flow_col + fail_col >= discharge`,  fail_col cost = +c
//      post-P0 : drop fail_col + coupling row, flow_col cost = -c,
//                add_obj_constant(+c · discharge)
//      where c = (use_value + fcost) · cf.
//
// Algebraic claim: at the LP optimum, primal AND dual values match
// between the two formulations.  In particular the dual of the bus
// balance row (for Demand) and the dual of the junction balance row
// (for FlowRight) are unchanged — both refactors touch only the
// served column's cost coefficient and an objective constant.
//
// This file hand-builds both LP shapes via `LinearProblem` (without
// going through DemandLP / FlowRightLP) so that the algebra is laid
// out explicitly and the dual comparison is unambiguous.

#include <cmath>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;

namespace
{

// Compact return type for the dual-extraction helpers.
struct DualResult
{
  double bus_balance_dual {0.0};
  double obj_value {0.0};
};

// Build and solve the pre-P0 Demand LP shape:
//   load_col ∈ [0, lmax],      cost 0
//   fail_col ∈ [0, +inf),      cost +c
//   gen_col  ∈ [0, gen_cap],   cost +gcost
//   capacity row :  load + fail = lmax     (equality, the row we drop)
//   bus_balance  :  gen − load    = 0      (equality, the row we observe)
//
// Returns the bus_balance dual and the optimal objective value.
[[nodiscard]] DualResult solve_demand_pre_p0(double lmax,
                                             double gen_cap,
                                             double gcost,
                                             double fcost)
{
  LinearProblem lp("demand_pre_p0");

  const auto load_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = lmax,
      .cost = 0.0,
  });
  const auto fail_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = LinearProblem::DblMax,
      .cost = fcost,
  });
  const auto gen_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = gen_cap,
      .cost = gcost,
  });

  auto cap_row = SparseRow {};
  cap_row[load_col] = 1.0;
  cap_row[fail_col] = 1.0;
  cap_row.equal(lmax);
  std::ignore = lp.add_row(std::move(cap_row));

  auto bus_balance = SparseRow {};
  bus_balance[gen_col] = 1.0;
  bus_balance[load_col] = -1.0;
  bus_balance.equal(0.0);
  const auto bus_balance_row = lp.add_row(std::move(bus_balance));

  const auto flat = lp.flatten({
      .equilibration_method = LpEquilibrationMethod::none,
  });
  LinearInterface li("", flat);
  std::ignore = li.initial_solve({});
  REQUIRE(li.is_optimal());

  const auto duals = li.get_row_dual();
  REQUIRE(duals.size() == 2);

  return {
      .bus_balance_dual = duals[bus_balance_row],
      .obj_value = li.get_obj_value(),
  };
}

// Build and solve the post-P0 Demand LP shape:
//   load_col ∈ [0, lmax],      cost -c        (substituted)
//   gen_col  ∈ [0, gen_cap],   cost +gcost
//   bus_balance :  gen − load = 0             (unchanged)
//   obj_constant += +c · lmax                 (rides the substitution)
[[nodiscard]] DualResult solve_demand_post_p0(double lmax,
                                              double gen_cap,
                                              double gcost,
                                              double fcost)
{
  LinearProblem lp("demand_post_p0");

  const auto load_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = lmax,
      .cost = -fcost,
  });
  const auto gen_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = gen_cap,
      .cost = gcost,
  });

  auto bus_balance = SparseRow {};
  bus_balance[gen_col] = 1.0;
  bus_balance[load_col] = -1.0;
  bus_balance.equal(0.0);
  const auto bus_balance_row = lp.add_row(std::move(bus_balance));

  lp.add_obj_constant(fcost * lmax);

  const auto flat = lp.flatten({
      .equilibration_method = LpEquilibrationMethod::none,
  });
  LinearInterface li("", flat);
  std::ignore = li.initial_solve({});
  REQUIRE(li.is_optimal());

  const auto duals = li.get_row_dual();
  REQUIRE(duals.size() == 1);

  return {
      .bus_balance_dual = duals[bus_balance_row],
      .obj_value = li.get_obj_value(),
  };
}

// Build and solve the pre-P0 FlowRight LP shape:
//   flow_col ∈ [0, discharge],       cost -uv  (use-value incentive)
//   fail_col ∈ [0, +inf),            cost +c   (deficit penalty)
//   src_col  ∈ [0, src_cap],         cost +scost (upstream supply cost)
//   coupling :  flow + fail >= discharge      (inequality, the row we drop)
//   junction :  src - flow = 0                (equality, the row we observe)
//
// Note: the pre-P0 flow_col is capped at `discharge` (per the spec), which
// is exactly what the production single-column path enforces — the upper
// bound is `target` (= discharge) until the bonus region is unlocked by a
// non-zero `uvalue`.  This is the structural invariant that makes the
// substitution dual-invariant: when `flow ≤ discharge` is forced by the
// column bound, `fail = discharge − flow ≥ 0` is automatic and the
// substituted form's negative-cost incentive cannot push flow beyond
// `discharge` to make `fail` go negative.
//
// `c` is the per-unit penalty in the substituted form: c = fcost (for the
// soft-target single-column case).  `uv` is the LP cost on the flow column
// pre-P0, which is `-use_value` (FlowRight rewards use under target).
//
// Returns the junction balance dual and optimal objective value.
[[nodiscard]] DualResult solve_flow_right_pre_p0(double discharge,
                                                 double src_cap,
                                                 double scost,
                                                 double use_value,
                                                 double fcost)
{
  LinearProblem lp("flow_right_pre_p0");

  const auto flow_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = discharge,
      .cost = -use_value,
  });
  const auto fail_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = LinearProblem::DblMax,
      .cost = fcost,
  });
  const auto src_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = src_cap,
      .cost = scost,
  });

  auto coupling = SparseRow {};
  coupling[flow_col] = 1.0;
  coupling[fail_col] = 1.0;
  coupling.greater_equal(discharge);
  std::ignore = lp.add_row(std::move(coupling));

  auto junction = SparseRow {};
  junction[src_col] = 1.0;
  junction[flow_col] = -1.0;
  junction.equal(0.0);
  const auto junction_row = lp.add_row(std::move(junction));

  const auto flat = lp.flatten({
      .equilibration_method = LpEquilibrationMethod::none,
  });
  LinearInterface li("", flat);
  std::ignore = li.initial_solve({});
  REQUIRE(li.is_optimal());

  const auto duals = li.get_row_dual();
  REQUIRE(duals.size() == 2);

  return {
      .bus_balance_dual = duals[junction_row],
      .obj_value = li.get_obj_value(),
  };
}

// Build and solve the post-P0 FlowRight LP shape:
//   flow_col ∈ [0, discharge],       cost -(use_value + fcost)
//   src_col  ∈ [0, src_cap],         cost +scost
//   junction :  src - flow = 0                (unchanged)
//   obj_constant += +fcost · discharge        (rides the substitution)
[[nodiscard]] DualResult solve_flow_right_post_p0(double discharge,
                                                  double src_cap,
                                                  double scost,
                                                  double use_value,
                                                  double fcost)
{
  LinearProblem lp("flow_right_post_p0");

  const auto flow_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = discharge,
      .cost = -(use_value + fcost),
  });
  const auto src_col = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = src_cap,
      .cost = scost,
  });

  auto junction = SparseRow {};
  junction[src_col] = 1.0;
  junction[flow_col] = -1.0;
  junction.equal(0.0);
  const auto junction_row = lp.add_row(std::move(junction));

  lp.add_obj_constant(fcost * discharge);

  const auto flat = lp.flatten({
      .equilibration_method = LpEquilibrationMethod::none,
  });
  LinearInterface li("", flat);
  std::ignore = li.initial_solve({});
  REQUIRE(li.is_optimal());

  const auto duals = li.get_row_dual();
  REQUIRE(duals.size() == 1);

  return {
      .bus_balance_dual = duals[junction_row],
      .obj_value = li.get_obj_value(),
  };
}

}  // namespace

TEST_CASE("P0 substitution preserves bus and junction balance row duals")
{
  // The dual-invariance epsilon — both shapes solve the same physical
  // problem so they should agree well below this tolerance, but we
  // pick a moderately generous bound to absorb solver-side numerical
  // noise (CLP and HiGHS differ slightly on how degenerate vertices
  // are reported).
  constexpr double kDualTol = 1e-9;

  SUBCASE("Demand: bus balance dual unchanged under curtailment")
  {
    // Curtailment scenario: gen_cap < lmax forces load to its
    // upper bound being unattainable.  In the pre-P0 form fail
    // takes the slack; in the post-P0 form the same primal is
    // reached with load_col interior and a negative cost coefficient.
    //
    // Both LPs land at gen = gen_cap, load = gen_cap, and the bus
    // balance row dual matches.  The shadow price magnitude is
    // `fcost`: one extra MW of demand costs `fcost` to leave
    // unserved, so relaxing the bus balance by +1 MW saves `fcost`.
    // (Sign depends on solver / row-direction convention; we assert
    // pre == post and check the magnitude.)
    constexpr double lmax = 100.0;
    constexpr double gen_cap = 60.0;
    constexpr double gcost = 50.0;
    constexpr double fcost = 1000.0;

    const auto pre = solve_demand_pre_p0(lmax, gen_cap, gcost, fcost);
    const auto post = solve_demand_post_p0(lmax, gen_cap, gcost, fcost);

    // Dual invariance — the headline assertion.
    CHECK(pre.bus_balance_dual
          == doctest::Approx(post.bus_balance_dual).epsilon(kDualTol));

    // Magnitude check: curtailment marginal cost is `fcost`.
    CHECK(std::abs(post.bus_balance_dual)
          == doctest::Approx(fcost).epsilon(1e-6));

    // Sanity: the objectives must also agree (the obj_constant
    // exactly accounts for the substitution).  pre-P0 cost =
    // 0·load + fcost·fail + gcost·gen = fcost·40 + gcost·60.
    // post-P0 cost = -fcost·60 + gcost·60 + fcost·100 = fcost·40 + gcost·60.
    CHECK(pre.obj_value == doctest::Approx(post.obj_value).epsilon(1e-6));
  }

  SUBCASE("Demand: bus balance dual unchanged when load fully served")
  {
    // Plenty of generation — load reaches its upper bound.  The bus
    // balance dual is set by the generator's cost (the generator is
    // the marginal supplier and is itself interior).  Magnitude
    // equals `gcost`.
    constexpr double lmax = 100.0;
    constexpr double gen_cap = 500.0;
    constexpr double gcost = 50.0;
    constexpr double fcost = 1000.0;

    const auto pre = solve_demand_pre_p0(lmax, gen_cap, gcost, fcost);
    const auto post = solve_demand_post_p0(lmax, gen_cap, gcost, fcost);

    CHECK(pre.bus_balance_dual
          == doctest::Approx(post.bus_balance_dual).epsilon(kDualTol));
    CHECK(std::abs(post.bus_balance_dual)
          == doctest::Approx(gcost).epsilon(1e-6));
    CHECK(pre.obj_value == doctest::Approx(post.obj_value).epsilon(1e-6));
  }

  SUBCASE("FlowRight: junction balance dual unchanged under deficit")
  {
    // Upstream supply (`src_cap`) is the bottleneck and the right
    // is not fully covered.  Pre-P0 absorbs the gap in `fail_col`;
    // post-P0 lets `flow_col` stay interior at the same primal
    // value with `cost = -(use_value + fcost)`.
    //
    // Note: the pre-P0 flow_col upper bound is `discharge` (the
    // production single-column path's structural cap when uvalue
    // is unset), so flow can never exceed discharge in either form.
    constexpr double discharge = 80.0;
    constexpr double src_cap = 30.0;
    constexpr double scost = 5.0;
    constexpr double use_value = 10.0;
    constexpr double fcost = 5000.0;

    const auto pre =
        solve_flow_right_pre_p0(discharge, src_cap, scost, use_value, fcost);
    const auto post =
        solve_flow_right_post_p0(discharge, src_cap, scost, use_value, fcost);

    CHECK(pre.bus_balance_dual
          == doctest::Approx(post.bus_balance_dual).epsilon(kDualTol));
    CHECK(pre.obj_value == doctest::Approx(post.obj_value).epsilon(1e-6));
  }

  SUBCASE("FlowRight: junction balance dual unchanged under full coverage")
  {
    // Plenty of upstream supply; flow_col is at `discharge` (its
    // upper bound), driven there by the use_value incentive.  The
    // upstream supplier (src) is the marginal contributor:
    // junction dual magnitude equals `scost`.
    constexpr double discharge = 50.0;
    constexpr double src_cap = 500.0;
    constexpr double scost = 5.0;
    // Use a strictly positive uvalue so the LP has a non-trivial
    // incentive to actually deliver `discharge` (rather than the
    // degenerate flow=0, fail=discharge corner that the
    // pre-substitution LP would also accept when uvalue=0 and
    // src cost is non-negative).
    constexpr double use_value = 10.0;
    constexpr double fcost = 5000.0;

    const auto pre =
        solve_flow_right_pre_p0(discharge, src_cap, scost, use_value, fcost);
    const auto post =
        solve_flow_right_post_p0(discharge, src_cap, scost, use_value, fcost);

    CHECK(pre.bus_balance_dual
          == doctest::Approx(post.bus_balance_dual).epsilon(kDualTol));
    CHECK(pre.obj_value == doctest::Approx(post.obj_value).epsilon(1e-6));
  }
}
