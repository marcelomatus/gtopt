// SPDX-License-Identifier: BSD-3-Clause
//
// Coverage test for the `add_row` cut-scaling fix in
// `LinearInterface::add_row` compose_physical path.
//
// SDDP optimality / feasibility cut rows are physical-space inputs and
// must be re-scaled into LP-space using the SAME divisor that
// `flatten()` already applied to the OBJ row (`scale_objective`).
// Pre-fix, only `col_scale` was applied; cuts ended up `scale_obj`
// times too large vs the OBJ row, and SDDP's iter-N backward pass
// compounded this factor across phases (juan/gtopt_iplp: LMAULE
// coefficient 4.2e+5 → 4.2e+8 → 4.2e+11 with `scale_objective=1000`).
//
// The fix divides cut-row elements + bounds by `m_scale_objective_`
// in the compose_physical branch.  This file pins the invariants the
// fix establishes — without referencing the numeric value of
// `scale_objective` in any assertion (the live value is read back via
// `LinearInterface::scale_objective()`).

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_matrix_options.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

// Build an LP that goes through the `compose_physical` add_row branch:
// `save_base_numrows()` flips `m_base_numrows_set_` (the production
// trigger after `flush_structural_build`), and a non-1.0 col_scale on
// at least one column ensures `have_col_scales` stays true after
// flatten.  The structural `cost` × col_scale stays harmless for the
// purposes of this test (we only inspect cut-row storage).
LinearInterface make_cut_phase_lp(double scale_obj, double col1_scale = 2.0)
{
  LinearProblem lp;
  const auto c0 = lp.add_col({
      .lowb = 0.0,
      .uppb = 100.0,
      .cost = 1.0,
  });
  const auto c1 = lp.add_col({
      .lowb = 0.0,
      .uppb = 100.0,
      .cost = 0.0,
      .scale = col1_scale,
  });
  SparseRow base_row;
  base_row[c0] = 1.0;
  base_row[c1] = 1.0;
  base_row.lowb = 0.0;
  base_row.uppb = 200.0;
  std::ignore = lp.add_row(std::move(base_row));

  LinearInterface li;
  LpMatrixOptions opts;
  opts.scale_objective = scale_obj;
  // Disable per-row equilibration so the cut-scaling fix is observable
  // on the raw stored row bounds.  With equilibration on, the per-row
  // max-abs pass would multiply elements + bounds back up by the
  // largest coefficient (and store the divisor as a row_scale), making
  // `get_row_low_raw()` return the unequilibrated value — equivalent
  // by construction to the pre-fix value.  In production the juan
  // case uses `--no-scale` which disables equilibration globally; this
  // test reproduces that path so the assertions can pin the scale_obj
  // divisor without having to subtract row_scale composition.
  opts.equilibration_method = LpEquilibrationMethod::none;
  li.load_flat(lp.flatten(opts));
  li.save_base_numrows();
  return li;
}

}  // namespace

