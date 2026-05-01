// SPDX-License-Identifier: BSD-3-Clause
//
// Coverage tests for the scale-critical surface of LinearInterface and
// LinearProblem.  Closes the zero-test gap on the raw/physical bound and
// scale round-trip paths exercised by SDDP's `propagate_trial_values` —
// the source of the phase-2 `reservoir_sini` double-scale-divide bug
// diagnosed on the plp_2_years case (2026-04-22).
//
// Each test targets one or more gaps (G1..G12) from the test-coverage
// audit.  The headline test (Test 1, SUBCASE
// "set_col_low (physical) → get_col_low_raw == physical / scale") is
// the one that would have caught the production bug.

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

// 2-column, 1-row toy LP with an explicit col_scale on col0.  RALCO's
// energy column uses scale ≈ √10 = 3.162 in production; we reuse that
// value so the bug arithmetic is self-documenting in the test.
constexpr double kRalcoEnergyScale = 3.162;

}  // namespace

// ────────────────────────────────────────────────────────────────────
// Test 1 — closes G1, G2, G3, G4
//
// The SUBCASE "set_col_low (physical) → get_col_low_raw == physical /
// scale" is the exact direction of the production bug; any double
// divide in the raw-setter path fails this assertion.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(
    "LinearInterface - raw col-bound setters and scale round-trip")  // NOLINT
{
  LinearProblem lp("scale_roundtrip");

  const auto c0 = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 300.0,
      .cost = 1.0,
      .scale = kRalcoEnergyScale,
  });
  const auto c1 = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .cost = 2.0,
  });

  auto r0 = SparseRow {};
  r0[c0] = 1.0;
  r0[c1] = 1.0;
  r0.bound(0.0, 100.0);
  std::ignore = lp.add_row(std::move(r0));

  LinearInterface li("", lp.flatten({}));

  REQUIRE(li.get_numcols() == 2);
  REQUIRE(li.get_col_scale(c0) == doctest::Approx(kRalcoEnergyScale));
  REQUIRE(li.get_col_scale(c1) == doctest::Approx(1.0));

  // Write a raw LP value equal to the exact ratio at which the Juan
  // bug was detected (233.44 / 3.162 ≈ 73.82).  If the raw setter
  // were silently descaling, the read-back would equal 73.82 / 3.162.
  constexpr double kRawVal = 73.82;
  li.set_col_low_raw(c0, kRawVal);
  li.set_col_upp_raw(c0, kRawVal);

  SUBCASE("get_col_low_raw returns the raw value unchanged")
  {
    const auto lo_raw = li.get_col_low_raw();
    const auto hi_raw = li.get_col_upp_raw();
    CHECK(lo_raw[c0] == doctest::Approx(kRawVal));
    CHECK(hi_raw[c0] == doctest::Approx(kRawVal));
  }

  SUBCASE("get_col_low (physical) returns raw × col_scale")
  {
    const double expected_phys = kRawVal * kRalcoEnergyScale;
    const auto lo_phys = li.get_col_low();
    const auto hi_phys = li.get_col_upp();
    CHECK(lo_phys[c0] == doctest::Approx(expected_phys));
    CHECK(hi_phys[c0] == doctest::Approx(expected_phys));
  }

  SUBCASE("set_col_low (physical) → get_col_low_raw == physical / scale")
  {
    // THIS IS THE TEST THAT WOULD HAVE CAUGHT THE PRODUCTION BUG.
    // propagate_trial_values reads back raw bounds via
    // get_col_low_raw(); the value must be physical / col_scale, not
    // physical / col_scale / col_scale.
    constexpr double kPhysVal = 233.44;
    li.set_col_low(c0, kPhysVal);
    li.set_col_upp(c0, kPhysVal);

    const auto lo_raw = li.get_col_low_raw();
    const auto hi_raw = li.get_col_upp_raw();
    const double expected_raw = kPhysVal / kRalcoEnergyScale;
    // Must be 73.83..., NOT 23.35 (double-divide) and NOT 233.44 (no divide).
    CHECK(lo_raw[c0] == doctest::Approx(expected_raw).epsilon(1e-4));
    CHECK(hi_raw[c0] == doctest::Approx(expected_raw).epsilon(1e-4));
    // Physical getter must recover the original value.
    CHECK(li.get_col_low()[c0] == doctest::Approx(kPhysVal).epsilon(1e-4));
    CHECK(li.get_col_upp()[c0] == doctest::Approx(kPhysVal).epsilon(1e-4));
  }

  SUBCASE("set_col_raw (both bounds in one call)")
  {
    constexpr double kRawPin = 50.0;
    li.set_col_raw(c0, kRawPin);
    const auto lo_raw = li.get_col_low_raw();
    const auto hi_raw = li.get_col_upp_raw();
    CHECK(lo_raw[c0] == doctest::Approx(kRawPin));
    CHECK(hi_raw[c0] == doctest::Approx(kRawPin));
  }

  SUBCASE("set_col (physical, both bounds)")
  {
    constexpr double kPhysPin = 31.62;
    li.set_col(c0, kPhysPin);
    const auto expected_raw = kPhysPin / kRalcoEnergyScale;
    CHECK(li.get_col_low_raw()[c0] == doctest::Approx(expected_raw));
    CHECK(li.get_col_upp_raw()[c0] == doctest::Approx(expected_raw));
    CHECK(li.get_col_low()[c0] == doctest::Approx(kPhysPin));
  }

  SUBCASE("c1 (unit scale) is unaffected by c0 bound changes")
  {
    const auto lo_raw = li.get_col_low_raw();
    const auto hi_raw = li.get_col_upp_raw();
    CHECK(lo_raw[c1] == doctest::Approx(0.0));
    CHECK(hi_raw[c1] == doctest::Approx(100.0));
    // Physical == raw when scale == 1.0.
    CHECK(li.get_col_low()[c1] == doctest::Approx(0.0));
    CHECK(li.get_col_upp()[c1] == doctest::Approx(100.0));
  }

  SUBCASE("normalize_bound maps DblMax to solver infinity on raw setter")
  {
    li.set_col_upp_raw(c0, LinearProblem::DblMax);
    CHECK(li.is_pos_inf(li.get_col_upp_raw()[c0]));
    li.set_col_low_raw(c0, -LinearProblem::DblMax);
    CHECK(li.is_neg_inf(li.get_col_low_raw()[c0]));
  }
}

// ────────────────────────────────────────────────────────────────────
// Test 2 — closes G5
// Covers set_col_scale / get_col_scale / get_col_scales directly, and
// verifies the physical view tracks the new scale.
// ────────────────────────────────────────────────────────────────────
TEST_CASE("LinearInterface - set_col_scale and get_col_scale")  // NOLINT
{
  // Build a solvable LP:  min x0 + x1  s.t.  x0 + x1 >= 1
  // Add the scale AFTER load_flat via set_col_scale.
  LinearProblem lp("col_scale_test");
  const auto c0 = lp.add_col(SparseCol {.uppb = 100.0, .cost = 1.0});
  const auto c1 = lp.add_col(SparseCol {.uppb = 50.0, .cost = 2.0});

  auto r0 = SparseRow {};
  r0[c0] = 1.0;
  r0[c1] = 1.0;
  r0.greater_equal(1.0);
  std::ignore = lp.add_row(std::move(r0));

  LinearInterface li("", lp.flatten({}));

  SUBCASE("default scale is 1.0")
  {
    CHECK(li.get_col_scale(c0) == doctest::Approx(1.0));
    CHECK(li.get_col_scale(c1) == doctest::Approx(1.0));
  }

  SUBCASE("set_col_scale updates get_col_scale")
  {
    li.set_col_scale(c0, 5.0);
    CHECK(li.get_col_scale(c0) == doctest::Approx(5.0));
    CHECK(li.get_col_scale(c1) == doctest::Approx(1.0));
    REQUIRE(li.get_col_scales().size() >= 1);
    CHECK(li.get_col_scales()[c0] == doctest::Approx(5.0));
  }

  SUBCASE("set_col_scale propagates to get_col_low physical view")
  {
    li.set_col_scale(c0, 5.0);
    li.set_col_low_raw(c0, 10.0);
    CHECK(li.get_col_low()[c0] == doctest::Approx(50.0));
  }

  SUBCASE("set_col_scale on out-of-range index grows the vector")
  {
    const auto sz_before = li.get_col_scales().size();
    li.set_col_scale(ColIndex {5}, 7.0);
    CHECK(li.get_col_scales().size() >= 6);
    CHECK(li.get_col_scale(ColIndex {5}) == doctest::Approx(7.0));
    // Indices between sz_before and 5 must default to 1.0.
    for (size_t i = sz_before; i < 5; ++i) {
      CHECK(li.get_col_scale(ColIndex {static_cast<int>(i)})
            == doctest::Approx(1.0));
    }
  }

  SUBCASE("get_col_scale out-of-range returns 1.0 (no throw)")
  {
    // ColIndex beyond the vector bounds — getter is defensive.
    CHECK(li.get_col_scale(ColIndex {999}) == doctest::Approx(1.0));
  }
}

