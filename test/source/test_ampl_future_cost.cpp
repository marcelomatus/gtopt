// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_ampl_future_cost.cpp
 * @brief     Gold-standard tests for AmplFutureCost (piece 5, step 2)
 * @date      2026-06-22
 * @copyright BSD-3-Clause
 *
 * Piece 5 step 2 of the FutureCost / UserModel refactor: the user-overridable
 * FCF.  When a `FutureCost` element carries `use_user_alpha = true`, the
 * built-in boundary-cut α (`varphi_s`, priced 1/N) is registered INERT (cost 0,
 * not a state variable) and the modeller's own global `state`/`link` α
 * `DecisionVariable` + global `UserConstraint` cut row(s) drive the cost-to-go
 * instead.  `boundary_cuts_file` and `use_user_alpha` are mutually exclusive.
 *
 * The GOLD-STANDARD EQUIVALENCE TEST builds the SAME tiny SDDP case TWICE:
 *   (A) a `boundary_cuts.csv` FCF: a single constant cut `α ≥ RHS`;
 *   (B) the equivalent user-authored global α `DecisionVariable`
 *       (`cost = 1.0`, free below, `state`/`link`) + a global `UserConstraint`
 *       `α ≥ RHS` + `use_user_alpha`,
 * and asserts the converged LB / UB match within tolerance.  Both pin the
 * cost-to-go at `RHS`, so the converged bounds are `baseline + RHS` on each
 * side.  This is the proof AmplFutureCost is correct, not just "runs".
 *
 * ### Pricing convention (proven by the equivalence test)
 * The built-in α column is registered with physical `cost = 1/N` (= 1.0 for the
 * single-scene single-α layout) on a column scaled by `scale_alpha`, so its
 * physical objective contribution is the realised cost-to-go.  The user α must
 * carry the SAME physical weight: a `DecisionVariable` with `cost = 1.0` and
 * `cost_type = "raw"` (face value — NO probability / discount / duration
 * weighting), free below (cost-to-go may be negative).  A coefficient of 1.0 in
 * the user `UserConstraint` (`decision_variable('α').value >= RHS`) matches the
 * boundary cut's `α + Σ(−gᵢ)·sᵢ ≥ rhs` row at `gᵢ = 0`.
 *
 * Plus: schema round-trip, the mutual-exclusion `std::unexpected`, the cost==0
 * guard error.
 */

#include <filesystem>
#include <fstream>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/decision_variable.hpp>
#include <gtopt/future_cost.hpp>
#include <gtopt/future_cost_lp.hpp>
#include <gtopt/json/json_future_cost.hpp>
#include <gtopt/monolithic_method.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace amplfcf_test  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

constexpr Uid kUserAlphaUid {4242};

/// A single-phase MONOLITHIC hydro fixture (1 phase × 1 stage × 4 hourly
/// blocks) on the single-bus hydro+thermal+reservoir topology.  MONOLITHIC
/// assembles the problem as ONE LP — there is NO SDDP backward pass, so the
/// user α + cut and the boundary cut are plain LP structure, and a single phase
/// means the FCF cost-to-go is a pure terminal constant.  This is the setting
/// for a numerically-exact user-α vs boundary-cut equivalence; the dynamic SDDP
/// cross-phase recourse over a user α is deferred (step 2c — the SDDP backward
/// pass is hardwired to the built-in α column).
auto make_monolithic_planning() -> Planning
{
  auto planning = make_2phase_linear_planning();  // hydro+thermal+reservoir
  planning.simulation.stage_array.resize(1);  // keep stage 1 (blocks 0-3)
  planning.simulation.phase_array.resize(1);
  planning.options.method = MethodType::monolithic;
  return planning;
}

/// The single-phase monolithic fixture with a user-authored global α
/// `DecisionVariable` (`cost = 1.0`, free below) + a global `UserConstraint`
/// cut `α ≥ rhs`, wired to a `FutureCost` via `use_user_alpha` /
/// `user_alpha_uid`. One phase ⇒ one (scene, phase) cell ⇒ exactly one α column
/// + one cut row, numerically matching the single terminal boundary cut.
auto make_user_alpha_planning(double rhs, double alpha_cost = 1.0) -> Planning
{
  auto planning = make_monolithic_planning();

  // The user α: a global column priced cost = 1.0 (raw face value — the
  // canonical 1:1 cost-to-go weight), free below so α may go negative.
  planning.system.decision_variable_array.push_back(DecisionVariable {
      .uid = kUserAlphaUid,
      .name = "user_alpha",
      .lower_bound = OptReal {-1.0e9},
      .upper_bound = OptReal {1.0e9},
      .cost = OptReal {alpha_cost},
      .cost_type = OptName {"raw"},
      .scope = OptName {"global"},
  });

  // The user cut: a global `α ≥ rhs` (a constant cost-to-go floor, exactly the
  // coef-0 boundary cut).  One per (scene, phase) cell = one row here.
  planning.system.user_constraint_array.push_back(UserConstraint {
      .uid = Uid {4243},
      .name = "fcf_cut",
      .expression = Name {std::format(
          "decision_variable('user_alpha').value >= {}", rhs)},
      .constraint_type = OptName {"raw"},
      .scope = OptName {"global"},
  });

  // The FutureCost element selecting the user α as the FCF.
  planning.system.future_cost_array.push_back(FutureCost {
      .uid = Uid {1},
      .name = "ufcf",
      .use_user_alpha = OptBool {true},
      .user_alpha_uid = OptUid {kUserAlphaUid},
  });

  return planning;
}

/// A 2-phase SDDP variant of `make_user_alpha_planning`, used only for the
/// init-time guard tests (mutual exclusion / cost==0) — those guards fire in
/// `SDDPMethod::initialize_solver` and return `std::unexpected` BEFORE the
/// SDDP backward pass, so the (deferred) dynamic-recourse crash is never
/// reached.  `state`/`link` is omitted (the guards don't need it).
auto make_user_alpha_sddp_planning(double rhs, double alpha_cost) -> Planning
{
  auto planning = make_2phase_linear_planning();  // 2 phases (SDDP needs ≥2)
  planning.options.method = MethodType::sddp;
  planning.system.decision_variable_array.push_back(DecisionVariable {
      .uid = kUserAlphaUid,
      .name = "user_alpha",
      .lower_bound = OptReal {-1.0e9},
      .upper_bound = OptReal {1.0e9},
      .cost = OptReal {alpha_cost},
      .cost_type = OptName {"raw"},
      .scope = OptName {"global"},
  });
  planning.system.user_constraint_array.push_back(UserConstraint {
      .uid = Uid {4243},
      .name = "fcf_cut",
      .expression = Name {std::format(
          "decision_variable('user_alpha').value >= {}", rhs)},
      .constraint_type = OptName {"raw"},
      .scope = OptName {"global"},
  });
  planning.system.future_cost_array.push_back(FutureCost {
      .uid = Uid {1},
      .name = "ufcf",
      .use_user_alpha = OptBool {true},
      .user_alpha_uid = OptUid {kUserAlphaUid},
  });
  return planning;
}