TEST_CASE(
    "LinearInterface add_row cut scaling (compose_physical path)")  // NOLINT
{
  SUBCASE(
      "compounding-bug invariant: identical physical cuts → identical LP-space "
      "rows")
  {
    // Headline check.  The pre-fix bug signature was each successive
    // cut's stored RHS being `scale_obj` times larger than the
    // previous, even though the input was identical — observed as
    // 1000× per-phase coefficient growth in SDDP backward pass on
    // juan/gtopt_iplp.  Three identical SparseRows must round-trip to
    // three identical raw row-bound values, regardless of the actual
    // scale factor.
    auto li = make_cut_phase_lp(1000.0);

    SparseRow cut;
    cut[ColIndex {0}] = 1.0;
    cut[ColIndex {1}] = 1.0e+5;
    cut.lowb = 1.0e+8;
    cut.uppb = LinearProblem::DblMax;

    const auto row_a = li.add_row(cut);
    const auto row_b = li.add_row(cut);
    const auto row_c = li.add_row(cut);

    const auto row_lows = li.get_row_low_raw();
    CHECK(row_lows[row_a] == doctest::Approx(row_lows[row_b]));
    CHECK(row_lows[row_b] == doctest::Approx(row_lows[row_c]));
  }

  SUBCASE("scale_objective=1: cut row stored verbatim (no /scale_obj)")
  {
    // With `scale_objective = 1`, the inv_so branch is a no-op and
    // the stored RHS equals the physical input.  Pins the upper edge
    // of the fix: it must not introduce a divisor when scale_obj=1.
    auto li = make_cut_phase_lp(1.0);
    REQUIRE(li.scale_objective() == doctest::Approx(1.0));

    SparseRow cut;
    cut[ColIndex {0}] = 1.0;
    cut.lowb = 1.8e+8;
    cut.uppb = LinearProblem::DblMax;
    const auto row_idx = li.add_row(cut);

    CHECK(li.get_row_low_raw()[row_idx] == doctest::Approx(1.8e+8));
  }

  SUBCASE("scale_objective>1: raw stored RHS = phys_rhs / scale_objective()")
  {
    // The raw value is what the solver sees in LP-space — divided by
    // scale_objective so cut and OBJ rows live in matching LP-space
    // units (flatten() applies the same divisor to the OBJ row).
    auto li = make_cut_phase_lp(1000.0);
    const double scale_obj = li.scale_objective();
    REQUIRE(scale_obj > 1.0);  // otherwise the fix is a no-op

    SparseRow cut;
    cut[ColIndex {0}] = 1.0;
    cut.lowb = 1.8e+8;
    cut.uppb = LinearProblem::DblMax;
    const auto row_idx = li.add_row(cut);

    CHECK(li.get_row_low_raw()[row_idx]
          == doctest::Approx(cut.lowb / scale_obj));
  }

  SUBCASE("physical readback round-trip: get_row_low()[cut] == phys_lb")
  {
    // The headline invariant of the linear_interface scaling fix.
    // Independent of scale_objective: a physical cut added via
    // add_row(SparseRow_physical) must round-trip through
    // get_row_low() (the ScaledView accessor that reapplies row_scale)
    // and return the original physical RHS verbatim.  The fix folds
    // scale_objective into composite_scale so this invariant holds.
    //
    // Pre-fix (in-place divide without composite_scale), the raw row
    // stored phys_lb / scale_obj but composite_scale = 1, so
    // get_row_low returned phys_lb / scale_obj — silently breaking
    // the contract for any caller that reads cut RHS as physical.
    constexpr double phys_rhs = 1.8e+8;
    auto li = make_cut_phase_lp(1000.0);

    SparseRow cut;
    cut[ColIndex {0}] = 1.0;
    cut[ColIndex {1}] = 4.2e+5;
    cut.lowb = phys_rhs;
    cut.uppb = LinearProblem::DblMax;
    const auto row_idx = li.add_row(cut);

    // The ScaledView reapplies row_scale (= composite_scale stored at
    // add_row time).  Output equals input physical RHS regardless of
    // scale_objective.
    CHECK(li.get_row_low()[row_idx] == doctest::Approx(phys_rhs));
  }

  SUBCASE("physical readback invariant under scale_objective sweep")
  {
    // Three identical physical cuts at three different scale_objective
    // settings must all read back the same physical RHS through
    // get_row_low().  Pins the linear_interface scaling contract
    // independently of any SDDP / aperture interaction — the
    // accessor identity holds in isolation.
    constexpr double phys_rhs = 1.0e+8;
    constexpr std::array scale_objs = {1.0, 100.0, 1000.0};

    for (const double scale_obj : scale_objs) {
      CAPTURE(scale_obj);
      auto li = make_cut_phase_lp(scale_obj);

      SparseRow cut;
      cut[ColIndex {0}] = 1.0;
      cut[ColIndex {1}] = 1.0e+5;
      cut.lowb = phys_rhs;
      cut.uppb = LinearProblem::DblMax;
      const auto row_idx = li.add_row(cut);

      // Physical readback identity, modulo float tolerance.
      CHECK(li.get_row_low()[row_idx] == doctest::Approx(phys_rhs));
    }
  }

  SUBCASE("infinite upper bound preserved (sentinel not divided)")
  {
    // Solver `infinity()` is finite (~1e+20) so a naive divide-by-
    // scale_obj on the upper bound would push it under the
    // infinity threshold and turn an open inequality into a finite
    // one.  The fix must short-circuit infinite bounds.
    auto li = make_cut_phase_lp(1000.0);

    SparseRow cut;
    cut[ColIndex {0}] = 1.0;
    cut.lowb = 5.0e+5;
    cut.uppb = LinearProblem::DblMax;
    const auto row_idx = li.add_row(cut);

    CHECK(li.get_row_upp_raw()[row_idx] >= 1.0e+19);
  }

  SUBCASE("infinite lower bound preserved (negative sentinel not divided)")
  {
    // Mirror of the upper-bound case: a `≤`-style cut has `lowb = -∞`.
    // The guard `lb > -infy && lb < infy` must skip the divide and
    // preserve the negative infinity sentinel verbatim.
    auto li = make_cut_phase_lp(1000.0);

    SparseRow cut;
    cut[ColIndex {0}] = 1.0;
    cut.lowb = -LinearProblem::DblMax;
    cut.uppb = 5.0e+5;
    const auto row_idx = li.add_row(cut);

    CHECK(li.get_row_low_raw()[row_idx] <= -1.0e+19);
  }

  SUBCASE("SparseRow::scale composes with /scale_objective")
  {
    // A user-supplied `row.scale != 1` must compose multiplicatively
    // with the new /scale_objective divisor.  Final raw lb =
    //   phys_lb / scale_obj / row.scale
    // (Step 1b runs BEFORE step 2 in compose_physical.)
    auto li = make_cut_phase_lp(1000.0);

    SparseRow cut;
    cut[ColIndex {0}] = 1.0;
    cut.lowb = 1.0e+8;
    cut.uppb = LinearProblem::DblMax;
    cut.scale = 4.0;  // user row divisor
    const auto row_idx = li.add_row(cut);

    const double scale_obj = li.scale_objective();
    CHECK(li.get_row_low_raw()[row_idx]
          == doctest::Approx(cut.lowb / scale_obj / 4.0));
  }
}