// ────────────────────────────────────────────────────────────────────
// Test 3 — closes G6
// Reduced-cost scale round-trip:
//   physical_rc = raw_rc × scale_objective / col_scale
// ────────────────────────────────────────────────────────────────────
TEST_CASE(
    "LinearInterface - get_col_cost_raw and get_col_cost scale")  // NOLINT
{
  // min x0 + 2*x1  s.t.  x0 + x1 >= 5, 0 <= x0,x1 <= 20
  // Optimal: x0=5, x1=0  → rc(x0) basic (=0), rc(x1) out-of-basis > 0.
  LinearProblem lp("rc_scale");
  const auto c0 = lp.add_col(SparseCol {
      .uppb = 20.0,
      .cost = 1.0,
      .scale = 4.0,
  });
  const auto c1 = lp.add_col(SparseCol {.uppb = 20.0, .cost = 2.0});

  auto r0 = SparseRow {};
  r0[c0] = 1.0;
  r0[c1] = 1.0;
  r0.greater_equal(5.0);
  std::ignore = lp.add_row(std::move(r0));

  const auto flat = lp.flatten({.scale_objective = 1000.0});

  LinearInterface li("", flat);
  const auto status = li.initial_solve({});
  REQUIRE((status && *status == 0));
  REQUIRE(li.is_optimal());

  SUBCASE("scale_objective is picked up by load_flat")
  {
    CHECK(li.scale_objective() == doctest::Approx(1000.0));
  }

  SUBCASE("get_col_cost_raw returns raw LP-space reduced cost")
  {
    const auto rc_raw = li.get_col_cost_raw();
    REQUIRE(rc_raw.size() == 2);
    // Basic variable: reduced cost is zero at optimum.
    CHECK(rc_raw[c0] == doctest::Approx(0.0).epsilon(1e-8));
    // Non-basic variable: positive reduced cost.
    CHECK(rc_raw[c1] > 0.0);
  }

  SUBCASE("get_col_cost (physical) == rc_raw × scale_obj / col_scale")
  {
    const auto rc_raw = li.get_col_cost_raw();
    const auto rc_phys = li.get_col_cost();
    REQUIRE(rc_phys.size() == 2);
    // c0: scale=4, scale_obj=1000 → physical = raw × 1000 / 4
    CHECK(rc_phys[c0]
          == doctest::Approx(rc_raw[c0] * 1000.0 / 4.0).epsilon(1e-8));
    // c1: scale=1, scale_obj=1000 → physical = raw × 1000
    CHECK(rc_phys[c1]
          == doctest::Approx(rc_raw[c1] * 1000.0 / 1.0).epsilon(1e-8));
  }
}

// ────────────────────────────────────────────────────────────────────
// Test 4 — closes G7
// Row dual scale round-trip with equilibration:
//   physical_dual = raw_dual × scale_objective / row_scale
// ────────────────────────────────────────────────────────────────────
TEST_CASE(
    "LinearInterface - get_row_dual_raw and get_row_dual scale")  // NOLINT
{
  // min x0 + x1  s.t.  100*x0 + 100*x1 >= 500,  0 <= x0,x1 <= 10
  // row_max equilibration divides the row by 100.  Optimal: x0=5,x1=0.
  LinearProblem lp("dual_scale");
  const auto c0 = lp.add_col(SparseCol {.uppb = 10.0, .cost = 1.0});
  const auto c1 = lp.add_col(SparseCol {.uppb = 10.0, .cost = 1.0});

  auto r0 = SparseRow {};
  r0[c0] = 100.0;
  r0[c1] = 100.0;
  r0.greater_equal(500.0);
  const auto r0_idx = lp.add_row(std::move(r0));

  const auto flat = lp.flatten({
      .equilibration_method = LpEquilibrationMethod::row_max,
      .scale_objective = 1000.0,
  });

  LinearInterface li("", flat);
  const auto status = li.initial_solve({});
  REQUIRE((status && *status == 0));
  REQUIRE(li.is_optimal());
  REQUIRE(li.get_row_scale(r0_idx) == doctest::Approx(100.0));

  SUBCASE("get_row_dual_raw is in LP solver units (non-zero binding row)")
  {
    const auto dual_raw = li.get_row_dual_raw();
    REQUIRE(dual_raw.size() == 1);
    // Binding >= constraint in minimization: dual is negative or positive
    // depending on sign convention; just assert nonzero magnitude.
    CHECK(std::abs(dual_raw[r0_idx]) > 1e-8);
  }

  SUBCASE("get_row_dual (physical) == raw × scale_obj / row_scale")
  {
    const auto dual_raw = li.get_row_dual_raw();
    const auto dual_phys = li.get_row_dual();
    const double expected =
        dual_raw[r0_idx] * li.scale_objective() / li.get_row_scale(r0_idx);
    CHECK(dual_phys[r0_idx] == doctest::Approx(expected).epsilon(1e-8));
  }

  SUBCASE("get_row_scales size matches numrows when equilibrated")
  {
    CHECK(li.get_row_scales().size() == 1);
    CHECK(li.get_row_scales()[r0_idx] == doctest::Approx(100.0));
  }
}

// ────────────────────────────────────────────────────────────────────
// Test 5 — closes G8, G11
// Raw row-bound setters + physical row-bound setters with non-unit
// row_scale, and normalize_bound on the raw path.
// ────────────────────────────────────────────────────────────────────
TEST_CASE("LinearInterface - raw and physical row bound setters")  // NOLINT
{
  LinearInterface li;
  const auto c0 = li.add_col(SparseCol {.uppb = 10.0, .cost = 1.0});

  SparseRow row;
  row[c0] = 1.0;
  row.lowb = 0.0;
  row.uppb = 10.0;
  const auto r0 = li.add_row(row);

  SUBCASE("set_row_low_raw and get_row_low_raw round-trip")
  {
    li.set_row_low_raw(r0, 3.0);
    CHECK(li.get_row_low_raw()[r0] == doctest::Approx(3.0));
  }

  SUBCASE("set_row_upp_raw and get_row_upp_raw round-trip")
  {
    li.set_row_upp_raw(r0, 7.0);
    CHECK(li.get_row_upp_raw()[r0] == doctest::Approx(7.0));
  }

  SUBCASE("set_rhs_raw pins both bounds")
  {
    li.set_rhs_raw(r0, 5.0);
    CHECK(li.get_row_low_raw()[r0] == doctest::Approx(5.0));
    CHECK(li.get_row_upp_raw()[r0] == doctest::Approx(5.0));
  }

  SUBCASE("set_row_low (physical) divides by row_scale")
  {
    li.set_row_scale(r0, 10.0);
    li.set_row_low(r0, 30.0);  // physical = 30; raw = 30 / 10 = 3.
    CHECK(li.get_row_low_raw()[r0] == doctest::Approx(3.0));
    CHECK(li.get_row_low()[r0] == doctest::Approx(30.0));
  }

  SUBCASE("set_row_upp (physical) divides by row_scale")
  {
    li.set_row_scale(r0, 10.0);
    li.set_row_upp(r0, 70.0);
    CHECK(li.get_row_upp_raw()[r0] == doctest::Approx(7.0));
    CHECK(li.get_row_upp()[r0] == doctest::Approx(70.0));
  }

  SUBCASE("set_rhs (physical) pins both bounds in raw space")
  {
    li.set_row_scale(r0, 10.0);
    li.set_rhs(r0, 50.0);
    CHECK(li.get_row_low_raw()[r0] == doctest::Approx(5.0));
    CHECK(li.get_row_upp_raw()[r0] == doctest::Approx(5.0));
  }

  SUBCASE("normalize_bound maps DblMax to +infinity on raw row setter")
  {
    li.set_row_upp_raw(r0, LinearProblem::DblMax);
    CHECK(li.is_pos_inf(li.get_row_upp_raw()[r0]));
  }
}

