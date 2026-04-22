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
// Test 6 — closes G9, G10
// Physical objective value and scale_objective() accessor.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(
    "LinearInterface - get_obj_value_physical with scale_objective")  // NOLINT
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

  SUBCASE("get_obj_value_physical == get_obj_value × scale_objective")
  {
    const double raw_obj = li.get_obj_value();
    const double phys_obj = li.get_obj_value_physical();
    CHECK(phys_obj == doctest::Approx(raw_obj * 1000.0));
    // Physical optimum: cost=2, x=5 → obj=10 (in physical $ units).
    CHECK(phys_obj == doctest::Approx(10.0).epsilon(1e-6));
  }

  SUBCASE("default scale_objective is 1.0 after ctor without load_flat")
  {
    LinearInterface li_empty;
    CHECK(li_empty.scale_objective() == doctest::Approx(1.0));
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
  // physical_coeff = raw_coeff × col_scale × row_scale.
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

  SUBCASE("set_coeff (physical) → get_coeff_raw applies /col_scale/row_scale")
  {
    constexpr double kPhysCoeff = 8.0;
    li.set_coeff(r0_idx, c0, kPhysCoeff);
    // Internal raw = phys / col_scale / row_scale = 8 / 4 / 100 = 0.02
    const double expected_raw = kPhysCoeff / col_scale / row_scale;
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
    CHECK(li.normalize_bound(LinearProblem::DblMax) == doctest::Approx(inf));
    CHECK(li.normalize_bound(-LinearProblem::DblMax) == doctest::Approx(-inf));
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
    CHECK(lo == doctest::Approx(-inf));
    CHECK(up == doctest::Approx(inf));
  }

  SUBCASE("is_pos_inf / is_neg_inf predicates")
  {
    // Predicates return true when |value| >= infinity().  Use
    // inf/2 as the "well below infinity" probe — inf itself varies
    // per solver (CLP ≈ 1e20, HiGHS ≈ 1e30, CPLEX ≈ 1e20).
    CHECK(li.is_pos_inf(inf));
    CHECK(li.is_pos_inf(inf * 2.0));
    CHECK_FALSE(li.is_pos_inf(0.0));
    CHECK_FALSE(li.is_pos_inf(inf * 0.5));
    CHECK(li.is_neg_inf(-inf));
    CHECK(li.is_neg_inf(-inf * 2.0));
    CHECK_FALSE(li.is_neg_inf(0.0));
    CHECK_FALSE(li.is_neg_inf(-inf * 0.5));
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
// Extra — obj_coeff setter/getter round-trip via set_obj_coeff.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(
    "LinearInterface - set_obj_coeff / get_obj_coeff round-trip")  // NOLINT
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

  li.set_obj_coeff(c0, 7.0);
  const auto obj_after = li.get_obj_coeff();
  CHECK(obj_after[c0] == doctest::Approx(7.0));
  CHECK(obj_after[c1] == doctest::Approx(2.0));  // unchanged
}