// ── Rec T1 (G-cut-1) ────────────────────────────────────────────────────────
//
// The cut-row dual extracted via `get_row_dual()` must equal the physical
// dual of the cut constraint, even when row-max equilibration was active
// at insertion time.  This is the path SDDP's feasibility-cut builder
// reads (`benders_cut.cpp:229` → `get_row_dual()`); without this pin,
// equilibration could silently break the feasibility-cut Farkas-ray
// arithmetic.
TEST_CASE(
    "LinearInterface cut row_max equilibration: dual round-trip")  // NOLINT
{
  constexpr double kScaleObj = 1000.0;

  // Build with row_max equilibration ON so step 3 of compose_physical
  // fires and records `row_scale = composite_scale` on the cut row.
  LinearProblem lp;
  const auto c0 = lp.add_col({
      .lowb = 0.0,
      .uppb = 100.0,
      .cost = 1.0,
      .scale = 2.0,
  });
  const auto c1 = lp.add_col({
      .lowb = 0.0,
      .uppb = 100.0,
      .cost = 0.0,
  });
  // Force c1 ≥ 5 so the LP has a non-trivial optimum we can check.
  SparseRow base_row;
  base_row[c0] = 0.0;
  base_row[c1] = 1.0;
  base_row.greater_equal(5.0);
  std::ignore = lp.add_row(std::move(base_row));

  LinearInterface li;
  LpMatrixOptions opts;
  opts.scale_objective = kScaleObj;
  opts.equilibration_method = LpEquilibrationMethod::row_max;
  li.load_flat(lp.flatten(opts));
  li.save_base_numrows();

  // Cut: 1·c0 + 0.5·c1 ≥ 30 (physical).  Picks up a non-trivial
  // composite_scale because col_scale on c0 is 2.0.
  SparseRow cut;
  cut[c0] = 1.0;
  cut[c1] = 0.5;
  cut.lowb = 30.0;
  cut.uppb = LinearProblem::DblMax;
  const auto cut_idx = li.add_row(cut);

  const auto solve_status = li.initial_solve({});
  REQUIRE(solve_status.has_value());
  REQUIRE(li.is_optimal());

  // Physical dual identity (audit Section 7.2):
  //   dual_phys = raw_dual × scale_objective / row_scale.
  // Reconstruct independently and compare against the ScaledView
  // returned by get_row_dual().
  const auto raw_duals = li.get_row_dual_raw();
  const auto phys_duals = li.get_row_dual();
  const double row_scale = li.get_row_scale(cut_idx);
  const double expected = raw_duals[cut_idx] * li.scale_objective() / row_scale;

  CHECK(phys_duals[cut_idx] == doctest::Approx(expected).epsilon(1e-10));
}