// ────────────────────────────────────────────────────────────────────
// Test 5.5 — `add_col` post-flatten applies `/ scale_objective`.
//
// Pins the parity between structural columns (whose cost is divided by
// `scale_objective` at `flatten()` time, linear_problem.cpp:725-729)
// and post-flatten columns (e.g. SDDP α via
// `SDDPMethod::add_alpha_state_var`).  Without the divide, the
// post-flatten column lands in the LP with cost `scale_objective`
// times larger than every structural column, dominating the
// minimisation and pinning the column to its lower bound.
//
// Symptom on juan/gtopt_iplp at scale_obj=1000: SDDP α gets cost 1000
// in LP-space (intended: 0.001) → α pinned to its bootstrap floor →
// cuts cannot bind α from above → LB inflates to 1.5e+19 at iter 1,
// max kappa 1.55e+34.  This test pins the dimensional contract
// directly so any regression that re-introduces an unscaled add_col
// path is caught at unit-test granularity.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(
    "LinearInterface - add_col post-flatten cost / scale_objective")  // NOLINT
{
  // The model: pin two columns to 1, one structural and one
  // post-flatten.  Both have phys cost = 1 (and col_scale = 1).
  // The optimal physical objective MUST equal 2.0 — same physical
  // cost, same physical solution — regardless of `scale_objective`.
  //
  // Pre-fix: the post-flatten column's stored cost was scale_obj
  // larger than the structural one, so the LP optimum at scale_obj
  // != 1 was incorrect (or different from scale_obj=1) by exactly
  // the missing factor.
  constexpr std::array scale_objs = {1.0, 100.0, 1000.0};

  for (const double scale_obj : scale_objs) {
    CAPTURE(scale_obj);
    LinearProblem lp("addcol_scale");
    const auto c_struct = lp.add_col(SparseCol {.uppb = 100.0, .cost = 1.0});
    auto r0 = SparseRow {};
    r0[c_struct] = 1.0;
    r0.greater_equal(0.0);
    std::ignore = lp.add_row(std::move(r0));

    LinearInterface li(
        "",
        lp.flatten({.equilibration_method = LpEquilibrationMethod::none,
                    .scale_objective = scale_obj}));
    li.save_base_numrows();

    // Post-flatten column with the SAME physical cost.  The fix
    // ensures the LP-space stored cost equals `1.0 / scale_obj`,
    // matching the structural column.
    const auto c_new = li.add_col(SparseCol {.uppb = 100.0, .cost = 1.0});

    // Pin both columns to physical x=1 so we can inspect obj_phys.
    li.set_col_low(c_struct, 1.0);
    li.set_col_upp(c_struct, 1.0);
    li.set_col_low(c_new, 1.0);
    li.set_col_upp(c_new, 1.0);

    const auto status = li.initial_solve({});
    REQUIRE((status && *status == 0));
    REQUIRE(li.is_optimal());

    // The physical obj must equal 2.0 = 2 × (cost × x_phys) at any
    // scale_objective.  With the fix, both cols' physical
    // contributions are identical (1.0 each).  Pre-fix, the
    // post-flatten col's contribution would be scale_obj larger,
    // breaking the physical invariance.
    CHECK(li.get_obj_value() == doctest::Approx(2.0).epsilon(1e-9));
  }
}

// ────────────────────────────────────────────────────────────────────
// Test 6 — closes G9, G10
// Physical objective value and scale_objective() accessor.
// ────────────────────────────────────────────────────────────────────
TEST_CASE("LinearInterface - get_obj_value with scale_objective")  // NOLINT
{
  // min 2*x  s.t.  x >= 5, 0 <= x <= 20
  // With scale_objective=1000, the LP obj coefficient is stored as
  // 2 / 1000 = 0.002, so raw_obj = 0.002 × 5 = 0.01 and physical_obj
  // = raw_obj × 1000 = 10.
  LinearProblem lp("obj_scale");
  const auto c0 = lp.add_col(SparseCol {.uppb = 20.0, .cost = 2.0});

  auto r0 = SparseRow {};
  r0[c0] = 1.0;
  r0.greater_equal(5.0);
  std::ignore = lp.add_row(std::move(r0));

  auto flat = lp.flatten({.scale_objective = 1000.0});

  LinearInterface li("", flat);
  CHECK(li.scale_objective() == doctest::Approx(1000.0));

  const auto status = li.initial_solve({});
  REQUIRE((status && *status == 0));
  REQUIRE(li.is_optimal());

  SUBCASE("get_obj_value == get_obj_value_raw × scale_objective")
  {
    const double raw_obj = li.get_obj_value_raw();
    const double phys_obj = li.get_obj_value();
    CHECK(phys_obj == doctest::Approx(raw_obj * 1000.0));
    // Physical optimum: cost=2, x=5 → obj=10 (in physical $ units).
    CHECK(phys_obj == doctest::Approx(10.0).epsilon(1e-6));
  }

  SUBCASE("default scale_objective is 1.0 after ctor without load_flat")
  {
    LinearInterface li_empty;
    CHECK(li_empty.scale_objective() == doctest::Approx(1.0));
  }

  SUBCASE("get_obj_value_raw is invariant of scale_objective semantics")
  {
    // raw_obj is what the LP solver reports directly; it is the
    // physical_obj divided by scale_objective.  This is the contract
    // SDDP relies on for picking which units to feed the LB/UB
    // aggregation.
    const double raw = li.get_obj_value_raw();
    const double phys = li.get_obj_value();
    CHECK(raw == doctest::Approx(phys / li.scale_objective()));
    CHECK(raw == doctest::Approx(0.01).epsilon(1e-6));  // 0.002 × 5
  }
}

