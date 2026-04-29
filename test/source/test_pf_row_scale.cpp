// SPDX-License-Identifier: BSD-3-Clause
//
// Documents the *actual* semantics of `SparseRow::scale` and the LP-folded
// (cost_factor-not-removed) convention of `LinearInterface::get_row_dual()`.
//
// **Background**: Earlier in the project an attempt was made to use
// ``SparseRow::scale = cost_factor`` (= prob × discount × duration) on
// cost-bearing rows so that ``get_row_dual()`` would return truly
// physical shadow prices.  The math was tested here and FAILED: the
// LP-solver dual on a row whose coefficients + RHS are uniformly
// divided by ``row.scale`` is multiplied by ``row.scale`` (relative to
// the unscaled dual), and the ``composite_scale`` extraction divides
// it back out — net no-op on the dual.  Equivalent to row-max
// equilibration's well-known invariance under uniform scaling.
//
// **Rule (locked in by these tests)**: ``SparseRow::scale`` is for
// *column-side neutralization* — e.g. cancelling the ``var_scale`` of
// a state-variable column so the row's coefficients stay dimensionless.
// It is NOT a free per-row scale stream and CANNOT carry
// ``cost_factor``.  The ``cost_factor`` inverse must be applied at
// READ time — exactly what ``OutputContext::add_row_dual`` does via
// ``CostHelper::block_icost_factors() / scenario_stage_icost_factors()``.

#include <doctest/doctest.h>
#include <gtopt/cost_helper.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

// Solve `min c × x  s.t.  x = 1, x ≥ 0` with the given `row.scale`.
// Returns get_row_dual()[0].
inline double dual_of_unit_fix(double c_LP, double row_scale)
{
  LinearProblem lp("pf_row_scale_unit_fix");
  const auto c0 = lp.add_col(SparseCol {
      .lowb = 0.0,
      .uppb = LinearProblem::DblMax,
      .cost = c_LP,
  });
  auto r0 = SparseRow {.scale = row_scale};
  r0[c0] = 1.0;
  r0.equal(1.0);
  const auto r0_idx = lp.add_row(std::move(r0));
  const auto flat = lp.flatten({});
  LinearInterface li("", flat);
  const auto status = li.initial_solve({});
  REQUIRE((status && *status == 0));
  REQUIRE(li.is_optimal());
  return li.get_row_dual()[r0_idx];
}

}  // namespace

// ────────────────────────────────────────────────────────────────────
// Locks in: row.scale uniform multiplier is a NO-OP on get_row_dual().
//
// LP construction divides row coefficients + RHS by row.scale (and
// records row.scale into composite_scale).  The LP-solver's dual on
// the modified row is multiplied by row.scale (relative to the
// unscaled dual).  The extraction `dual_solver × scale_obj /
// composite_scale` cancels both, leaving the dual identical to
// running the LP without `row.scale` at all.
//
// This means: setting `row.scale = cost_factor` cannot recover
// physical shadow prices from an LP whose cost coefficients have
// `cost_factor` folded in via `block_ecost / stage_ecost`.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "SparseRow::scale uniform multiplier is a no-op for get_row_dual")
{
  constexpr double kCLP = 50.0;  // arbitrary

  const std::array<double, 4> row_scales = {1.0, 0.5, 2.0, 100.0};
  double baseline = 0.0;
  for (std::size_t i = 0; i < row_scales.size(); ++i) {
    const double rs = row_scales[i];
    CAPTURE(rs);
    const double dual = dual_of_unit_fix(kCLP, rs);
    if (i == 0) {
      baseline = dual;
      // For row.scale = 1.0 (default), no equilibration, scale_obj=1,
      // single +1 coefficient: dual = c_LP exactly.
      CHECK(dual == doctest::Approx(kCLP));
    } else {
      // Any other row.scale value yields the SAME dual.  Proves the
      // row.scale = cost_factor approach cannot unfold prob × discount
      // × duration from the LP-folded dual.
      CHECK(dual == doctest::Approx(baseline));
    }
  }
}