// ── Rec T2 (G-cut-2) ────────────────────────────────────────────────────────
//
// A physical cut installed via `add_row(SparseRow)` after
// `save_base_numrows()` must yield the same primal solution as the same
// row written into the original LinearProblem before `flatten()`.
// This is the strongest invariant: it pins the END-TO-END semantic
// equivalence of the two paths, including all scale composition.
TEST_CASE(
    "LinearInterface cut: pre-flatten vs post-flatten equivalence")  // NOLINT
{
  constexpr double kScaleObj = 1000.0;

  // Helper to build the same model in either form.  `as_cut=true`
  // means: flatten without the cut, then add it post-flatten as a
  // physical SparseRow.  `as_cut=false` means: include the cut as a
  // structural row before flatten.
  //
  // Model:  min c0 + c1   s.t.  c0 + c1 ≥ 5,   c0 + c1 ≥ 7   (the cut)
  //   col bounds: c0,c1 ∈ [0, 100]
  //   col_scale: c0 = 2.0, c1 = 1.0
  //   The cut binds → optimum is c0 + c1 = 7.
  auto build = [](bool as_cut)
  {
    LinearProblem lp;
    const auto c0 = lp.add_col({
        .lowb = 0.0,
        .uppb = 100.0,
        .cost = 1.0,
        .scale = 2.0,
    });
    const auto c1 = lp.add_col({
        .lowb = 0.0,
        .uppb = 100.0,
        .cost = 1.0,
    });
    // Loose constraint c0+c1 ≥ 5.
    SparseRow base;
    base[c0] = 1.0;
    base[c1] = 1.0;
    base.greater_equal(5.0);
    std::ignore = lp.add_row(std::move(base));

    if (!as_cut) {
      // Pre-flatten cut: include as a structural row.
      SparseRow cut;
      cut[c0] = 1.0;
      cut[c1] = 1.0;
      cut.greater_equal(7.0);
      std::ignore = lp.add_row(std::move(cut));
    }

    LinearInterface li;
    LpMatrixOptions opts;
    opts.scale_objective = kScaleObj;
    opts.equilibration_method = LpEquilibrationMethod::none;
    li.load_flat(lp.flatten(opts));
    li.save_base_numrows();

    if (as_cut) {
      // Post-flatten cut: install via add_row(SparseRow), exercising
      // the compose_physical path.
      SparseRow cut;
      cut[c0] = 1.0;
      cut[c1] = 1.0;
      cut.lowb = 7.0;
      cut.uppb = LinearProblem::DblMax;
      std::ignore = li.add_row(cut);
    }

    return li;
  };

  auto pre = build(false);
  auto post = build(true);

  REQUIRE(pre.initial_solve({}).has_value());
  REQUIRE(post.initial_solve({}).has_value());
  REQUIRE(pre.is_optimal());
  REQUIRE(post.is_optimal());

  // Physical objective must match (both LPs share the same physical
  // model and optimum).
  CHECK(pre.get_obj_value_physical()
        == doctest::Approx(post.get_obj_value_physical()).epsilon(1e-9));
}

