// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_alpha_relax.cpp
 * @brief     Tests for the α (future-cost) lower-bound relaxation on first
 *            Benders cut installation.
 * @date      2026-04-21
 *
 * The SDDP solver creates α eagerly with `lowb = sddp_alpha_bootstrap_min`
 * (0.0 by default) so the iter-0 cold-start LP is bounded.  Once a Benders
 * cut lands on α, the cut itself bounds α from below — the column bound
 * becomes redundant and, if kept at 0, artificially clips legitimately-
 * negative future-cost values.
 *
 * `SDDPMethod::relax_alpha_lower_bound()` releases the bootstrap at each
 * cut-install site.  Two tests:
 *
 *   M1: after relax, `get_col_low_raw()[alpha_col]` returns `-DblMax`.
 *   M2: after relax + `release_backend` + `ensure_backend` under
 *       `LowMemoryMode::compress`, the reloaded α still has
 *       `lowb = -DblMax` (not reverted to 0).  Guards the fix where
 *       `relax_alpha_lower_bound` also updates the matching
 *       `m_dynamic_cols_` entry so `apply_post_load_replay` sees the
 *       freed bound.
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_types.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Threshold below which a lower bound is considered "effectively
/// −∞".  Solver backends (CPLEX, HiGHS, …) normalise gtopt's
/// `LinearProblem::DblMax` sentinel (`1.8e308`) to their own
/// infinity representation on read-back — CPLEX returns `-1e20`,
/// HiGHS `-1e30`, CBC `-1e30`.  Any raw value below `-1e15` is
/// comfortably past any realistic scale_alpha * physical_cost
/// product and confirms α is unbounded below.
constexpr double kEffectivelyMinusInf = -1e15;

/// Read α's current raw LP lower bound at (scene, phase).  Returns
/// `std::nullopt` if α is not registered (e.g. the last phase, or
/// before `initialize_alpha_variables`).
auto alpha_lowb_raw(const PlanningLP& plp,
                    SceneIndex scene_index,
                    PhaseIndex phase_index) -> std::optional<double>
{
  const auto* svar =
      find_alpha_state_var(plp.simulation(), scene_index, phase_index);
  if (svar == nullptr) {
    return std::nullopt;
  }
  const auto& li = plp.system(scene_index, phase_index).linear_interface();
  return li.get_col_low_raw()[svar->col()];
}

// ═══════════════════════════════════════════════════════════════════════════
// M1 — basic: relax on live backend updates the raw column lower bound
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDPMethod::relax_alpha_lower_bound — live backend: 0.0 → −DblMax")
{
  auto planning = make_nphase_simple_hydro_planning(3);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;  // skip training — we only need the init
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);
  // `ensure_initialized()` runs the bootstrap path that creates α on
  // every non-last phase with `lowb = sddp_alpha_bootstrap_min`.
  REQUIRE(sddp.ensure_initialized().has_value());

  const auto scene = first_scene_index();
  constexpr PhaseIndex phase {0};  // first non-last phase has α

  SUBCASE("pre-relax baseline matches sddp_alpha_bootstrap_min (= 0.0)")
  {
    const auto lowb = alpha_lowb_raw(plp, scene, phase);
    REQUIRE(lowb.has_value());
    CHECK(*lowb == doctest::Approx(sddp_alpha_bootstrap_min));
  }

  SUBCASE("after relax, α's raw lowb is −DblMax")
  {
    sddp.relax_alpha_lower_bound(scene, phase);

    const auto lowb = alpha_lowb_raw(plp, scene, phase);
    REQUIRE(lowb.has_value());
    CHECK(*lowb < kEffectivelyMinusInf);
  }

  SUBCASE("relax is idempotent")
  {
    sddp.relax_alpha_lower_bound(scene, phase);
    sddp.relax_alpha_lower_bound(scene, phase);  // second call is a no-op
    sddp.relax_alpha_lower_bound(scene, phase);

    const auto lowb = alpha_lowb_raw(plp, scene, phase);
    REQUIRE(lowb.has_value());
    CHECK(*lowb < kEffectivelyMinusInf);
  }

  SUBCASE("relax on the last phase is a silent no-op (no α there)")
  {
    const auto last = plp.simulation().last_phase_index();
    // Smoke test: must not throw and must not touch anything.
    sddp.relax_alpha_lower_bound(scene, last);

    const auto lowb = alpha_lowb_raw(plp, scene, last);
    // α is not registered at the last phase — `alpha_lowb_raw`
    // returns nullopt and this subcase passes trivially.
    CHECK_FALSE(lowb.has_value());
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// M2 — snapshot-persistent: release_backend + ensure_backend under
// LowMemoryMode::compress preserves the freed bound
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDPMethod::relax_alpha_lower_bound — survives low_memory compress "
    "release+reload")
{
  auto planning = make_nphase_simple_hydro_planning(3);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;
  sddp_opts.low_memory_mode = LowMemoryMode::compress;
  sddp_opts.memory_codec = CompressionCodec::uncompressed;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  const auto scene = first_scene_index();
  constexpr PhaseIndex phase {0};

  // 1. Relax on the live backend (M1 mechanic).
  sddp.relax_alpha_lower_bound(scene, phase);
  {
    const auto lowb = alpha_lowb_raw(plp, scene, phase);
    REQUIRE(lowb.has_value());
    REQUIRE(*lowb < kEffectivelyMinusInf);
  }

  // 2. Release the backend: compresses the snapshot, drops the live
  //    solver object.  The snapshot captured the ORIGINAL column
  //    bound (sddp_alpha_bootstrap_min), so only the `m_dynamic_cols_`
  //    mirror — updated by M2 — preserves the freed bound.
  auto& sys = plp.system(scene, phase);
  sys.release_backend();

  // 3. Reload.  `ensure_lp_built` reconstructs the backend from the
  //    snapshot and replays `m_dynamic_cols_` via
  //    `apply_post_load_replay`.  If M2 is wired correctly the replayed
  //    α carries `lowb = -DblMax`; otherwise it reverts to 0.
  sys.ensure_lp_built();

  const auto lowb = alpha_lowb_raw(plp, scene, phase);
  REQUIRE(lowb.has_value());
  CHECK(*lowb < kEffectivelyMinusInf);
}

// ═══════════════════════════════════════════════════════════════════════════
// LinearInterface::update_dynamic_col_lowb — unit-level
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "LinearInterface::update_dynamic_col_lowb finds matching entry by "
    "class_name + variable_name")
{
  auto planning = make_nphase_simple_hydro_planning(3);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;
  sddp_opts.low_memory_mode = LowMemoryMode::compress;
  sddp_opts.memory_codec = CompressionCodec::uncompressed;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  auto& li = plp.system(first_scene_index(), PhaseIndex {0}).linear_interface();

  SUBCASE("match by class + variable name updates lowb, returns true")
  {
    const auto updated = li.update_dynamic_col_lowb(
        sddp_alpha_class_name, sddp_alpha_col_name, -42.0);
    CHECK(updated);
  }

  SUBCASE("no matching entry returns false (bogus class_name)")
  {
    const auto updated = li.update_dynamic_col_lowb(
        "NotSddp", sddp_alpha_col_name, -LinearProblem::DblMax);
    CHECK_FALSE(updated);
  }

  SUBCASE("no matching entry returns false (bogus variable_name)")
  {
    const auto updated = li.update_dynamic_col_lowb(
        sddp_alpha_class_name, "not_alpha", -LinearProblem::DblMax);
    CHECK_FALSE(updated);
  }
}

}  // namespace