/// Solve @p planning via the monolithic method (loading @p boundary_cuts_file
/// when non-empty) and return the realised objective of the single cell.
[[nodiscard]] auto solve_monolithic_obj(Planning planning,
                                        const std::string& boundary_cuts_file)
    -> double
{
  PlanningLP planning_lp(std::move(planning));
  MonolithicMethod mm;
  mm.enable_api = false;
  mm.boundary_cuts_file = boundary_cuts_file;
  mm.boundary_cuts_mode = BoundaryCutsMode::combined;
  const SolverOptions opts {};
  const auto rc = mm.solve(planning_lp, opts);
  REQUIRE(rc.has_value());
  auto&& systems = planning_lp.systems();
  REQUIRE(!systems.empty());
  REQUIRE(!systems.front().empty());
  return systems.front().front().linear_interface().get_obj_value();
}

}  // namespace
}  // namespace amplfcf_test

// ─── 1. Schema round-trip ──────────────────────────────────────────────────

TEST_CASE("AmplFutureCost — schema round-trip of use_user_alpha")  // NOLINT
{
  const std::string js = R"({
      "uid": 1, "name": "ufcf",
      "use_user_alpha": true, "user_alpha_uid": 4242
  })";
  const auto fc = daw::json::from_json<FutureCost>(js);
  CHECK(fc.uid == Uid {1});
  CHECK(fc.use_user_alpha.value_or(false));
  CHECK((fc.user_alpha_uid && *fc.user_alpha_uid == Uid {4242}));

  const auto round = daw::json::to_json(fc);
  const auto fc2 = daw::json::from_json<FutureCost>(round);
  CHECK(fc2.use_user_alpha.value_or(false));
  CHECK((fc2.user_alpha_uid && *fc2.user_alpha_uid == Uid {4242}));

  // Default (legacy boundary-cut path): both unset.
  const auto def = daw::json::from_json<FutureCost>(
      R"({ "uid": 2, "name": "bfcf", "cuts_file": "boundary_cuts.csv" })");
  CHECK_FALSE(def.use_user_alpha.value_or(false));
  CHECK_FALSE(def.user_alpha_uid.has_value());
}

// ─── 1b. Schema round-trip of single_cut_equality ──────────────────────────

TEST_CASE(
    "AmplFutureCost — schema round-trip of single_cut_equality")  // NOLINT
{
  // Explicit `false`: keep the slack-able `≥` form even for a single cut.
  const auto off = daw::json::from_json<FutureCost>(
      R"({ "uid": 1, "name": "bfcf", "cuts_file": "boundary_cuts.csv",
           "single_cut_equality": false })");
  CHECK(off.single_cut_equality.has_value());
  CHECK_FALSE(off.single_cut_equality.value_or(true));

  // Round-trip preserves the explicit false.
  const auto round = daw::json::from_json<FutureCost>(daw::json::to_json(off));
  CHECK(round.single_cut_equality.has_value());
  CHECK_FALSE(round.single_cut_equality.value_or(true));

  // Explicit `true`.
  const auto on = daw::json::from_json<FutureCost>(
      R"({ "uid": 2, "name": "bfcf", "single_cut_equality": true })");
  CHECK(on.single_cut_equality.value_or(false));

  // Unset ⇒ the legacy single-cut equality behaviour (value_or(true)).
  const auto def = daw::json::from_json<FutureCost>(
      R"({ "uid": 3, "name": "bfcf", "cuts_file": "boundary_cuts.csv" })");
  CHECK_FALSE(def.single_cut_equality.has_value());
  CHECK(def.single_cut_equality.value_or(true));  // default = current behaviour
}

// ─── 2. GOLD-STANDARD EQUIVALENCE: user α vs boundary-cut FCF ───────────────

TEST_CASE(  // NOLINT
    "AmplFutureCost — user α matches the equivalent boundary-cut FCF (obj)")
{
  using namespace amplfcf_test;  // NOLINT(google-build-using-namespace)

  // ── Baseline: plain monolithic single-phase solve (to size the cut RHS) ──
  const double baseline_obj =
      solve_monolithic_obj(make_monolithic_planning(), /*boundary=*/"");
  REQUIRE(std::isfinite(baseline_obj));

  // A clearly-binding constant cost-to-go floor.
  const double rhs = (5.0 * std::abs(baseline_obj)) + 1000.0;
  CAPTURE(baseline_obj);
  CAPTURE(rhs);

  // ── (A) boundary-cuts.csv FCF: single constant cut `α ≥ rhs` ──
  // One phase → the boundary cut installs the terminal `α = rhs` equality
  // directly in the LP, so the objective is `baseline_obj + rhs`.
  const auto cuts_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_amplfcf_bdr.csv")
          .string();
  {
    std::ofstream ofs(cuts_file);
    ofs << "iteration,scene,rhs,rsv1\n";
    ofs << "1,1," << rhs << ",0.0\n";  // coef 0 on rsv1 → constant FCF
  }
  const double bdr_obj =
      solve_monolithic_obj(make_monolithic_planning(), cuts_file);
  std::filesystem::remove(cuts_file);
  REQUIRE(std::isfinite(bdr_obj));

  // ── (B) user-authored α + cut + use_user_alpha ──
  const double usr_obj =
      solve_monolithic_obj(make_user_alpha_planning(rhs), /*boundary=*/"");
  REQUIRE(std::isfinite(usr_obj));

  // ── Equivalence: both add `rhs` once to the baseline objective. ──
  CAPTURE(bdr_obj);
  CAPTURE(usr_obj);
  const double expected = baseline_obj + rhs;
  // PRICING CONVENTION (proven here): the user α DecisionVariable with
  // `cost = 1.0`, `cost_type = "raw"` contributes exactly `rhs · 1.0` to the
  // objective when its cut forces `α = rhs` — identical to the boundary cut's
  // physical α (cost 1.0) at `α = rhs`.
  CHECK(bdr_obj == doctest::Approx(expected).epsilon(1e-3));
  CHECK(usr_obj == doctest::Approx(bdr_obj).epsilon(1e-3));
  CHECK(usr_obj == doctest::Approx(expected).epsilon(1e-3));
}