// ── Solve invariance: the LP optimum must be invariant to scale_objective ──
//
// The user's headline invariant: changing `scale_objective` must NOT
// change the physical optimum.  Mathematically the OBJ is divided by
// scale_objective at flatten time and physically equivalent (same
// minimizer in physical units).  Empirically on juan/gtopt_iplp this
// invariant breaks when SDDP adds cuts at scale_obj=1000 — but the
// invariant should hold even on a TRIVIAL LP without SDDP, just from
// the linear_interface layer alone.
//
// This test pins it on a 2-col LP that exercises both the structural
// and post-flatten cut paths.  If this fails, the bug is in
// linear_interface scaling.  If it passes (which it should), the
// SDDP-side blowup is in cut accumulation / solver-tolerance
// interaction, not in the linear_interface itself.
TEST_CASE(
    "LinearInterface scale_objective invariance: same physical optimum")  // NOLINT
{
  // Build the SAME physical problem at three scale_objective values
  // and verify the physical objective + primal are identical
  // (modulo float tolerance).  Includes a post-flatten cut to
  // exercise the compose_physical path under each scale.
  //
  // Physical model:
  //   min  c0 + c1
  //   s.t. c0 + c1 ≥ 7   (cut, post-flatten)
  //        c0, c1 ∈ [0, 100]
  // Optimum: c0 + c1 = 7, obj_phys = 7.
  constexpr std::array scale_objs = {1.0, 100.0, 1000.0};
  std::vector<double> phys_objs;
  std::vector<double> c0_phys_sols;
  for (const double scale_obj : scale_objs) {
    CAPTURE(scale_obj);

    LinearProblem lp;
    const auto c0 = lp.add_col({
        .lowb = 0.0,
        .uppb = 100.0,
        .cost = 1.0,
    });
    const auto c1 = lp.add_col({
        .lowb = 0.0,
        .uppb = 100.0,
        .cost = 1.0,
    });
    SparseRow base;
    base[c0] = 1.0;
    base[c1] = 1.0;
    base.greater_equal(0.0);
    std::ignore = lp.add_row(std::move(base));

    LinearInterface li;
    LpMatrixOptions opts;
    opts.scale_objective = scale_obj;
    opts.equilibration_method = LpEquilibrationMethod::none;
    li.load_flat(lp.flatten(opts));
    li.save_base_numrows();

    SparseRow cut;
    cut[c0] = 1.0;
    cut[c1] = 1.0;
    cut.lowb = 7.0;
    cut.uppb = LinearProblem::DblMax;
    std::ignore = li.add_row(cut);

    REQUIRE(li.initial_solve({}).has_value());
    REQUIRE(li.is_optimal());

    phys_objs.push_back(li.get_obj_value_physical());
    c0_phys_sols.push_back(li.get_col_sol()[c0]);
  }

  // All three runs must converge to the SAME physical objective.
  // The expected value is 7.0 (sum of c0 + c1 when the cut binds).
  CHECK(phys_objs[0] == doctest::Approx(7.0).epsilon(1e-9));
  CHECK(phys_objs[1] == doctest::Approx(7.0).epsilon(1e-9));
  CHECK(phys_objs[2] == doctest::Approx(7.0).epsilon(1e-9));

  // The primal solution must also be invariant — the LP optimum
  // (c0=0, c1=7) or (c0=3.5, c1=3.5) etc. depends on basis selection,
  // but physical c0+c1 = 7 must hold.  Just check the OBJ here for
  // the strongest invariant.
}

// ── clone() invariance: cloned LP solves to the same physical optimum ──────
//
// `LinearInterface::clone()` creates a deep copy used by SDDP's
// aperture / elastic filter paths.  The clone must reproduce the
// original's primal-dual solution AND the physical objective.
// Critical state to preserve across clone:
//   - m_scale_objective_     (so get_obj_value_physical agrees)
//   - m_col_scales_          (so get_col_cost / get_col_sol agree)
//   - m_row_scales_          (so get_row_dual / get_row_low agree)
//   - m_equilibration_method_, m_base_numrows_, m_base_numrows_set_
//                            (so post-clone add_row dispatches correctly)
//
// The original `clone()` only copied the first three; the rest were
// silently lost — making any cut added on the clone go through the
// LP-space dispatch path with stale equilibration assumptions.
TEST_CASE(
    "LinearInterface clone(): same physical optimum across scales")  // NOLINT
{
  constexpr std::array scale_objs = {1.0, 100.0, 1000.0};

  for (const double scale_obj : scale_objs) {
    CAPTURE(scale_obj);
    SUBCASE(("scale_obj=" + std::to_string(scale_obj)).c_str())
    {
      // Build a non-trivial LP with col_scale != 1 and a cut, then
      // clone it.
      LinearProblem lp;
      const auto c0 = lp.add_col({
          .lowb = 0.0,
          .uppb = 100.0,
          .cost = 1.0,
          .scale = 2.0,
      });
      const auto c1 = lp.add_col({
          .lowb = 0.0,
          .uppb = 100.0,
          .cost = 1.0,
      });
      SparseRow base;
      base[c0] = 1.0;
      base[c1] = 1.0;
      base.greater_equal(3.0);
      std::ignore = lp.add_row(std::move(base));

      LinearInterface original;
      LpMatrixOptions opts;
      opts.scale_objective = scale_obj;
      opts.equilibration_method = LpEquilibrationMethod::row_max;
      original.load_flat(lp.flatten(opts));
      original.save_base_numrows();

      // Add a cut post-flatten on the original.
      SparseRow cut;
      cut[c0] = 1.0;
      cut[c1] = 1.0;
      cut.lowb = 7.0;
      cut.uppb = LinearProblem::DblMax;
      std::ignore = original.add_row(cut);

      // Clone BEFORE the original has been solved (production
      // ordering: SDDP forks the aperture clone from a freshly-built
      // LP, then both solve independently in parallel).  This pins
      // that clone() captures the LP structure correctly without
      // requiring a prior solve.
      auto cloned = original.clone();

      // Clone must inherit ALL scaling state.  Concrete invariants:
      CHECK(cloned.scale_objective() == doctest::Approx(scale_obj));
      // The clone backend's row count must match.
      CHECK(cloned.get_numrows() == original.get_numrows());
      CHECK(cloned.get_numcols() == original.get_numcols());

      // Solve the clone first.
      REQUIRE(cloned.initial_solve({}).has_value());
      REQUIRE(cloned.is_optimal());

      const double clone_obj_phys = cloned.get_obj_value_physical();
      const double clone_c0_phys = cloned.get_col_sol()[c0];
      const double clone_c1_phys = cloned.get_col_sol()[c1];

      // Now solve the original AFTER the clone — verifies that
      // clone() did not side-effect the original LP.  Same physical
      // optimum required.
      REQUIRE(original.initial_solve({}).has_value());
      REQUIRE(original.is_optimal());
      const double orig_obj_phys = original.get_obj_value_physical();
      const double orig_c0_phys = original.get_col_sol()[c0];
      const double orig_c1_phys = original.get_col_sol()[c1];

      CHECK(clone_obj_phys == doctest::Approx(orig_obj_phys).epsilon(1e-9));
      CHECK(clone_c0_phys == doctest::Approx(orig_c0_phys).epsilon(1e-9));
      CHECK(clone_c1_phys == doctest::Approx(orig_c1_phys).epsilon(1e-9));

      // Both primal solutions must satisfy the cut: c0_phys + c1_phys ≥ 7.
      CHECK(clone_c0_phys + clone_c1_phys >= 7.0 - 1e-9);
      CHECK(orig_c0_phys + orig_c1_phys >= 7.0 - 1e-9);
    }
  }
}