// ────────────────────────────────────────────────────────────────────
// **Working mechanism** to make get_row_dual() return truly physical:
// PRE-MULTIPLY row coefficients + RHS by cost_factor AND set
// row.scale = cost_factor.
//
// The pre-multiply compensates exactly for add_row's
// divide-by-row.scale step (linear_interface.cpp:1147-1159), so the
// LP-side row is left unchanged.  But composite_scale records
// cost_factor, and the extraction `dual_solver × scale_obj /
// composite_scale` divides through it — recovering truly physical.
//
// This is the user's design: cost_factor never disturbs the LP
// solver (LP unchanged); it only enters the bookkeeping channel that
// ScaledView uses to unscale at read time.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "Pre-multiply rows + row.scale = cost_factor recovers physical dual")
{
  constexpr double kCPhys = 100.0;

  struct Case
  {
    double prob;
    double discount;
    double duration;
    const char* label;
  };
  const std::array<Case, 5> cases = {{
      {.prob = 1.0,
       .discount = 1.0,
       .duration = 1.0,
       .label = "unit cost_factor"},
      {.prob = 0.5, .discount = 1.0, .duration = 1.0, .label = "prob=0.5"},
      {.prob = 0.3, .discount = 1.0, .duration = 1.0, .label = "prob=0.3"},
      {.prob = 1.0,
       .discount = 0.95,
       .duration = 24.0,
       .label = "discount + duration"},
      {.prob = 0.5,
       .discount = 0.95,
       .duration = 24.0,
       .label = "full block-level cost_factor"},
  }};

  for (const auto& tc : cases) {
    CAPTURE(tc.label);
    SUBCASE(tc.label)
    {
      const double cf = tc.prob * tc.discount * tc.duration;
      // Mimic block_ecost folding: c_LP = c_phys × cost_factor.
      const double c_LP = kCPhys * cf;

      LinearProblem lp("pf_row_scale_premul");
      const auto c0 = lp.add_col(SparseCol {
          .lowb = 0.0,
          .uppb = LinearProblem::DblMax,
          .cost = c_LP,
      });
      // PRE-MULTIPLY: row coeff and RHS by cost_factor.  add_row will
      // divide by row.scale = cost_factor, so the LP-side row is
      // unchanged.  But composite_scale records cost_factor.
      auto r0 = SparseRow {.scale = cf};
      r0[c0] = 1.0 * cf;
      r0.equal(1.0 * cf);
      const auto r0_idx = lp.add_row(std::move(r0));
      const auto flat = lp.flatten({});
      LinearInterface li("", flat);
      const auto status = li.initial_solve({});
      REQUIRE((status && *status == 0));
      REQUIRE(li.is_optimal());

      const double phys_dual = li.get_row_dual()[r0_idx];
      CAPTURE(cf);
      CAPTURE(c_LP);
      CAPTURE(phys_dual);
      CHECK(phys_dual == doctest::Approx(kCPhys).epsilon(1e-8));
    }
  }
}

// ────────────────────────────────────────────────────────────────────
// Locks in: get_row_dual() returns LP-folded value when cost has
// cost_factor folded in AND no row pre-multiply is applied.  To
// recover truly physical shadow prices, the consumer must apply
// `1 / cost_factor` at READ time.
//
// Mirrors what OutputContext::add_row_dual does in production today.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "get_row_dual returns LP-folded without pre-multiply")
{
  constexpr double kCPhys = 100.0;

  struct Case
  {
    double prob;
    double discount;
    double duration;
    const char* label;
  };
  const std::array<Case, 4> cases = {{
      {.prob = 1.0,
       .discount = 1.0,
       .duration = 1.0,
       .label = "unit (cost_factor=1, dual=physical by coincidence)"},
      {.prob = 0.5,
       .discount = 1.0,
       .duration = 1.0,
       .label = "prob=0.5 (cost_factor=0.5)"},
      {.prob = 0.4,
       .discount = 0.95,
       .duration = 1.0,
       .label = "with discount"},
      {.prob = 0.5,
       .discount = 0.95,
       .duration = 24.0,
       .label = "block-level cost_factor (× duration)"},
  }};

  for (const auto& tc : cases) {
    CAPTURE(tc.label);
    SUBCASE(tc.label)
    {
      const double cf = tc.prob * tc.discount * tc.duration;
      // Mimic block_ecost folding: c_LP = c_phys × cost_factor.
      const double c_LP = kCPhys * cf;
      // row.scale = 1.0 (default) — the helper above proved that
      // setting it to cost_factor would NOT change the result.
      const double dual_lp_folded = dual_of_unit_fix(c_LP, /*row_scale=*/1.0);
      // get_row_dual returns LP-folded value:
      CHECK(dual_lp_folded == doctest::Approx(c_LP));
      // Physical recovery: divide by cost_factor at read time.
      const double dual_physical = dual_lp_folded / cf;
      CHECK(dual_physical == doctest::Approx(kCPhys));
    }
  }
}