// ────────────────────────────────────────────────────────────────────
// SDDP LB/UB unit-consistency invariant
//
// SDDP aggregates the upper bound in physical units (sddp_forward_pass
// returns `obj_physical - alpha`) and reads the lower bound from the
// first-phase LP objective.  Both sides MUST be in the same units —
// otherwise a non-unit `scale_objective` leaves a permanent
// `(scale - 1) / scale` gap that prevents convergence (the juan
// IPLP_uninodal symptom: gap pinned at ~0.999 with scale_obj=1000).
//
// This test pins the unit-consistency contract:  for the same LP, the
// "LB-side" reading via get_obj_value() (physical) must equal the
// "UB-side" aggregation (also physical) once alpha is removed.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "LinearInterface - get_obj_value matches SDDP LB/UB physical units")
{
  // min 50 x  s.t.  x >= 4, 0 <= x <= 10
  // physical optimum: 50 × 4 = 200; raw = 200 / scale_obj.
  for (const double scale_obj : {1.0, 10.0, 1000.0, 1e6}) {
    LinearProblem lp("sddp_unit_invariant");
    const auto c0 = lp.add_col(SparseCol {.uppb = 10.0, .cost = 50.0});

    auto r0 = SparseRow {};
    r0[c0] = 1.0;
    r0.greater_equal(4.0);
    std::ignore = lp.add_row(std::move(r0));

    auto flat = lp.flatten({.scale_objective = scale_obj});

    LinearInterface li("", flat);
    REQUIRE(li.scale_objective() == doctest::Approx(scale_obj));

    const auto status = li.initial_solve({});
    REQUIRE((status && *status == 0));
    REQUIRE(li.is_optimal());

    CAPTURE(scale_obj);

    // Physical obj must be 200 regardless of scale_objective — this is
    // the value SDDP feeds into the UB aggregation.
    const double phys_obj = li.get_obj_value();
    CHECK(phys_obj == doctest::Approx(200.0).epsilon(1e-6));

    // Raw obj scales inversely with scale_objective.
    const double raw_obj = li.get_obj_value_raw();
    CHECK(raw_obj == doctest::Approx(200.0 / scale_obj).epsilon(1e-6));

    // The LB read site (now also physical post-fix) and the UB
    // aggregation MUST agree on the same LP up to alpha; with alpha=0
    // they match exactly.
    CHECK(phys_obj == doctest::Approx(raw_obj * scale_obj));

    // Sanity: the broken LB-raw / UB-physical pairing would produce
    // exactly the (scale - 1) / scale gap the user observed.  Pinning
    // this here means any future regression that re-introduces the
    // unit mismatch will fail the test on the very same shape.
    if (scale_obj > 1.0) {
      const double broken_gap =
          std::abs(phys_obj - raw_obj) / std::max(1.0, std::abs(phys_obj));
      const double expected_broken = 1.0 - (1.0 / scale_obj);
      CHECK(broken_gap == doctest::Approx(expected_broken).epsilon(1e-6));
    }
  }
}

// ────────────────────────────────────────────────────────────────────
// Test 7 — closes G1, G4 reinforcement + bounds-after-flatten
// End-to-end: raw bound set after flatten/load_flat survives into the
// solver and is honored by an actual solve.  This is the integration
// test for the scale-vector wiring from LinearProblem → FlatLinearProblem
// → LinearInterface.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "LinearInterface - raw bound pin survives flatten+load_flat and solve")
{
  // Build via LinearProblem with a non-unit scale, flatten, and pin a
  // raw bound that forces x to exactly physical=100 (raw=20, scale=5).
  LinearProblem lp("flatten_then_raw");
  const auto c0 = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 200.0,
      .cost = 1.0,
      .scale = 5.0,
  });
  const auto c1 = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 100.0,
      .cost = 2.0,
  });

  auto r0 = SparseRow {};
  r0[c0] = 1.0;
  r0[c1] = 1.0;
  r0.bound(0.0, 1000.0);
  std::ignore = lp.add_row(std::move(r0));

  LinearInterface li("", lp.flatten({}));

  REQUIRE(li.get_col_scale(c0) == doctest::Approx(5.0));
  REQUIRE(li.get_col_scale(c1) == doctest::Approx(1.0));

  // Pin c0 at raw=20 (physical=100) via the raw setters.
  li.set_col_low_raw(c0, 20.0);
  li.set_col_upp_raw(c0, 20.0);

  SUBCASE("raw getters return the pinned raw value")
  {
    CHECK(li.get_col_low_raw()[c0] == doctest::Approx(20.0));
    CHECK(li.get_col_upp_raw()[c0] == doctest::Approx(20.0));
  }

  SUBCASE("physical getters return raw × col_scale")
  {
    CHECK(li.get_col_low()[c0] == doctest::Approx(100.0));
    CHECK(li.get_col_upp()[c0] == doctest::Approx(100.0));
  }

  SUBCASE("c1 is unaffected by the c0 pin")
  {
    CHECK(li.get_col_low_raw()[c1] == doctest::Approx(0.0));
    CHECK(li.get_col_upp_raw()[c1] == doctest::Approx(100.0));
  }

  SUBCASE("solver honours the raw pin: optimal x0_physical = 100")
  {
    const auto status = li.initial_solve({});
    REQUIRE((status && *status == 0));
    REQUIRE(li.is_optimal());

    // get_col_sol() returns physical via ×col_scale.  Pinned at raw=20,
    // physical = 20 × 5 = 100.
    const auto sol_phys = li.get_col_sol();
    CHECK(sol_phys[c0] == doctest::Approx(100.0).epsilon(1e-6));
    // Raw solution should equal the raw bound.
    const auto sol_raw = li.get_col_sol_raw();
    CHECK(sol_raw[c0] == doctest::Approx(20.0).epsilon(1e-6));
  }
}

// ────────────────────────────────────────────────────────────────────
// Test 8 — closes G12
// LinearProblem::get_col_scale returns the stored scale, and flatten()
// populates FlatLinearProblem::col_scales with the same value.
// ────────────────────────────────────────────────────────────────────
TEST_CASE("LinearProblem - get_col_scale and flatten col_scales")  // NOLINT
{
  LinearProblem lp("scale_flat");
  const auto c0 = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 1.0,
      .scale = 3.162,
  });
  const auto c1 =
      lp.add_col(SparseCol {.lowb = 0.0, .uppb = 10.0, .cost = 2.0});

  SUBCASE("get_col_scale returns the stored SparseCol.scale")
  {
    CHECK(lp.get_col_scale(c0) == doctest::Approx(3.162));
    CHECK(lp.get_col_scale(c1) == doctest::Approx(1.0));
  }

  SUBCASE("flatten populates FlatLinearProblem::col_scales")
  {
    auto r0 = SparseRow {};
    r0[c0] = 1.0;
    r0.uppb = 10.0;
    std::ignore = lp.add_row(std::move(r0));

    const auto flat = lp.flatten({});
    REQUIRE(flat.col_scales.size() == 2);
    CHECK(flat.col_scales[c0] == doctest::Approx(3.162));
    CHECK(flat.col_scales[c1] == doctest::Approx(1.0));
  }

  SUBCASE("LinearInterface picks up col_scales from load_flat")
  {
    auto r0 = SparseRow {};
    r0[c0] = 1.0;
    r0.uppb = 10.0;
    std::ignore = lp.add_row(std::move(r0));

    LinearInterface li("", lp.flatten({}));
    CHECK(li.get_col_scale(c0) == doctest::Approx(3.162));
    CHECK(li.get_col_scale(c1) == doctest::Approx(1.0));
    CHECK(li.get_col_scales().size() == 2);
  }
}

// ────────────────────────────────────────────────────────────────────
// Extra — set_coeff_raw vs set_coeff (physical) round-trip.
// Production callers mutate coefficients through set_coeff (physical)
// on equilibrated LPs; set_coeff_raw is used by the benders-cut builder.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "LinearInterface - set_coeff_raw and set_coeff scale round-trip")
{
  // 2-var LP with col_scale=4.0 on c0 and row_max equilibration.
  // Source-of-truth (linear_problem.cpp:346-369):
  //   physical_value = LP_value × col_scale  →  LP_coeff = phys × col_scale
  //   then row_scale divides:                  raw = phys × col_scale /
  //   row_scale
  LinearProblem lp("coeff_scale");
  const auto c0 = lp.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
      .scale = 4.0,
  });
  const auto c1 = lp.add_col(SparseCol {.uppb = 10.0, .cost = 1.0});

  auto r0 = SparseRow {};
  r0[c0] = 100.0;
  r0[c1] = 100.0;
  r0.bound(0.0, 1000.0);
  const auto r0_idx = lp.add_row(std::move(r0));

  LinearInterface li("",
                     lp.flatten({
                         .equilibration_method = LpEquilibrationMethod::row_max,
                     }));

  if (!li.supports_set_coeff()) {
    return;  // Backend does not support in-place coefficient updates.
  }

  const double col_scale = li.get_col_scale(c0);
  const double row_scale = li.get_row_scale(r0_idx);
  REQUIRE(col_scale == doctest::Approx(4.0));
  // row_max picks max|raw_coeff × col_scale| per row: c0 contributes
  // 100 × 4 = 400, c1 contributes 100 × 1 = 100, row max = 400.
  REQUIRE(row_scale == doctest::Approx(400.0));

  SUBCASE("set_coeff_raw and get_coeff_raw round-trip")
  {
    li.set_coeff_raw(r0_idx, c0, 0.25);
    CHECK(li.get_coeff_raw(r0_idx, c0) == doctest::Approx(0.25));
  }

  SUBCASE("set_coeff (physical) → get_coeff_raw applies × col_scale/row_scale")
  {
    constexpr double kPhysCoeff = 8.0;
    li.set_coeff(r0_idx, c0, kPhysCoeff);
    // Internal raw = phys × col_scale / row_scale = 8 × 4 / 400 = 0.08
    const double expected_raw = kPhysCoeff * col_scale / row_scale;
    CHECK(li.get_coeff_raw(r0_idx, c0) == doctest::Approx(expected_raw));
    CHECK(li.get_coeff(r0_idx, c0) == doctest::Approx(kPhysCoeff));
  }
}