// ── clone() row-dual round-trip: physical dual must match ──────────────────
//
// SDDP's feasibility-cut builder reads `get_row_dual()` on cloned
// elastic-filter LPs (`benders_cut.cpp:229`).  If clone() does not
// copy `m_row_scales_`, the cloned LP returns a wrong physical dual
// because the ScaledView formula `raw_dual × scale_objective /
// row_scale[i]` reads `row_scale[i] = 1.0` (default) instead of the
// composite scale recorded at insertion time.
TEST_CASE("LinearInterface clone(): get_row_dual matches original")  // NOLINT
{
  constexpr double kScaleObj = 1000.0;

  LinearProblem lp;
  const auto c0 = lp.add_col({
      .lowb = 0.0,
      .uppb = 100.0,
      .cost = 1.0,
      .scale = 2.0,
  });
  SparseRow base;
  base[c0] = 1.0;
  base.greater_equal(5.0);
  const auto base_row_idx_in_lp = lp.add_row(std::move(base));
  std::ignore = base_row_idx_in_lp;

  LinearInterface original;
  LpMatrixOptions opts;
  opts.scale_objective = kScaleObj;
  opts.equilibration_method = LpEquilibrationMethod::row_max;
  original.load_flat(lp.flatten(opts));
  original.save_base_numrows();

  // Clone the original BEFORE solving — production ordering.  The
  // clone must inherit all scaling state needed for `get_row_dual()`
  // to return the correct physical dual after solve.
  auto cloned = original.clone();

  REQUIRE(cloned.initial_solve({}).has_value());
  REQUIRE(cloned.is_optimal());

  // Snapshot clone's physical duals.
  const std::size_t nrows = cloned.get_numrows();
  std::vector<double> clone_phys_duals_copy(nrows);
  {
    const auto clone_phys_duals = cloned.get_row_dual();
    for (std::size_t i = 0; i < nrows; ++i) {
      clone_phys_duals_copy[i] = clone_phys_duals[i];
    }
  }

  // Solve the original AFTER the clone — verifies clone() did not
  // perturb the original LP.
  REQUIRE(original.initial_solve({}).has_value());
  REQUIRE(original.is_optimal());

  const auto orig_phys_duals = original.get_row_dual();
  for (std::size_t i = 0; i < nrows; ++i) {
    CAPTURE(i);
    CHECK(clone_phys_duals_copy[i]
          == doctest::Approx(orig_phys_duals[i]).epsilon(1e-9));
  }
}

