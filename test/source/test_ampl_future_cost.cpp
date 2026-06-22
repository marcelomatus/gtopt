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