// ────────────────────────────────────────────────────────────────────
// Extra — LinearInterface infinity handling.
// normalize_bound, is_pos_inf, is_neg_inf cover the shim used by every
// raw setter.
// ────────────────────────────────────────────────────────────────────
TEST_CASE("LinearInterface - infinity normalization")  // NOLINT
{
  LinearInterface li;
  const auto inf = li.infinity();
  REQUIRE(inf > 0.0);

  SUBCASE("normalize_bound maps DblMax to +infinity")
  {
    // Direct equality — `Approx` degenerates to NaN for IEEE inf
    // (HiGHS's `infinity()`) and reports spurious inequalities.
    CHECK(li.normalize_bound(LinearProblem::DblMax) == inf);
    CHECK(li.normalize_bound(-LinearProblem::DblMax) == -inf);
  }

  SUBCASE("normalize_bound preserves finite values")
  {
    CHECK(li.normalize_bound(42.0) == doctest::Approx(42.0));
    CHECK(li.normalize_bound(-42.0) == doctest::Approx(-42.0));
    CHECK(li.normalize_bound(0.0) == doctest::Approx(0.0));
  }

  SUBCASE("normalize_bounds returns the pair-transformed result")
  {
    const auto [lo, up] =
        li.normalize_bounds(-LinearProblem::DblMax, LinearProblem::DblMax);
    // Direct equality — `doctest::Approx` uses a relative-tolerance
    // margin that degenerates to NaN for IEEE infinities (the
    // `infinity()` value HiGHS returns) and reports spurious
    // inequalities even when both sides are the same infinity.
    CHECK(lo == -inf);
    CHECK(up == inf);
  }

  SUBCASE("is_pos_inf / is_neg_inf predicates")
  {
    // Predicates return true when |value| >= infinity().  The
    // "well below infinity" probe depends on what infinity() is:
    //   CPLEX / CLP / CBC: finite sentinel (~1e20) — `inf * 0.5`
    //     is strictly less than inf, so !is_pos_inf.
    //   HiGHS: IEEE `std::numeric_limits<double>::infinity()` —
    //     `inf * 0.5` is still IEEE inf, so is_pos_inf(inf * 0.5)
    //     is true.  Use a finite probe (1.0) instead.
    CHECK(li.is_pos_inf(inf));
    CHECK(li.is_pos_inf(inf * 2.0));
    CHECK_FALSE(li.is_pos_inf(0.0));
    CHECK_FALSE(li.is_pos_inf(1.0));
    CHECK(li.is_neg_inf(-inf));
    CHECK(li.is_neg_inf(-inf * 2.0));
    CHECK_FALSE(li.is_neg_inf(0.0));
    CHECK_FALSE(li.is_neg_inf(-1.0));
  }
}

// ────────────────────────────────────────────────────────────────────
// Extra — LinearProblem normalize_bound via add_col and set_infinity.
// LinearProblem's internal normalize_bound runs inside add_col/add_row
// and clamps DblMax to the configured infinity.
// ────────────────────────────────────────────────────────────────────
TEST_CASE("LinearProblem - normalize_bound via set_infinity")  // NOLINT
{
  LinearProblem lp("inf_clamp");
  lp.set_infinity(1e9);
  CHECK(lp.infinity() == doctest::Approx(1e9));

  const auto c0 = lp.add_col(SparseCol {
      .lowb = -LinearProblem::DblMax,
      .uppb = LinearProblem::DblMax,
  });

  // Bounds are normalized in-place during add_col.
  CHECK(lp.get_col_lowb(c0) == doctest::Approx(-1e9));
  CHECK(lp.get_col_uppb(c0) == doctest::Approx(1e9));

  auto row = SparseRow {};
  row[c0] = 1.0;
  row.lowb = -LinearProblem::DblMax;
  row.uppb = LinearProblem::DblMax;
  const auto r0 = lp.add_row(std::move(row));
  CHECK(lp.row_at(r0).lowb == doctest::Approx(-1e9));
  CHECK(lp.row_at(r0).uppb == doctest::Approx(1e9));
}

// ────────────────────────────────────────────────────────────────────
// Extra — obj_coeff setter/getter round-trip via set_obj_coeff_raw.
// ────────────────────────────────────────────────────────────────────
// `get_obj_coeff()` returns the backend's raw obj vector — so the
// natural round-trip companion is `set_obj_coeff_raw`.  The
// transforming `set_obj_coeff(physical)` is exercised by the
// dedicated scale tests below.
TEST_CASE(
    "LinearInterface - set_obj_coeff_raw / get_obj_coeff round-trip")  // NOLINT
{
  LinearProblem lp("obj_coeff");
  const auto c0 = lp.add_col(SparseCol {.uppb = 10.0, .cost = 1.0});
  const auto c1 = lp.add_col(SparseCol {.uppb = 10.0, .cost = 2.0});
  auto r0 = SparseRow {};
  r0[c0] = 1.0;
  r0[c1] = 1.0;
  r0.uppb = 20.0;
  std::ignore = lp.add_row(std::move(r0));

  LinearInterface li("", lp.flatten({}));

  const auto obj_before = li.get_obj_coeff();
  REQUIRE(obj_before.size() == 2);
  CHECK(obj_before[c0] == doctest::Approx(1.0));
  CHECK(obj_before[c1] == doctest::Approx(2.0));

  li.set_obj_coeff_raw(c0, 7.0);
  const auto obj_after = li.get_obj_coeff();
  CHECK(obj_after[c0] == doctest::Approx(7.0));
  CHECK(obj_after[c1] == doctest::Approx(2.0));  // unchanged
}

// ────────────────────────────────────────────────────────────────────
// LP-SEMANTIC TESTS
//
// All tests above verify *round-trip* consistency (set then get).
// Round-trip tests are self-consistent regardless of whether the
// transform is correct: a set_coeff that applies /col_scale twice and
// a get_coeff that applies *col_scale twice would still round-trip.
//
// The tests below actually SOLVE the LP and verify the solver enforces
// the correct physical constraint.  They would have caught the bug
// where set_coeff stored physical_value / col_scale / col_scale instead
// of physical_value / col_scale / row_scale — causing the LP solver to
// enforce a constraint 100× weaker than intended (col_scale=10 case).
//
// Pattern: build LP with analytically-known optimal, mutate ONE
// coefficient via the mutator under test, solve, assert x_opt.
// ────────────────────────────────────────────────────────────────────