// ── Rec T3 (G-cut-3) ────────────────────────────────────────────────────────
//
// SDDP-style 2-step cut compounding regression with an actual solve.
// Unlike the in-file SUBCASE that checks raw lb equality of three
// blindly-installed cuts, this test installs a cut, solves, and reads
// back the physical reduced cost.  Pre-fix: this rc would be
// `scale_objective×` larger than the physical truth, and a second cut
// derived from it (the SDDP backward chain) would carry coefficients
// `scale_objective²` larger.  Post-fix the round-trip is identity.
TEST_CASE(
    "LinearInterface cut compounding regression: solved round-trip")  // NOLINT
{
  constexpr double kScaleObj = 1000.0;

  LinearProblem lp;
  const auto c0 = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 1.0,
  });
  // Trivial structural row so the LP is non-empty.
  SparseRow base;
  base[c0] = 1.0;
  base.greater_equal(0.0);
  std::ignore = lp.add_row(std::move(base));

  LinearInterface li;
  LpMatrixOptions opts;
  opts.scale_objective = kScaleObj;
  opts.equilibration_method = LpEquilibrationMethod::none;
  li.load_flat(lp.flatten(opts));
  li.save_base_numrows();

  // Cut: c0 ≥ 5 (physical).  Solving forces c0 = 5, obj_phys = 5.
  SparseRow cut;
  cut[c0] = 1.0;
  cut.lowb = 5.0;
  cut.uppb = LinearProblem::DblMax;
  std::ignore = li.add_row(cut);

  REQUIRE(li.initial_solve({}).has_value());
  REQUIRE(li.is_optimal());

  // Physical objective must equal the cut RHS.  Pre-fix, the cut was
  // installed as `c0 ≥ 5000` in LP-space (no /scale_obj), forcing
  // c0_LP=5000 and obj_LP=5000 → obj_phys = 5000 × 1000 = 5e+6
  // (compared to the correct 5).  This single CHECK pins the entire
  // dimensional consistency of the compose_physical fix.
  CHECK(li.get_obj_value_physical() == doctest::Approx(5.0).epsilon(1e-9));

  // The physical primal must also be 5 (not 5000).
  const auto sol_phys = li.get_col_sol();
  CHECK(sol_phys[c0] == doctest::Approx(5.0).epsilon(1e-9));
}

// ── Coverage extension: lp_space dispatch path ──────────────────────────────
//
// When neither col_scales nor equilibration are active, `add_row(SparseRow)`
// short-circuits to `add_row_raw()` (linear_interface.cpp:964) instead
// of compose_physical.  The fix's /scale_objective only lives in the
// compose_physical branch, so this path stores the cut verbatim — which is
// correct ONLY because col_scale=1 and equilibration=none means physical
// already equals LP space.  Pin that path so a regression that accidentally
// disables the dispatch gate is caught.
TEST_CASE(
    "LinearInterface add_row lp_space dispatch (no col_scales)")  // NOLINT
{
  // Build with NO col_scale != 1 anywhere AND no equilibration.
  LinearProblem lp;
  const auto c0 = lp.add_col({
      .lowb = 0.0,
      .uppb = 100.0,
      .cost = 1.0,
  });
  SparseRow base;
  base[c0] = 1.0;
  base.greater_equal(0.0);
  std::ignore = lp.add_row(std::move(base));

  LinearInterface li;
  LpMatrixOptions opts;
  opts.scale_objective = 1.0;  // disable obj scaling so dispatch == lp_space
  opts.equilibration_method = LpEquilibrationMethod::none;
  li.load_flat(lp.flatten(opts));
  li.save_base_numrows();

  // With scale_obj=1 and no col_scale, the cut row should land
  // verbatim — the lp_space dispatch path neither multiplies nor
  // divides anything (other than SparseRow::scale, which we leave
  // at default 1.0).
  SparseRow cut;
  cut[c0] = 1.0;
  cut.lowb = 7.0;
  cut.uppb = LinearProblem::DblMax;
  const auto row_idx = li.add_row(cut);

  CHECK(li.get_row_low_raw()[row_idx] == doctest::Approx(7.0));
}