// ─── 3. Mutual exclusion: use_user_alpha + boundary_cuts_file ──────────────

TEST_CASE(  // NOLINT
    "AmplFutureCost — use_user_alpha + boundary_cuts_file is std::unexpected")
{
  using namespace amplfcf_test;  // NOLINT(google-build-using-namespace)

  const auto cuts_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_amplfcf_mx.csv")
          .string();
  {
    std::ofstream ofs(cuts_file);
    ofs << "iteration,scene,rhs,rsv1\n1,1,1000.0,0.0\n";
  }

  PlanningLP planning_lp(make_user_alpha_sddp_planning(1000.0, 1.0));
  SDDPOptions opts;
  opts.max_iterations = 1;
  opts.enable_api = false;
  opts.boundary_cuts_file = cuts_file;  // BOTH active → error

  SDDPMethod sddp(planning_lp, opts);
  auto results = sddp.solve();
  CHECK_FALSE(results.has_value());  // std::unexpected, not a throw
  if (!results.has_value()) {
    CHECK(results.error().code == ErrorCode::InvalidInput);
  }
  std::filesystem::remove(cuts_file);
}

// ─── 4. cost == 0 guard: unpriced user α is a hard error ───────────────────

TEST_CASE(
    "AmplFutureCost — user α with cost == 0 is std::unexpected")  // NOLINT
{
  using namespace amplfcf_test;  // NOLINT(google-build-using-namespace)

  PlanningLP planning_lp(
      make_user_alpha_sddp_planning(1000.0, /*alpha_cost=*/0.0));
  SDDPOptions opts;
  opts.max_iterations = 1;
  opts.enable_api = false;

  SDDPMethod sddp(planning_lp, opts);
  auto results = sddp.solve();
  CHECK_FALSE(results.has_value());
  if (!results.has_value()) {
    CHECK(results.error().code == ErrorCode::InvalidInput);
  }
}

// ─── 5. Config consolidation (step 2b): element beats SDDPOptions ──────────