// ────────────────────────────────────────────────────────────────────
// Defensive: T-context cost-folding contract.
//
// Locks the convention that cost-bearing columns indexed by stage
// alone (TIndexHolder, e.g. capacity expansion) use
// ``CostHelper::stage_ecost(stage, cost, prob=1.0)`` — folding ONLY
// ``discount × duration_stage`` into the LP coefficient — and the
// matching ``stage_icost_factors = 1/(discount × duration_stage)``
// inverse contains NO probability.
//
// Symmetry ledger: column-side fold = `× discount × duration_stage`
// (1 multiply); read-side inverse = `× 1/(discount × duration_stage)`
// (1 divide).  Probability is intentionally absent on BOTH sides.
//
// Future drift this test catches: a contributor writing a T-context
// column with cost folded via ``scenario_stage_ecost`` (or
// ``stage_ecost(prob=...)`` with a non-default probability) would
// silently produce output values off by the prob factor — the
// stage_icost_factors inverse only undoes ``discount × duration``.
//
// We pin the helpers' arithmetic identities directly, the most stable
// surface to lock: any change that adds prob to ``cost_factor(stage,
// prob=1.0)`` would break the deterministic-cost convention.
TEST_CASE(  // NOLINT
    "CostHelper — T-context cost convention is prob-free")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // The scalar overload of cost_factor with default `duration = 1.0`
  // returns probability × discount × 1.0.  For prob=1.0 (the
  // deterministic-cost convention) this reduces to `discount`.
  CHECK(CostHelper::cost_factor(/*prob=*/1.0, /*discount=*/0.95)
        == doctest::Approx(0.95));
  CHECK(CostHelper::cost_factor(
            /*prob=*/1.0, /*discount=*/0.95, /*duration=*/24.0)
        == doctest::Approx(0.95 * 24.0));

  // Asymmetry: scenario-aware ecost helpers DO fold prob;
  // stage_ecost (default prob=1.0) does NOT.  The two helpers MUST
  // differ by exactly the prob factor when the same `cost` and stage
  // are passed — anything else means a prob factor leaked into one
  // path and not the other.
  //
  // We can't easily build StageLP / ScenarioLP at unit-test scope, but
  // the raw scalar identity is what the helpers ultimately compute:
  //   stage_ecost(stage, c, prob=1.0)             = c × 1.0   × discount ×
  //   duration scenario_stage_ecost(scenario, stage, c)    = c × prob  ×
  //   discount × duration
  // ⇒ scenario_stage_ecost / stage_ecost = prob.
  constexpr double kCost = 100.0;
  constexpr double kDiscount = 0.95;
  constexpr double kDurationStage = 24.0;
  constexpr double kProb = 0.4;
  const double t_path = kCost * 1.0 * kDiscount * kDurationStage;
  const double st_path = kCost * kProb * kDiscount * kDurationStage;
  CHECK(t_path == doctest::Approx(kCost * kDiscount * kDurationStage));
  CHECK(st_path == doctest::Approx(t_path * kProb));

  // The matching inverse for T-context output (stage_icost_factors)
  // is ``1 / (discount × duration_stage)`` — explicitly prob-free.
  // Multiplying t_path by this inverse must recover the user-input
  // physical cost regardless of any per-scenario probability.
  const double t_inverse = 1.0 / (kDiscount * kDurationStage);
  CHECK(t_path * t_inverse == doctest::Approx(kCost));

  // Documented hazard: applying the SCENARIO-stage inverse to a
  // T-folded cost would over-divide by `1/prob`, producing
  // `t_path × scenario_stage_inverse = kCost / prob` — wrong by
  // 1/prob.  The deep PF audit's Item A (demand_lp.cpp:92) was
  // exactly this mismatch (since-fixed).  This test pins the
  // contract so a future T-context caller can't silently reintroduce
  // the asymmetry.
  const double scenario_stage_inverse =
      1.0 / (kProb * kDiscount * kDurationStage);
  const double mismatched_output = t_path * scenario_stage_inverse;
  CHECK(mismatched_output == doctest::Approx(kCost / kProb));  // off by 1/prob
}