// ────────────────────────────────────────────────────────────────────
// LP-Semantic Test 1 — set_coeff LP-semantic correctness
//
// LP: min 1·x  s.t.  a·x = 50, 0 ≤ x ≤ 1000.
// Analytical optimum: x = 50 / a.
// We set a = 5.0 via set_coeff and expect x_opt = 10.0.
//
// The pre-fix bug: set_coeff with col_scale=10 stored 5/10/10 = 0.05
// instead of 5/10/1 = 0.5 in LP-raw units (row_max equilibration case
// has row_scale != 1 but plain eq=none has row_scale=1).  The solver
// then saw 0.05·x_raw = 50·(1/row_scale), driving x_raw ≈ 1000 →
// x_phys = 10000.  This test catches any regression where col_scale is
// applied incorrectly in the write direction.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "LinearInterface - set_coeff LP-semantic: solver enforces physical "
    "constraint")
{
  struct Params
  {
    double col_scale;
    double scale_obj;
    LpEquilibrationMethod eq;
  };

  constexpr double kRhs = 50.0;
  constexpr double kPhysCoeff = 5.0;
  constexpr double kExpectedX = kRhs / kPhysCoeff;  // 10.0

  const auto run_subcase = [&](Params p)
  {
    LinearProblem lp("set_coeff_lp_semantic");
    const auto c0 = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1000.0,
        .cost = 1.0,
        .scale = p.col_scale,
    });

    // Placeholder coefficient = 1 so the row is non-degenerate at build
    // time; we will overwrite it via set_coeff before solving.
    SparseRow row;
    row[c0] = 1.0;
    row.lowb = kRhs;
    row.uppb = kRhs;
    const auto r0 = lp.add_row(std::move(row));

    LinearInterface li("",
                       lp.flatten({
                           .equilibration_method = p.eq,
                           .scale_objective = p.scale_obj,
                       }));

    if (!li.supports_set_coeff()) {
      return;
    }

    // Update the coefficient to the physical value under test.
    li.set_coeff(r0, c0, kPhysCoeff);

    const auto status = li.initial_solve({});
    REQUIRE((status && *status == 0));
    REQUIRE(li.is_optimal());

    const auto sol_phys = li.get_col_sol();
    CAPTURE(p.col_scale);
    CAPTURE(p.scale_obj);
    CHECK(sol_phys[c0] == doctest::Approx(kExpectedX).epsilon(1e-3));
    CHECK(li.get_obj_value() == doctest::Approx(kExpectedX).epsilon(1e-3));
  };

  SUBCASE("trivial: col_scale=1, scale_obj=1, eq=none")
  {
    run_subcase({.col_scale = 1.0,
                 .scale_obj = 1.0,
                 .eq = LpEquilibrationMethod::none});
  }

  SUBCASE("var_scale: col_scale=10, scale_obj=1, eq=none")
  {
    // The bug-catching subcase: col_scale=10 triggers the double-divide
    // path in pre-fix code.  x_opt must still be 10, not 1000.
    run_subcase({.col_scale = 10.0,
                 .scale_obj = 1.0,
                 .eq = LpEquilibrationMethod::none});
  }

  SUBCASE("scale_obj: col_scale=1, scale_obj=1000, eq=none")
  {
    run_subcase({.col_scale = 1.0,
                 .scale_obj = 1000.0,
                 .eq = LpEquilibrationMethod::none});
  }

  SUBCASE("all: col_scale=10, scale_obj=1000, eq=row_max")
  {
    // Full juan-like scaling.  row_max equilibration divides the row by
    // max|raw_col_elem|; the physical x_opt must still be 10.
    run_subcase({.col_scale = 10.0,
                 .scale_obj = 1000.0,
                 .eq = LpEquilibrationMethod::row_max});
  }
}

// ────────────────────────────────────────────────────────────────────
// LP-Semantic Test 2 — set_rhs LP-semantic correctness
//
// LP: min 1·x  s.t.  2·x = b, 0 ≤ x ≤ 500.
// Analytical optimum: x = b / 2.
// We set b = 100 via set_rhs and expect x_opt = 50.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "LinearInterface - set_rhs LP-semantic: solver enforces physical RHS")
{
  struct Params
  {
    double col_scale;
    double scale_obj;
    LpEquilibrationMethod eq;
  };

  constexpr double kCoeff = 2.0;
  constexpr double kPhysRhs = 100.0;
  constexpr double kExpectedX = kPhysRhs / kCoeff;  // 50.0

  const auto run_subcase = [&](Params p)
  {
    LinearProblem lp("set_rhs_lp_semantic");
    const auto c0 = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 500.0,
        .cost = 1.0,
        .scale = p.col_scale,
    });

    SparseRow row;
    row[c0] = kCoeff;
    // Placeholder RHS: will be overwritten by set_rhs.
    row.lowb = 1.0;
    row.uppb = 1.0;
    const auto r0 = lp.add_row(std::move(row));

    LinearInterface li("",
                       lp.flatten({
                           .equilibration_method = p.eq,
                           .scale_objective = p.scale_obj,
                       }));

    // Update both bounds to the physical RHS.
    li.set_rhs(r0, kPhysRhs);

    const auto status = li.initial_solve({});
    REQUIRE((status && *status == 0));
    REQUIRE(li.is_optimal());

    const auto sol_phys = li.get_col_sol();
    CAPTURE(p.col_scale);
    CAPTURE(p.scale_obj);
    CHECK(sol_phys[c0] == doctest::Approx(kExpectedX).epsilon(1e-3));
  };

  SUBCASE("trivial: col_scale=1, scale_obj=1, eq=none")
  {
    run_subcase({.col_scale = 1.0,
                 .scale_obj = 1.0,
                 .eq = LpEquilibrationMethod::none});
  }

  SUBCASE("var_scale: col_scale=10, scale_obj=1, eq=none")
  {
    run_subcase({.col_scale = 10.0,
                 .scale_obj = 1.0,
                 .eq = LpEquilibrationMethod::none});
  }

  SUBCASE("scale_obj: col_scale=1, scale_obj=1000, eq=none")
  {
    run_subcase({.col_scale = 1.0,
                 .scale_obj = 1000.0,
                 .eq = LpEquilibrationMethod::none});
  }

  SUBCASE("all: col_scale=10, scale_obj=1000, eq=row_max")
  {
    run_subcase({.col_scale = 10.0,
                 .scale_obj = 1000.0,
                 .eq = LpEquilibrationMethod::row_max});
  }
}

// ────────────────────────────────────────────────────────────────────
// LP-Semantic Test 3 — set_obj_coeff LP-semantic correctness
//
// LP: min c·x  s.t.  5 ≤ x ≤ 100.
// For c > 0: optimal x = 5 (lower bound), obj = 5·c (physical).
// We set c via set_obj_coeff and verify the physical objective.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "LinearInterface - set_obj_coeff LP-semantic: solver minimizes with "
    "correct physical cost")
{
  struct Params
  {
    double col_scale;
    double scale_obj;
  };

  constexpr double kPhysCost = 3.0;
  constexpr double kLb = 5.0;
  constexpr double kExpectedObj = kPhysCost * kLb;  // 15.0

  const auto run_subcase = [&](Params p)
  {
    LinearProblem lp("set_obj_coeff_lp_semantic");
    const auto c0 = lp.add_col(SparseCol {
        .lowb = kLb,
        .uppb = 100.0,
        .cost = 0.0,  // zero placeholder; will be set via set_obj_coeff
        .scale = p.col_scale,
    });
    // Need at least one row for a valid LP.
    SparseRow row;
    row[c0] = 1.0;
    row.lowb = 0.0;
    row.uppb = 1000.0;
    std::ignore = lp.add_row(std::move(row));

    LinearInterface li("",
                       lp.flatten({
                           .equilibration_method = LpEquilibrationMethod::none,
                           .scale_objective = p.scale_obj,
                       }));

    // Set the physical cost.
    li.set_obj_coeff(c0, kPhysCost);

    const auto status = li.initial_solve({});
    REQUIRE((status && *status == 0));
    REQUIRE(li.is_optimal());

    CAPTURE(p.col_scale);
    CAPTURE(p.scale_obj);
    // x is pinned to lower bound kLb by the objective (c > 0).
    const auto sol_phys = li.get_col_sol();
    CHECK(sol_phys[c0] == doctest::Approx(kLb).epsilon(1e-3));
    // Physical objective = kPhysCost × kLb regardless of scaling.
    CHECK(li.get_obj_value() == doctest::Approx(kExpectedObj).epsilon(1e-3));
  };

  SUBCASE("trivial: col_scale=1, scale_obj=1")
  {
    run_subcase({.col_scale = 1.0, .scale_obj = 1.0});
  }

  SUBCASE("var_scale: col_scale=10, scale_obj=1")
  {
    run_subcase({.col_scale = 10.0, .scale_obj = 1.0});
  }

  SUBCASE("scale_obj: col_scale=1, scale_obj=1000")
  {
    run_subcase({.col_scale = 1.0, .scale_obj = 1000.0});
  }

  SUBCASE("all: col_scale=10, scale_obj=1000")
  {
    run_subcase({.col_scale = 10.0, .scale_obj = 1000.0});
  }
}