TEST_CASE(  // NOLINT
    "AmplFutureCost — FutureCost.mean_shift=false beats SDDPOptions=true")
{
  using namespace amplfcf_test;  // NOLINT(google-build-using-namespace)

  // A boundary cut on the 3-phase fixture; a FutureCost element authors the
  // boundary fields (cuts_file + mean_shift=false).  SDDPOptions requests
  // mean_shift=true.  Element wins ⇒ no rebase ⇒ scene_alpha_offset == 0
  // (the byte-for-byte mean_shift-OFF behaviour).
  const auto cuts_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_amplfcf_2b.csv")
          .string();
  {
    std::ofstream ofs(cuts_file);
    ofs << "iteration,scene,rhs,rsv1\n1,1,1000.0,2.0\n";
  }

  auto planning = make_3phase_hydro_planning();
  planning.options.method = MethodType::sddp;
  planning.system.future_cost_array.push_back(FutureCost {
      .uid = Uid {1},
      .name = "bfcf",
      .cuts_file = OptName {cuts_file},
      .mean_shift = OptBool {false},  // element OVERRIDES the option below
      .mode = std::optional<BoundaryCutsMode> {BoundaryCutsMode::combined},
  });
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 1;
  opts.convergence_tol = 1e-6;
  opts.enable_api = false;
  opts.boundary_cuts_mean_shift = true;  // option says ON; element says OFF

  SDDPMethod sddp(planning_lp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  // mean_shift OFF (element won) ⇒ no per-scene rebase offset.
  CHECK(sddp.scene_alpha_offset(SceneIndex {0}) == 0.0);

  std::filesystem::remove(cuts_file);
}

// ─── 6. AmplFutureCost 2c — DYNAMIC multi-phase SDDP recourse over a user α ──
//
// The proof that a USER-authored α is a correct SDDP backward-pass recourse
// column: build the 3-phase hydro fixture with a user-authored global
// `state`/`link` α `DecisionVariable` (cost 1.0 raw, free below) + a TERMINAL
// `UserConstraint`
//   `user_alpha + W·reservoir('rsv1').efin ≥ R,  for(stage in {3})`
// + `FutureCost{use_user_alpha=true}`, run SDDP, and assert the user-α policy
// converges — self-consistently (LB ≤ UB) — to the INDEPENDENTLY-DERIVED
// analytic optimum of the deterministic-equivalent multi-stage problem.
//
// Why an analytic oracle here: the built-in `boundary_cuts.csv` path, AS
// CONFIGURED BY DEFAULT, applies transformations the user-α path deliberately
// does not — the single-cut `≥`→`=` + free-α terminal-value conversion (a
// DELIBERATE, correct, and now CONFIGURABLE form, only ever for a single cut;
// many cuts always keep `≥`), the `mean_shift` α-rebase, and
// `apply_alpha_floor`'s projection of the cut onto the worst-case state box.
// Under those defaults the two are different formulations whose bounds need not
// coincide, so the analytic optimum is the rigorous, non-circular invariant.
// (Set `FutureCost.single_cut_equality = false` and `mean_shift = false` and
// the single-cut boundary path BECOMES the same `≥` formulation as this user-α
// cut — see test 8, the direct faithful-oracle cross-run that asserts the
// converged bounds match.)
//
// Cut design (W, R chosen for a UNIQUE, non-degenerate, binding optimum):
//   * W = 90 $/dam³ is STRICTLY above the hydro−thermal differential
//     ($50 − $5 = $45/dam³), so every saved dam³ is worth $45 more in the cut
//     than the foregone dispatch it costs → the LP drives terminal storage to
//     its UNIQUE upper corner (emax = 500).  W = 45 (= the differential) would
//     leave terminal efin DEGENERATE (a flat objective face over [0, 500]).
//   * R = W·emax = 90·500 = 45000 makes α = R − W·efin = 0 at the optimum, so
//     UB == LB (gap → 0) at the unique corner.
//
// Analytic optimum (first-principles arithmetic on the fixture parameters):
//   terminal efin = 500 (forced) ⇒ net storage +250 over the 3-phase horizon.
//   water dispatched = eini(250) + 3·inflow(240) − efin(500) = 470 dam³ hydro.
//   demand = 100 MW · 72 h = 7200 MWh.
//   thermal = (7200 − 470) MWh · $50 = $336500;  hydro = 470 · $5 = $2350.
//   terminal α = max(0, 45000 − 90·500) = 0.
//   ⇒ optimum = $338850.
//
// The reservoir terminal column is the AMPL `efin` variable
// (`StorageLP::EfinName`).  The `for(stage in {3})` clause restricts the user
// cut to the terminal phase (rep_stage uid = 3); a `global` UserConstraint
// otherwise fires at EVERY phase, adding intermediate-phase floors the FCF
// must not impose.
TEST_CASE(  // NOLINT
    "AmplFutureCost 2c — user α drives SDDP recourse to the analytic optimum")
{
  using namespace amplfcf_test;  // NOLINT(google-build-using-namespace)

  constexpr double kW =
      90.0;  // $/dam³ — STRICTLY > differential → unique corner
  constexpr double kR = 45000.0;  // = W·emax → unique efin = 500, α = 0
  constexpr double kAnalytic = 338850.0;  // hand-derived deterministic optimum
  constexpr Uid kUaUid {4244};

  auto planning = make_3phase_hydro_planning();
  planning.options.method = MethodType::sddp;
  // Global state/link α DecisionVariable, priced cost = 1.0 raw, free below.
  planning.system.decision_variable_array.push_back(DecisionVariable {
      .uid = kUaUid,
      .name = "user_alpha",
      .lower_bound = OptReal {-1.0e9},
      .cost = OptReal {1.0},
      .cost_type = OptName {"raw"},
      .scope = OptName {"global"},
      .state = OptBool {true},
      .link = OptBool {true},
  });
  // Terminal cut: user_alpha + W·efin ≥ R, restricted to the terminal stage
  // (uid 3) so it installs ONLY on the last phase.
  planning.system.user_constraint_array.push_back(UserConstraint {
      .uid = Uid {4245},
      .name = "fcf_cut",
      .expression = Name {std::format(
          "decision_variable('user_alpha').value + {}*reservoir('rsv1').efin "
          ">= {}, for(stage in {{3}})",
          kW,
          kR)},
      .constraint_type = OptName {"raw"},
      .scope = OptName {"global"},
  });
  planning.system.future_cost_array.push_back(FutureCost {
      .uid = Uid {1},
      .name = "ufcf",
      .use_user_alpha = OptBool {true},
      .user_alpha_uid = OptUid {kUaUid},
  });

  PlanningLP planning_lp(std::move(planning));
  SDDPOptions opts;
  // gap_only + stationary OFF so the forward policy is driven to the unique
  // deterministic optimum (no early ΔUB stationary stop).  20 iters is ample
  // for this 3-phase fixture (gap closes within ~5).
  opts.max_iterations = 20;
  opts.convergence_tol = 1e-4;
  opts.convergence_mode = ConvergenceMode::gap_only;
  opts.stationary_tol = 0.0;
  opts.cut_sharing = CutSharingMode::none;
  opts.aperture_chunk_size = -1;  // aperture disabled (increment A scope)
  opts.enable_api = false;

  SDDPMethod sddp(planning_lp, opts);
  const auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  const double lb = results->back().lower_bound;
  const double ub = results->back().upper_bound;
  const double gap = results->back().gap;
  CAPTURE(lb);
  CAPTURE(ub);
  CAPTURE(gap);
  CAPTURE(kAnalytic);

  // (1) SDDP bound consistency: no LB overshoot (gap ≥ −fp-noise), UB ≥ LB.
  CHECK(gap >= -1e-6);
  CHECK(ub >= lb - 1e-6);

  // (2) Non-trivial bounds — a stuck user-α bootstrap pin would leave LB ≈ 0.
  CHECK(std::isfinite(lb));
  CHECK(std::isfinite(ub));
  CHECK(lb > 1000.0);

  // (3) Convergence to the TRUE multi-stage optimum.  This proves the user α
  //     is a correct SDDP recourse column — the value function it carries
  //     reproduces the deterministic-equivalent optimum, not merely a
  //     self-consistent-but-wrong fixed point.  1e-3 relative is 10× the
  //     convergence_tol yet tight enough to catch any encoding error > ~$340.
  CHECK(lb == doctest::Approx(kAnalytic).epsilon(1e-3));
  // (4) [G4] The forward UB-estimator STRIPS the user-α value: at the unique
  //     optimum the cut design pins α = 0, so a correctly-stripped UB lands at
  //     kAnalytic.  A UB that double-counted the α value function (or failed to
  //     strip it) would land well above kAnalytic.  1e-3 here is even tighter
  //     than the 5e-3 the coverage audit asked for.
  CHECK(ub == doctest::Approx(kAnalytic).epsilon(1e-3));
}

// ─── 7. FutureCost.single_cut_equality — `=`+free-α vs slack-able `≥` ───────
//
// A lone boundary cut is, by default, installed as the EQUALITY
// `α + Σ wvᵣ·efinᵣ = FCF` with α freed below — the continuous PLEXOS-style
// terminal value (α pinned to `FCF − Σ wvᵣ·efinᵣ`, so a reservoir saved ABOVE
// the FCF break-even earns a NEGATIVE α future-cost credit).  Setting
// `single_cut_equality = false` keeps the slack-able `≥` Benders lower-bound
// form (α floored at the cut, NOT freed), so that credit is capped at the cut
// floor and never goes negative.
//
// The fixture uses a single cut `α ≥ FCF − wv·s_rsv1` (CSV coef = −wv) with a
// small `FCF` and a large `wv`, so the optimal policy (which conserves water
// to its emax corner) drives `FCF − wv·s` strictly negative.  Then:
//   * `single_cut_equality = true`  (default): α = FCF − wv·s  < 0  ⇒ the
//     converged LB picks up the negative future-cost credit (LOWER bound).
//   * `single_cut_equality = false`         : α floored, never freed ⇒ the
//     credit is capped, both bounds converge to the higher `≥` value.
// The two converge to STRICTLY different lower bounds — the cleanest single
// observable that the flag is wired and toggles the `≥`→`=`+free-α conversion.
namespace amplfcf_test  // NOLINT
{
namespace  // NOLINT
{
struct SddpBounds
{
  double lb {};
  double ub {};
};

/// Run a single-boundary-cut SDDP solve on the 3-phase hydro fixture with the
/// given `single_cut_equality` element setting (`nullopt` = unset = default).
[[nodiscard]] auto solve_single_cut_bounds(const std::string& cuts_file,
                                           std::optional<bool> single_cut_eq)
    -> SddpBounds
{
  auto planning = make_3phase_hydro_planning();
  planning.options.method = MethodType::sddp;
  FutureCost fc {
      .uid = Uid {1},
      .name = "bfcf",
      .cuts_file = OptName {cuts_file},
      .mode = std::optional<BoundaryCutsMode> {BoundaryCutsMode::combined},
  };
  if (single_cut_eq.has_value()) {
    fc.single_cut_equality = OptBool {*single_cut_eq};
  }
  planning.system.future_cost_array.push_back(fc);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 8;
  opts.convergence_tol = 1e-6;
  opts.enable_api = false;
  opts.boundary_cuts_file = cuts_file;
  opts.boundary_cuts_mode = BoundaryCutsMode::combined;
  opts.boundary_cuts_mean_shift = false;  // isolate the `=`/`≥` effect

  SDDPMethod sddp(planning_lp, opts);
  const auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());
  return {results->back().lower_bound, results->back().upper_bound};
}
}  // namespace
}  // namespace amplfcf_test

TEST_CASE(  // NOLINT
    "FutureCost.single_cut_equality — false keeps `≥`, default/true installs "
    "`=`")
{
  using namespace amplfcf_test;  // NOLINT(google-build-using-namespace)

  const auto cuts_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_single_cut_eq.csv")
          .string();
  {
    // α ≥ FCF − wv·s_rsv1 (CSV coef carries gᵢ where row is α ≥ rhs + gᵢ·s).
    // FCF = 1000 (small), wv = 90 (large) ⇒ at the conserved emax corner the
    // intercept goes strongly negative, so the `=`+free-α path lets α < 0.
    std::ofstream ofs(cuts_file);
    ofs << "iteration,scene,rhs,rsv1\n";
    ofs << "1,1,1000.0,-90.0\n";
  }

  const auto def = solve_single_cut_bounds(cuts_file, std::nullopt);  // default
  const auto eq = solve_single_cut_bounds(cuts_file, true);  // explicit true
  const auto ge = solve_single_cut_bounds(cuts_file, false);  // explicit false
  std::filesystem::remove(cuts_file);

  CAPTURE(def.lb);
  CAPTURE(def.ub);
  CAPTURE(eq.lb);
  CAPTURE(eq.ub);
  CAPTURE(ge.lb);
  CAPTURE(ge.ub);

  // (1) NO REGRESSION: unset default == explicit true, byte-for-byte.
  CHECK(def.lb == doctest::Approx(eq.lb));
  CHECK(def.ub == doctest::Approx(eq.ub));

  // (2) The flag TOGGLES the formulation: the `≥` (false) lower bound is
  //     STRICTLY ABOVE the `=`+free-α (true) lower bound — the equality lets
  //     α go negative (a future-cost credit), the `≥` floors it.
  CHECK(ge.lb > eq.lb + 1.0);

  // (3) The `≥` form converges (LB == UB) — α is floored at the cut, not freed,
  //     so the master and the simulated policy agree on the cost-to-go.
  CHECK(ge.ub == doctest::Approx(ge.lb).epsilon(1e-4));

  // (4) Both finite, positive.
  CHECK(std::isfinite(eq.lb));
  CHECK(std::isfinite(ge.lb));
  CHECK(eq.lb > 0.0);
}