// ────────────────────────────────────────────────────────────────────
// LP-Semantic Test 4 — add_row post-flatten LP-semantic correctness
//
// After flatten + save_base_numrows (the cut-phase gate), add a row with
// physical-space coefficients via add_row.  The compose_physical path
// must translate physical → LP-raw correctly so the solver enforces the
// intended constraint.
//
// LP structure:
//   Structural: min 1·x  s.t.  x ≤ 1000, x ≥ 0  (unconstrained above 0)
//   Cut row:    a·x = 50 (physical, inserted post-flatten)
//   Analytical: x = 50/a = 10 (a=5.0)
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "LinearInterface - add_row post-flatten LP-semantic: compose_physical "
    "applied correctly")
{
  struct Params
  {
    double col_scale;
    double scale_obj;
    LpEquilibrationMethod eq;
  };

  constexpr double kPhysCoeff = 5.0;
  constexpr double kPhysRhs = 50.0;
  constexpr double kExpectedX = kPhysRhs / kPhysCoeff;  // 10.0

  const auto run_subcase = [&](Params p)
  {
    LinearProblem lp("add_row_postflatten");
    const auto c0 = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1000.0,
        .cost = 1.0,
        .scale = p.col_scale,
    });
    SparseRow structural;
    structural[c0] = 1.0;
    structural.lowb = 0.0;
    structural.uppb = 1000.0;
    std::ignore = lp.add_row(std::move(structural));

    LinearInterface li("",
                       lp.flatten({
                           .equilibration_method = p.eq,
                           .scale_objective = p.scale_obj,
                       }));

    // Activate the cut phase so compose_physical triggers.
    li.save_base_numrows();

    // Add a physical-space equality constraint: kPhysCoeff·x = kPhysRhs.
    SparseRow cut_row;
    cut_row[c0] = kPhysCoeff;
    cut_row.lowb = kPhysRhs;
    cut_row.uppb = kPhysRhs;
    std::ignore = li.add_row(cut_row);

    const auto status = li.initial_solve({});
    REQUIRE((status && *status == 0));
    REQUIRE(li.is_optimal());

    const auto sol_phys = li.get_col_sol();
    CAPTURE(p.col_scale);
    CAPTURE(p.scale_obj);
    CHECK(sol_phys[c0] == doctest::Approx(kExpectedX).epsilon(1e-3));
  };

  SUBCASE("trivial: col_scale=1, scale_obj=1, eq=none")
  {
    run_subcase({.col_scale = 1.0,
                 .scale_obj = 1.0,
                 .eq = LpEquilibrationMethod::none});
  }

  SUBCASE("var_scale: col_scale=10, scale_obj=1, eq=none")
  {
    run_subcase({.col_scale = 10.0,
                 .scale_obj = 1.0,
                 .eq = LpEquilibrationMethod::none});
  }

  SUBCASE("scale_obj: col_scale=1, scale_obj=1000, eq=none")
  {
    run_subcase({.col_scale = 1.0,
                 .scale_obj = 1000.0,
                 .eq = LpEquilibrationMethod::none});
  }

  SUBCASE("all: col_scale=10, scale_obj=1000, eq=row_max")
  {
    run_subcase({.col_scale = 10.0,
                 .scale_obj = 1000.0,
                 .eq = LpEquilibrationMethod::row_max});
  }
}

// ────────────────────────────────────────────────────────────────────
// LP-Semantic Test 5 — add_rows (bulk) == add_row (per-row) semantics
//
// Verify that add_rows dispatches through compose_physical identically
// to a loop of add_row calls.  This catches the pre-2026-05-01 bug
// where add_rows was the bulk equivalent of add_row_raw (no
// compose_physical), so a col_scale=10 constraint was 10× too weak when
// inserted via add_rows but correct via add_row.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "LinearInterface - add_rows bulk == add_row loop under col_scale != 1")
{
  constexpr double kPhysCoeff = 5.0;
  constexpr double kPhysRhs = 50.0;
  constexpr double kExpectedX = kPhysRhs / kPhysCoeff;  // 10.0
  constexpr double kColScale = 10.0;
  constexpr double kScaleObj = 1000.0;

  const auto make_base_li = [&](ColIndex& c0_out) -> LinearInterface
  {
    LinearProblem lp("add_rows_bulk");
    const auto c0 = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1000.0,
        .cost = 1.0,
        .scale = kColScale,
    });
    c0_out = c0;
    SparseRow structural;
    structural[c0] = 1.0;
    structural.lowb = 0.0;
    structural.uppb = 1000.0;
    std::ignore = lp.add_row(std::move(structural));

    LinearInterface li("",
                       lp.flatten({
                           .equilibration_method = LpEquilibrationMethod::none,
                           .scale_objective = kScaleObj,
                       }));
    li.save_base_numrows();
    return li;
  };

  // Build a cut row with physical coefficients.
  ColIndex c0 {0};
  // Use c0 index 0 (the only column); make_base_li updates c0_out.
  SparseRow cut_row;
  cut_row[ColIndex {0}] = kPhysCoeff;
  cut_row.lowb = kPhysRhs;
  cut_row.uppb = kPhysRhs;
  const std::vector<SparseRow> cut_rows = {cut_row};

  SUBCASE("add_rows bulk: x_opt == expected (compose_physical applied)")
  {
    ColIndex c0_bulk {0};
    auto li = make_base_li(c0_bulk);
    li.add_rows(cut_rows);

    const auto status = li.initial_solve({});
    REQUIRE((status && *status == 0));
    REQUIRE(li.is_optimal());

    const auto sol_phys = li.get_col_sol();
    CHECK(sol_phys[c0_bulk] == doctest::Approx(kExpectedX).epsilon(1e-3));
  }

  SUBCASE("add_row per-row: x_opt == expected (reference path)")
  {
    ColIndex c0_single {0};
    auto li = make_base_li(c0_single);
    li.add_row(cut_row);

    const auto status = li.initial_solve({});
    REQUIRE((status && *status == 0));
    REQUIRE(li.is_optimal());

    const auto sol_phys = li.get_col_sol();
    CHECK(sol_phys[c0_single] == doctest::Approx(kExpectedX).epsilon(1e-3));
  }
}

// ────────────────────────────────────────────────────────────────────
// LP-Semantic Test 6 — set_col_low / set_col_upp LP-semantic
//
// LP: min 1·x  (no equality constraints — x goes to lower bound).
// Use set_col_low(physical) to update the bound and verify x_opt.
// The existing scale test (Test 7 in original file) tests raw bounds;
// this tests the physical-space setters across the subcase matrix.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "LinearInterface - set_col_low/upp LP-semantic: solver respects physical "
    "bounds")
{
  struct Params
  {
    double col_scale;
    double scale_obj;
  };

  constexpr double kPhysLow = 37.5;
  constexpr double kPhysUpp = 200.0;

  const auto run_subcase = [&](Params p)
  {
    LinearProblem lp("setcol_phys_bounds");
    const auto c0 = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1000.0,
        .cost = 1.0,
        .scale = p.col_scale,
    });
    SparseRow row;
    row[c0] = 1.0;
    row.lowb = 0.0;
    row.uppb = 1000.0;
    std::ignore = lp.add_row(std::move(row));

    LinearInterface li("",
                       lp.flatten({
                           .equilibration_method = LpEquilibrationMethod::none,
                           .scale_objective = p.scale_obj,
                       }));

    // Pin to physical [kPhysLow, kPhysUpp] via the physical setters.
    li.set_col_low(c0, kPhysLow);
    li.set_col_upp(c0, kPhysUpp);

    const auto status = li.initial_solve({});
    REQUIRE((status && *status == 0));
    REQUIRE(li.is_optimal());

    CAPTURE(p.col_scale);
    CAPTURE(p.scale_obj);
    // Minimizing: x pins to lower bound.
    const auto sol_phys = li.get_col_sol();
    CHECK(sol_phys[c0] == doctest::Approx(kPhysLow).epsilon(1e-3));
    CHECK(li.get_obj_value() == doctest::Approx(kPhysLow).epsilon(1e-3));
  };

  SUBCASE("trivial: col_scale=1, scale_obj=1")
  {
    run_subcase({.col_scale = 1.0, .scale_obj = 1.0});
  }

  SUBCASE("var_scale: col_scale=10, scale_obj=1")
  {
    run_subcase({.col_scale = 10.0, .scale_obj = 1.0});
  }

  SUBCASE("scale_obj: col_scale=1, scale_obj=1000")
  {
    run_subcase({.col_scale = 1.0, .scale_obj = 1000.0});
  }

  SUBCASE("all: col_scale=10, scale_obj=1000")
  {
    run_subcase({.col_scale = 10.0, .scale_obj = 1000.0});
  }
}