// ─── 8. Faithful oracle — single `≥` boundary cut == user-authored `≥` cut ──
//
// With `single_cut_equality = false` the single-cut boundary path keeps the
// SAME `≥` formulation the user authors with a `state`/`link` α
// `DecisionVariable` + a terminal `≥` `UserConstraint` (test 6 / "2c").  The
// two are therefore the SAME formulation, so they must converge to the SAME
// LB/UB.  This is the direct cross-run faithful oracle (the test-6 analytic
// optimum stays the independent third check).
//
// Boundary cut `α + W·efin ≥ R` ⇔ `α ≥ R − W·efin` ⇒ CSV coef = −W, rhs = R,
// matching the user cut `user_alpha + W·reservoir('rsv1').efin ≥ R`.
TEST_CASE(  // NOLINT
    "FutureCost.single_cut_equality=false — single boundary `≥` cut is a "
    "faithful oracle for the user-authored `≥` FCF (2c)")
{
  using namespace amplfcf_test;  // NOLINT(google-build-using-namespace)

  constexpr double kW = 90.0;  // $/dam³ — same as the 2c user cut
  constexpr double kR = 45000.0;  // = W·emax — same as the 2c user cut
  constexpr double kAnalytic = 338850.0;  // 2c hand-derived optimum
  constexpr Uid kUaUid {4244};

  // Common SDDP options for both runs (mirror the 2c test exactly).
  const auto make_opts = []
  {
    SDDPOptions o;
    o.max_iterations = 20;
    o.convergence_tol = 1e-4;
    o.convergence_mode = ConvergenceMode::gap_only;
    o.stationary_tol = 0.0;
    o.cut_sharing = CutSharingMode::none;
    o.aperture_chunk_size = -1;
    o.enable_api = false;
    return o;
  };

  // ── (A) boundary-cut side: single `≥` cut, single_cut_equality=false ──
  double bdr_lb = 0.0;
  double bdr_ub = 0.0;
  {
    const auto cuts_file = (std::filesystem::temp_directory_path()
                            / "gtopt_test_faithful_oracle.csv")
                               .string();
    {
      std::ofstream ofs(cuts_file);
      ofs << "iteration,scene,rhs,rsv1\n";
      ofs << "1,1," << kR << "," << (-kW) << "\n";  // α ≥ kR − kW·efin
    }
    auto planning = make_3phase_hydro_planning();
    planning.options.method = MethodType::sddp;
    planning.system.future_cost_array.push_back(FutureCost {
        .uid = Uid {1},
        .name = "bfcf",
        .cuts_file = OptName {cuts_file},
        .single_cut_equality = OptBool {false},  // faithful `≥` form
        .mode = std::optional<BoundaryCutsMode> {BoundaryCutsMode::combined},
    });
    PlanningLP planning_lp(std::move(planning));
    auto opts = make_opts();
    opts.boundary_cuts_file = cuts_file;
    opts.boundary_cuts_mode = BoundaryCutsMode::combined;
    opts.boundary_cuts_mean_shift = false;
    SDDPMethod sddp(planning_lp, opts);
    const auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    bdr_lb = results->back().lower_bound;
    bdr_ub = results->back().upper_bound;
    std::filesystem::remove(cuts_file);
  }

  // ── (B) user-α side: the 2c formulation (terminal `≥` UserConstraint) ──
  double usr_lb = 0.0;
  double usr_ub = 0.0;
  {
    auto planning = make_3phase_hydro_planning();
    planning.options.method = MethodType::sddp;
    planning.system.decision_variable_array.push_back(DecisionVariable {
        .uid = kUaUid,
        .name = "user_alpha",
        .lower_bound = OptReal {-1.0e9},
        .cost = OptReal {1.0},
        .cost_type = OptName {"raw"},
        .scope = OptName {"global"},
        .state = OptBool {true},
        .link = OptBool {true},
    });
    planning.system.user_constraint_array.push_back(UserConstraint {
        .uid = Uid {4245},
        .name = "fcf_cut",
        .expression = Name {std::format(
            "decision_variable('user_alpha').value + "
            "{}*reservoir('rsv1').efin >= {}, for(stage in {{3}})",
            kW,
            kR)},
        .constraint_type = OptName {"raw"},
        .scope = OptName {"global"},
    });
    planning.system.future_cost_array.push_back(FutureCost {
        .uid = Uid {1},
        .name = "ufcf",
        .use_user_alpha = OptBool {true},
        .user_alpha_uid = OptUid {kUaUid},
    });
    PlanningLP planning_lp(std::move(planning));
    SDDPMethod sddp(planning_lp, make_opts());
    const auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    usr_lb = results->back().lower_bound;
    usr_ub = results->back().upper_bound;
  }

  CAPTURE(bdr_lb);
  CAPTURE(bdr_ub);
  CAPTURE(usr_lb);
  CAPTURE(usr_ub);

  // The `≥` boundary cut and the user-authored `≥` cut are the SAME
  // formulation ⇒ their converged bounds coincide (faithful oracle) and both
  // hit the independently hand-derived 2c analytic optimum.
  CHECK(bdr_lb == doctest::Approx(usr_lb).epsilon(1e-3));
  CHECK(bdr_ub == doctest::Approx(usr_ub).epsilon(1e-3));
  CHECK(bdr_lb == doctest::Approx(kAnalytic).epsilon(1e-3));
  CHECK(usr_lb == doctest::Approx(kAnalytic).epsilon(1e-3));
}

// ─── 9. active_user_alpha_uid helper — the DRY'd user-α guard ───────────────
//
// `active_user_alpha_uid(PlanningLP&)` is the single source of truth for the
// "user α is the FCF recourse column" guard used across the SDDP backward /
// forward / alpha passes (it replaced the 3-condition guard duplicated at 5
// sites).  It returns the user α uid ONLY when an ACTIVE FutureCost element has
// BOTH `use_user_alpha = true` AND `user_alpha_uid` set; nullopt otherwise.
// This pins each precondition independently so a future change to any one of
// them is caught here, not only in the (slow) end-to-end SDDP tests.
TEST_CASE("active_user_alpha_uid — guard preconditions")  // NOLINT
{
  using namespace amplfcf_test;  // NOLINT(google-build-using-namespace)

  SUBCASE("nullopt when there is no FutureCost element")
  {
    PlanningLP planning_lp(make_monolithic_planning());  // no future_cost_array
    CHECK_FALSE(active_user_alpha_uid(planning_lp).has_value());
  }

  SUBCASE("nullopt when use_user_alpha is unset (legacy boundary-cut FCF)")
  {
    auto planning = make_monolithic_planning();
    planning.system.future_cost_array.push_back(FutureCost {
        .uid = Uid {1},
        .name = "bfcf",
        .cuts_file = OptName {"boundary_cuts.csv"},
        // use_user_alpha unset, user_alpha_uid set → still nullopt (the flag
        // gates the helper, not the uid presence).
        .user_alpha_uid = OptUid {kUserAlphaUid},
    });
    PlanningLP planning_lp(std::move(planning));
    CHECK_FALSE(active_user_alpha_uid(planning_lp).has_value());
  }

  SUBCASE("nullopt when use_user_alpha is explicit false")
  {
    auto planning = make_monolithic_planning();
    planning.system.future_cost_array.push_back(FutureCost {
        .uid = Uid {1},
        .name = "bfcf",
        .use_user_alpha = OptBool {false},
        .user_alpha_uid = OptUid {kUserAlphaUid},
    });
    PlanningLP planning_lp(std::move(planning));
    CHECK_FALSE(active_user_alpha_uid(planning_lp).has_value());
  }

  SUBCASE("nullopt when use_user_alpha=true but user_alpha_uid unset")
  {
    auto planning = make_monolithic_planning();
    planning.system.future_cost_array.push_back(FutureCost {
        .uid = Uid {1},
        .name = "ufcf",
        .use_user_alpha = OptBool {true},
        // user_alpha_uid deliberately omitted.
    });
    PlanningLP planning_lp(std::move(planning));
    CHECK_FALSE(active_user_alpha_uid(planning_lp).has_value());
  }

  SUBCASE("returns the uid when both use_user_alpha and user_alpha_uid set")
  {
    PlanningLP planning_lp(make_user_alpha_planning(1000.0));
    const auto ua = active_user_alpha_uid(planning_lp);
    REQUIRE(ua.has_value());
    CHECK(*ua == kUserAlphaUid);
  }

  SUBCASE("nullopt when the FutureCost element is explicitly deactivated")
  {
    // An explicit scalar `active = false` deactivates the element, so even with
    // both flags set the helper returns nullopt (no ACTIVE FutureCost).
    auto planning = make_monolithic_planning();
    planning.system.future_cost_array.push_back(FutureCost {
        .uid = Uid {1},
        .name = "ufcf",
        .active = Active {IntBool {False}},
        .use_user_alpha = OptBool {true},
        .user_alpha_uid = OptUid {kUserAlphaUid},
    });
    PlanningLP planning_lp(std::move(planning));
    CHECK_FALSE(active_user_alpha_uid(planning_lp).has_value());
  }
}

// ─── 10. use_user_alpha without user_alpha_uid — hard error ─────────────────
//
// The init-time guard requires `user_alpha_uid` whenever `use_user_alpha` is
// set: without it there is no column to price the FCF onto.  Sibling of test 3
// (mutual exclusion) / test 4 (cost==0) — the other two `std::unexpected`
// branches of the same validation block.
TEST_CASE(  // NOLINT
    "AmplFutureCost — use_user_alpha without user_alpha_uid is std::unexpected")
{
  using namespace amplfcf_test;  // NOLINT(google-build-using-namespace)

  auto planning = make_2phase_linear_planning();
  planning.options.method = MethodType::sddp;
  planning.system.future_cost_array.push_back(FutureCost {
      .uid = Uid {1},
      .name = "ufcf",
      .use_user_alpha = OptBool {true},
      // user_alpha_uid deliberately omitted → REQUIRED-field error.
  });
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 1;
  opts.enable_api = false;

  SDDPMethod sddp(planning_lp, opts);
  const auto results = sddp.solve();
  CHECK_FALSE(results.has_value());
  if (!results.has_value()) {
    CHECK(results.error().code == ErrorCode::InvalidInput);
  }
}

// ─── 11. user_alpha_uid that matches no DecisionVariable — hard error ───────
//
// The init-time guard reads the user α DecisionVariable's cost from the System
// input data; a uid that resolves to no DecisionVariable is a hard error (the
// "does not match any DecisionVariable" branch — distinct from the cost==0
// branch, which fires only AFTER the DV is found).
TEST_CASE(  // NOLINT
    "AmplFutureCost — user_alpha_uid matching no DecisionVariable is "
    "std::unexpected")
{
  using namespace amplfcf_test;  // NOLINT(google-build-using-namespace)

  auto planning = make_2phase_linear_planning();
  planning.options.method = MethodType::sddp;
  // NO DecisionVariable with this uid is added → dangling reference.
  planning.system.future_cost_array.push_back(FutureCost {
      .uid = Uid {1},
      .name = "ufcf",
      .use_user_alpha = OptBool {true},
      .user_alpha_uid = OptUid {Uid {999999}},
  });
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 1;
  opts.enable_api = false;

  SDDPMethod sddp(planning_lp, opts);
  const auto results = sddp.solve();
  CHECK_FALSE(results.has_value());
  if (!results.has_value()) {
    CHECK(results.error().code == ErrorCode::InvalidInput);
  }
}

// ─── 12. [G1] use_user_alpha + cut_sharing=multicut — hard error ────────────
//
// The user α is a SINGLE column priced `cost`, whereas `CutSharingMode::
// multicut` expects N dedicated `varphi_s` α columns (one per source scene) to
// route per-scenario cuts onto.  The init-time guard rejects the combination
// (multicut support over a user α is deferred); the error message must name
// `multicut` so the operator knows which option to drop.
TEST_CASE(  // NOLINT
    "AmplFutureCost — use_user_alpha + cut_sharing=multicut is std::unexpected")
{
  using namespace amplfcf_test;  // NOLINT(google-build-using-namespace)

  PlanningLP planning_lp(make_user_alpha_sddp_planning(1000.0, 1.0));
  SDDPOptions opts;
  opts.max_iterations = 1;
  opts.enable_api = false;
  opts.cut_sharing = CutSharingMode::multicut;  // incompatible with user α

  SDDPMethod sddp(planning_lp, opts);
  const auto results = sddp.solve();
  CHECK_FALSE(results.has_value());
  if (!results.has_value()) {
    CHECK(results.error().code == ErrorCode::InvalidInput);
    // The message must name the offending option so the diagnostic is
    // actionable.
    CHECK(results.error().message.find("multicut") != std::string::npos);
  }
}

// ─── 13. [G5] user α is NOT elastically zeroed (skipped from outgoing_links) ─
//
// The active user α is excluded from `outgoing_links` in the backward pass
// (`collect_outgoing_links`): the elastic filter relaxes each link's dependent
// column with penalised slacks, and were the user α treated as a forward state
// it would receive those slacks → its cost-to-go value function would be zeroed
// → the converged LB would free-fall to ≈ 0.  `outgoing_links` is private, so
// this asserts the BEHAVIORAL proxy: a multi-iteration run on the analytic 2c
// fixture converges to a non-trivial, self-consistent (gap ≥ −fp-noise) LB.  A
// non-trivial LB proves the user-α skip is in place (a stuck/zeroed α would
// leave LB ≈ 0).  This is the focused, value-agnostic regression guard
// complementing test 6's exact-optimum convergence assertion.
TEST_CASE(  // NOLINT
    "AmplFutureCost — user α is not elastically zeroed (non-trivial LB)")
{
  using namespace amplfcf_test;  // NOLINT(google-build-using-namespace)

  constexpr double kW = 90.0;
  constexpr double kR = 45000.0;
  constexpr Uid kUaUid {4244};

  auto planning = make_3phase_hydro_planning();
  planning.options.method = MethodType::sddp;
  planning.system.decision_variable_array.push_back(DecisionVariable {
      .uid = kUaUid,
      .name = "user_alpha",
      .lower_bound = OptReal {-1.0e9},
      .cost = OptReal {1.0},
      .cost_type = OptName {"raw"},
      .scope = OptName {"global"},
      .state = OptBool {true},
      .link = OptBool {true},
  });
  planning.system.user_constraint_array.push_back(UserConstraint {
      .uid = Uid {4245},
      .name = "fcf_cut",
      .expression = Name {std::format(
          "decision_variable('user_alpha').value + {}*reservoir('rsv1').efin "
          ">= {}, for(stage in {{3}})",
          kW,
          kR)},
      .constraint_type = OptName {"raw"},
      .scope = OptName {"global"},
  });
  planning.system.future_cost_array.push_back(FutureCost {
      .uid = Uid {1},
      .name = "ufcf",
      .use_user_alpha = OptBool {true},
      .user_alpha_uid = OptUid {kUaUid},
  });

  PlanningLP planning_lp(std::move(planning));
  SDDPOptions opts;
  opts.max_iterations = 4;  // ≥ 2 iterations exercises the cut/forward loop
  opts.convergence_tol = 1e-4;
  opts.convergence_mode = ConvergenceMode::gap_only;
  opts.stationary_tol = 0.0;
  opts.cut_sharing = CutSharingMode::none;
  opts.aperture_chunk_size = -1;
  opts.enable_api = false;

  SDDPMethod sddp(planning_lp, opts);
  const auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE(results->size() >= 2);  // ran ≥ 2 iterations

  const double lb = results->back().lower_bound;
  const double gap = results->back().gap;
  CAPTURE(lb);
  CAPTURE(gap);

  // No LB overshoot (self-consistent) AND a non-trivial LB — proof the user α
  // survived the elastic-filter outgoing-link skip rather than being zeroed.
  CHECK(gap >= -1e-6);
  CHECK(lb > 100.0);
}