// ────────────────────────────────────────────────────────────────────
// LP-Semantic Test 7 — Regression guard: add_row vs set_coeff
//
// This is the definitive test that would have caught the set_coeff bug.
//
// Build two LP instances with col_scale=10:
//   Path A: add_row with physical coeff=5 directly.
//   Path B: add_row with placeholder coeff=1, then set_coeff(phys=5).
//
// Both must solve to x_opt=10 (physical).
// Pre-fix: Path A → x=10, Path B → x=1000 (col_scale applied twice).
//
// This test FAILS on pre-fix code and PASSES on fixed code.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "LinearInterface - add_row + set_coeff equivalence under col_scale != 1 "
    "[regression guard]")
{
  constexpr double kColScale = 10.0;
  constexpr double kPhysCoeff = 5.0;
  constexpr double kPhysRhs = 50.0;
  constexpr double kExpectedX = kPhysRhs / kPhysCoeff;  // 10.0

  for (const double scale_obj : {1.0, 1000.0}) {
    CAPTURE(scale_obj);

    const auto make_li = [&](ColIndex& c0_out) -> LinearInterface
    {
      LinearProblem lp("regression_guard");
      const auto c0 = lp.add_col(SparseCol {
          .lowb = 0.0,
          .uppb = 10000.0,
          .cost = 1.0,
          .scale = kColScale,
      });
      c0_out = c0;
      // Structural row: unconstrained above 0.
      SparseRow structural;
      structural[c0] = 1.0;
      structural.lowb = 0.0;
      structural.uppb = 10000.0;
      std::ignore = lp.add_row(std::move(structural));

      LinearInterface li(
          "",
          lp.flatten({
              .equilibration_method = LpEquilibrationMethod::none,
              .scale_objective = scale_obj,
          }));
      li.save_base_numrows();
      return li;
    };

    // Path A: insert physical constraint directly via add_row.
    double x_opt_a = 0.0;
    {
      ColIndex c0 {0};
      auto li = make_li(c0);
      SparseRow row;
      row[c0] = kPhysCoeff;
      row.lowb = kPhysRhs;
      row.uppb = kPhysRhs;
      std::ignore = li.add_row(row);

      const auto st = li.initial_solve({});
      REQUIRE((st && *st == 0));
      REQUIRE(li.is_optimal());
      x_opt_a = li.get_col_sol()[c0];
    }

    // Path B: placeholder coefficient, then update via set_coeff.
    double x_opt_b = 0.0;
    bool skip = false;
    {
      ColIndex c0 {0};
      auto li = make_li(c0);
      if (!li.supports_set_coeff()) {
        skip = true;
      } else {
        // Insert with placeholder coefficient 1.0.
        SparseRow row;
        row[c0] = 1.0;
        row.lowb = kPhysRhs;
        row.uppb = kPhysRhs;
        const auto r0 = li.add_row(row);

        // Overwrite with the intended physical coefficient.
        li.set_coeff(r0, c0, kPhysCoeff);

        const auto st = li.initial_solve({});
        REQUIRE((st && *st == 0));
        REQUIRE(li.is_optimal());
        x_opt_b = li.get_col_sol()[c0];
      }
    }

    // Both paths must produce the same optimal x.
    // Pre-fix: x_opt_a=10, x_opt_b=1000  → test fails.
    // Post-fix: x_opt_a=10, x_opt_b=10   → test passes.
    CHECK(x_opt_a == doctest::Approx(kExpectedX).epsilon(1e-3));
    if (!skip) {
      CHECK(x_opt_b == doctest::Approx(kExpectedX).epsilon(1e-3));
      CHECK(x_opt_a == doctest::Approx(x_opt_b).epsilon(1e-3));
    }
  }
}

// ────────────────────────────────────────────────────────────────────
// LP-Semantic Test 8 — get_coeff LP-semantic round-trip via solve
//
// After inserting a physical-space row via add_row (compose_physical
// path), get_coeff must return the ORIGINAL physical value — regardless
// of col_scale, row_scale, or scale_objective.
//
// This complements the existing round-trip test which only exercises
// set_coeff ↔ get_coeff.  Here we verify the compose_physical path
// of add_row followed by get_coeff.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "LinearInterface - get_coeff after add_row returns original physical value")
{
  struct Params
  {
    double col_scale;
    double scale_obj;
    LpEquilibrationMethod eq;
  };

  constexpr double kPhysCoeff = 7.0;
  constexpr double kPhysRhs = 70.0;

  const auto run_subcase = [&](Params p)
  {
    LinearProblem lp("get_coeff_semantic");
    const auto c0 = lp.add_col(SparseCol {
        .lowb = 0.0,
        .uppb = 1000.0,
        .cost = 1.0,
        .scale = p.col_scale,
    });
    SparseRow structural;
    structural[c0] = 1.0;
    structural.lowb = 0.0;
    structural.uppb = 1000.0;
    std::ignore = lp.add_row(std::move(structural));

    LinearInterface li("",
                       lp.flatten({
                           .equilibration_method = p.eq,
                           .scale_objective = p.scale_obj,
                       }));
    li.save_base_numrows();

    // Insert physical-space row.
    SparseRow cut;
    cut[c0] = kPhysCoeff;
    cut.lowb = kPhysRhs;
    cut.uppb = kPhysRhs;
    const auto r_cut = li.add_row(cut);

    // get_coeff must return the physical value regardless of scaling.
    CAPTURE(p.col_scale);
    CAPTURE(p.scale_obj);
    CHECK(li.get_coeff(r_cut, c0) == doctest::Approx(kPhysCoeff).epsilon(1e-3));

    // Also verify the constraint is enforced by the solver.
    const auto status = li.initial_solve({});
    REQUIRE((status && *status == 0));
    REQUIRE(li.is_optimal());
    const double expected_x = kPhysRhs / kPhysCoeff;  // 10.0
    CHECK(li.get_col_sol()[c0] == doctest::Approx(expected_x).epsilon(1e-3));
  };

  SUBCASE("trivial: col_scale=1, scale_obj=1, eq=none")
  {
    run_subcase({.col_scale = 1.0,
                 .scale_obj = 1.0,
                 .eq = LpEquilibrationMethod::none});
  }

  SUBCASE("var_scale: col_scale=10, scale_obj=1, eq=none")
  {
    run_subcase({.col_scale = 10.0,
                 .scale_obj = 1.0,
                 .eq = LpEquilibrationMethod::none});
  }

  SUBCASE("scale_obj: col_scale=1, scale_obj=1000, eq=none")
  {
    run_subcase({.col_scale = 1.0,
                 .scale_obj = 1000.0,
                 .eq = LpEquilibrationMethod::none});
  }

  SUBCASE("all: col_scale=10, scale_obj=1000, eq=row_max")
  {
    run_subcase({.col_scale = 10.0,
                 .scale_obj = 1000.0,
                 .eq = LpEquilibrationMethod::row_max});
  }
}