// ─── 14. [G6] DecisionVariable.obj_constant on a global-scope column ─────────
//
// `obj_constant` is the α-rebase restitution: the LP adds `cost · obj_constant`
// back via `LinearProblem::add_obj_constant`, which `get_obj_value()` folds
// into the reported objective.  On a GLOBAL-scope column the planning-pass
// builder (`build_cell_col`) applies it once per cell.  A global DV pinned to
// `value = 0` by a `≥ 0` cut contributes only the constant, so the solved
// objective equals the baseline plus `obj_constant`.
TEST_CASE(  // NOLINT
    "DecisionVariable — obj_constant on a global-scope column adds the "
    "constant")
{
  using namespace amplfcf_test;  // NOLINT(google-build-using-namespace)

  const double baseline =
      solve_monolithic_obj(make_monolithic_planning(), /*boundary=*/"");
  REQUIRE(std::isfinite(baseline));

  auto planning = make_monolithic_planning();
  // Global DV: cost 1.0 raw + obj_constant 500.0.  obj_constant set ⇒ the
  // column defaults free below; the cut below pins value = 0, so the column's
  // objective contribution is purely `cost · obj_constant = 1.0 · 500.0`.
  planning.system.decision_variable_array.push_back(DecisionVariable {
      .uid = Uid {7001},
      .name = "dv_oc",
      .cost = OptReal {1.0},
      .cost_type = OptName {"raw"},
      .scope = OptName {"global"},
      .obj_constant = OptReal {500.0},
  });
  planning.system.user_constraint_array.push_back(UserConstraint {
      .uid = Uid {7002},
      .name = "force_zero",
      .expression = Name {"decision_variable('dv_oc').value >= 0"},
      .constraint_type = OptName {"raw"},
      .scope = OptName {"global"},
  });

  const double obj = solve_monolithic_obj(std::move(planning), /*boundary=*/"");
  REQUIRE(std::isfinite(obj));
  CAPTURE(baseline);
  CAPTURE(obj);
  CHECK(obj == doctest::Approx(baseline + 500.0).epsilon(1e-6));
}

// ─── 15. [G8] DecisionVariable cost_type="power" on a global-scope column ────
//
// On a global cell column `cost_type="power"` has no single block, so
// `build_cell_col` folds the cost with the energy factor (probability ×
// discount, NO duration).  A SINGLE scenario's `probability_factor` is always
// renormalized to 1.0 (sum-to-one), so to expose the fold we instead use a
// non-unit per-stage `discount_factor = 0.5`: then `energy_factor = prob(1.0) ×
// discount(0.5) = 0.5`, and a `cost = 10.0` power column forced to `value >= 1`
// contributes `10.0 · 0.5 = 5.0`.  A RAW column (which ignores the factor)
// contributes the full `10.0` — the power-vs-raw delta proves the fold.
TEST_CASE(  // NOLINT
    "DecisionVariable — cost_type=power on a global column folds prob×discount")
{
  using namespace amplfcf_test;  // NOLINT(google-build-using-namespace)

  // A planning whose single (terminal) stage carries discount_factor = 0.5.
  const auto make_discounted = []
  {
    auto p = make_monolithic_planning();
    p.simulation.stage_array.front().discount_factor = OptReal {0.5};
    return p;
  };

  const double baseline =
      solve_monolithic_obj(make_discounted(), /*boundary=*/"");
  REQUIRE(std::isfinite(baseline));

  // power column: cost 10.0, cost_type "power" → col_cost = 10.0 × 0.5 = 5.0.
  auto planning = make_discounted();
  planning.system.decision_variable_array.push_back(DecisionVariable {
      .uid = Uid {8001},
      .name = "dv_pow",
      .cost = OptReal {10.0},
      .cost_type = OptName {"power"},
      .scope = OptName {"global"},
  });
  planning.system.user_constraint_array.push_back(UserConstraint {
      .uid = Uid {8002},
      .name = "force_one",
      .expression = Name {"decision_variable('dv_pow').value >= 1.0"},
      .constraint_type = OptName {"raw"},
      .scope = OptName {"global"},
  });
  const double obj = solve_monolithic_obj(std::move(planning), /*boundary=*/"");
  REQUIRE(std::isfinite(obj));

  // raw column (same cost, same RHS): col_cost = 10.0 verbatim, NO discount
  // fold — the discriminating control proving the power fold actually applied.
  auto raw_planning = make_discounted();
  raw_planning.system.decision_variable_array.push_back(DecisionVariable {
      .uid = Uid {8001},
      .name = "dv_raw",
      .cost = OptReal {10.0},
      .cost_type = OptName {"raw"},
      .scope = OptName {"global"},
  });
  raw_planning.system.user_constraint_array.push_back(UserConstraint {
      .uid = Uid {8002},
      .name = "force_one",
      .expression = Name {"decision_variable('dv_raw').value >= 1.0"},
      .constraint_type = OptName {"raw"},
      .scope = OptName {"global"},
  });
  const double raw_obj =
      solve_monolithic_obj(std::move(raw_planning), /*boundary=*/"");
  REQUIRE(std::isfinite(raw_obj));

  CAPTURE(baseline);
  CAPTURE(obj);
  CAPTURE(raw_obj);

  // power column adds cost × discount(0.5) = 5.0.
  CHECK(obj == doctest::Approx(baseline + 5.0).epsilon(1e-6));
  // raw column adds the full cost = 10.0 (no fold) — the fold is real.
  CHECK(raw_obj == doctest::Approx(baseline + 10.0).epsilon(1e-6));
}
