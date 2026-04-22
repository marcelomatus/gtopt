// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_alpha_relax.cpp
 * @brief     Tests for α (future-cost) bootstrap pin release on first cut
 *            installation.
 * @date      2026-04-21
 *
 * The SDDP solver creates α eagerly on every phase (including the last)
 * with `lowb = uppb = sddp_alpha_bootstrap_min` (0.0) so the iter-0
 * cold-start LP is bounded AND does not carry a spurious degree of
 * freedom on α until a cut lands.  Once a Benders (or boundary) cut
 * lands on α, the cut itself bounds α — the column pin becomes
 * redundant and, if kept at `0 = 0`, artificially clips any legitimately-
 * nonzero future-cost value.
 *
 * `SDDPMethod::free_alpha()` / the free-function `gtopt::free_alpha`
 * releases BOTH bounds (`lowb ← -DblMax`, `uppb ← +DblMax`) at each
 * cut-install site.  Three coverage axes:
 *
 *   M1: after free, α's raw bounds are `-DblMax` / `+DblMax` on the
 *       live backend (both directions released).
 *   M2: after free + `release_backend` + `ensure_backend` under
 *       `LowMemoryMode::compress`, the reloaded α still carries the
 *       freed bounds (not reverted to 0/0).  Guards the
 *       `update_dynamic_col_bounds` mirror in `apply_post_load_replay`.
 *   L:  α is registered on the *last* phase too (new contract), so
 *       `free_alpha(…, last_phase)` is no longer a silent no-op — it
 *       actually flips both bounds, which is what the boundary-cut
 *       loader relies on.
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
constexpr double kEffectivelyPlusInf = 1e15;

/// Read α's current raw LP bounds at (scene, phase).  Returns
/// `std::nullopt` if α is not registered.
struct AlphaBounds
{
  double lowb {};
  double uppb {};
};

auto alpha_bounds_raw(const PlanningLP& plp,
                      SceneIndex scene_index,
                      PhaseIndex phase_index) -> std::optional<AlphaBounds>
{
  const auto* svar =
      find_alpha_state_var(plp.simulation(), scene_index, phase_index);
  if (svar == nullptr) {
    return std::nullopt;
  }
  const auto& li = plp.system(scene_index, phase_index).linear_interface();
  return AlphaBounds {
      .lowb = li.get_col_low_raw()[svar->col()],
      .uppb = li.get_col_upp_raw()[svar->col()],
  };
}

// ═══════════════════════════════════════════════════════════════════════════
// M1 — basic: free_alpha on the live backend releases both bounds
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("SDDPMethod::free_alpha — live backend: 0/+∞ → -DblMax/+DblMax")
{
  auto planning = make_nphase_simple_hydro_planning(3);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;  // skip training — only need init
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);
  // `ensure_initialized()` creates α on every phase with the
  // bidirectional bootstrap pin `lowb = uppb =
  // sddp_alpha_bootstrap_min (=0)`.  Pinning (rather than the
  // earlier `uppb=+∞` bootstrap) keeps the α column frozen at 0
  // inside the Chinneck Phase-1 elastic filter on the forward pass
  // — under Phase-1 all objective coefficients (including α's
  // `scale_alpha`) are zeroed on the clone, so without a pin α
  // would drift to whatever value simplex picks and contaminate
  // the captured trial / bcut Z.  Phase 0 is released in the last
  // step of every backward pass via
  // `install_aperture_backward_cut → free_alpha(src_phase_index=0)`.
  REQUIRE(sddp.ensure_initialized().has_value());

  const auto scene = first_scene_index();
  constexpr PhaseIndex phase {0};

  SUBCASE("pre-free baseline: α pinned at 0")
  {
    const auto bounds = alpha_bounds_raw(plp, scene, phase);
    REQUIRE(bounds.has_value());
    CHECK(bounds->lowb == doctest::Approx(sddp_alpha_bootstrap_min));
    CHECK(bounds->uppb == doctest::Approx(sddp_alpha_bootstrap_min));
  }

  SUBCASE("after free_alpha, both bounds are released")
  {
    sddp.free_alpha(scene, phase);

    const auto bounds = alpha_bounds_raw(plp, scene, phase);
    REQUIRE(bounds.has_value());
    CHECK(bounds->lowb < kEffectivelyMinusInf);
    CHECK(bounds->uppb > kEffectivelyPlusInf);
  }

  SUBCASE("free_alpha is idempotent")
  {
    sddp.free_alpha(scene, phase);
    sddp.free_alpha(scene, phase);
    sddp.free_alpha(scene, phase);

    const auto bounds = alpha_bounds_raw(plp, scene, phase);
    REQUIRE(bounds.has_value());
    CHECK(bounds->lowb < kEffectivelyMinusInf);
    CHECK(bounds->uppb > kEffectivelyPlusInf);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// L — α now exists on the last phase too, and free_alpha works on it
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("SDDPMethod::free_alpha — last phase has α and can be freed")
{
  auto planning = make_nphase_simple_hydro_planning(3);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 0;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  const auto scene = first_scene_index();
  const auto last = plp.simulation().last_phase_index();

  SUBCASE("α IS registered on the last phase pinned at 0")
  {
    const auto bounds = alpha_bounds_raw(plp, scene, last);
    REQUIRE(bounds.has_value());
    CHECK(bounds->lowb == doctest::Approx(sddp_alpha_bootstrap_min));
    CHECK(bounds->uppb == doctest::Approx(sddp_alpha_bootstrap_min));
  }

  SUBCASE("free_alpha on the last phase releases the pin")
  {
    sddp.free_alpha(scene, last);

    const auto bounds = alpha_bounds_raw(plp, scene, last);
    REQUIRE(bounds.has_value());
    CHECK(bounds->lowb < kEffectivelyMinusInf);
    CHECK(bounds->uppb > kEffectivelyPlusInf);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// M2 — snapshot-persistent: release+reload preserves the freed bounds
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(
    "SDDPMethod::free_alpha — survives low_memory compress release+reload")
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

  // 1. Free on the live backend.
  sddp.free_alpha(scene, phase);
  {
    const auto bounds = alpha_bounds_raw(plp, scene, phase);
    REQUIRE(bounds.has_value());
    REQUIRE(bounds->lowb < kEffectivelyMinusInf);
    REQUIRE(bounds->uppb > kEffectivelyPlusInf);
  }

  // 2. Release+reload.  The flat snapshot captured the ORIGINAL
  //    `lowb = uppb = 0` pin, so only the `m_dynamic_cols_` mirror
  //    — updated by `update_dynamic_col_bounds` inside free_alpha
  //    — preserves the freed bounds on replay.
  auto& sys = plp.system(scene, phase);
  sys.release_backend();
  sys.ensure_lp_built();

  const auto bounds = alpha_bounds_raw(plp, scene, phase);
  REQUIRE(bounds.has_value());
  CHECK(bounds->lowb < kEffectivelyMinusInf);
  CHECK(bounds->uppb > kEffectivelyPlusInf);
}

// ═══════════════════════════════════════════════════════════════════════════
// M2b — regression guard for bc257d1d's α bidirectional bootstrap pin:
//       the pin survives release+reload even when `free_alpha` was
//       never called.
// ═══════════════════════════════════════════════════════════════════════════
//
// Invariant under test (bc257d1d):
//   `register_alpha_variables` builds α's `SparseCol` with
//   `lowb = uppb = sddp_alpha_bootstrap_min (= 0)`.  Pre-bc257d1d the
//   column was built with `uppb = LinearProblem::DblMax`, which left α
//   as a free-above variable inside Chinneck's Phase-1 elastic clone
//   (where all objective coefficients — including α's scale_alpha —
//   are zeroed).  Without the bidirectional pin, simplex picks an
//   arbitrary basic value for α on the clone, and that value used to
//   leak into `StateVariable.col_sol()`.
//
// This test is symmetric to "survives low_memory compress
// release+reload" above, but deliberately WITHOUT calling free_alpha
// first.  The pin must survive a release+reload cycle on its own —
// both bounds still pinned at `sddp_alpha_bootstrap_min` after
// `release_backend` + `ensure_lp_built`.
TEST_CASE(
    "SDDPMethod::free_alpha — bootstrap pin survives low_memory compress "
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
  // `ensure_initialized` registers α on every phase (with the
  // bidirectional bootstrap pin lowb = uppb = sddp_alpha_bootstrap_min)
  // and, under low_memory mode, eagerly releases every per-cell
  // backend at the end.
  REQUIRE(sddp.ensure_initialized().has_value());

  const auto scene = first_scene_index();
  constexpr PhaseIndex phase {0};

  // Release+reload: no `free_alpha` in between.  The pin must be
  // preserved by the snapshot/replay path — neither the flat snapshot
  // nor `apply_post_load_replay`'s `m_dynamic_cols_` mirror may widen
  // uppb back to +∞ (the pre-bc257d1d default, which left α as a
  // free-above variable inside Chinneck's Phase-1 clone).
  auto& sys = plp.system(scene, phase);
  sys.release_backend();
  sys.ensure_lp_built();

  const auto bounds = alpha_bounds_raw(plp, scene, phase);
  REQUIRE(bounds.has_value());
  CHECK(bounds->lowb == doctest::Approx(sddp_alpha_bootstrap_min));
  CHECK(bounds->uppb == doctest::Approx(sddp_alpha_bootstrap_min));
}

// ═══════════════════════════════════════════════════════════════════════════
// LinearInterface::update_dynamic_col_bounds — unit-level
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(
    "LinearInterface::update_dynamic_col_bounds matches by class+variable")
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

  SUBCASE("match updates both lowb+uppb, returns true")
  {
    const auto updated = li.update_dynamic_col_bounds(
        sddp_alpha_class_name, sddp_alpha_col_name, -42.0, 42.0);
    CHECK(updated);
  }

  SUBCASE("bogus class_name → false")
  {
    const auto updated =
        li.update_dynamic_col_bounds("NotSddp", sddp_alpha_col_name, -1.0, 1.0);
    CHECK_FALSE(updated);
  }

  SUBCASE("bogus variable_name → false")
  {
    const auto updated = li.update_dynamic_col_bounds(
        sddp_alpha_class_name, "not_alpha", -1.0, 1.0);
    CHECK_FALSE(updated);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// register_alpha_variables — idempotency + coverage of all phases
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("register_alpha_variables is idempotent across repeated calls")
{
  auto planning = make_nphase_simple_hydro_planning(3);
  PlanningLP plp(std::move(planning));

  const auto& sim = plp.simulation();
  const auto num_phases = sim.phase_count();
  constexpr double kScaleAlpha = 1.0;
  const auto scene = first_scene_index();

  // Pre-condition: no α anywhere.
  for (const auto pi : iota_range<PhaseIndex>(0, num_phases)) {
    CHECK(find_alpha_state_var(sim, scene, pi) == nullptr);
  }

  // First call: α on every phase, pinned at 0.
  register_alpha_variables(plp, scene, kScaleAlpha);
  for (const auto pi : iota_range<PhaseIndex>(0, num_phases)) {
    const auto* svar = find_alpha_state_var(sim, scene, pi);
    REQUIRE(svar != nullptr);
  }

  // Record col indices so the idempotency check can assert that
  // subsequent calls don't allocate new columns.
  std::vector<ColIndex> first_call_cols;
  first_call_cols.reserve(static_cast<std::size_t>(num_phases));
  for (const auto pi : iota_range<PhaseIndex>(0, num_phases)) {
    first_call_cols.push_back(find_alpha_state_var(sim, scene, pi)->col());
  }

  // Second call: no-op — every (scene, phase) already has α, so the
  // skip-if-present guard short-circuits each iteration and no new
  // column is added.  Re-invocation from external callers (tests,
  // batch loaders) is therefore safe.
  register_alpha_variables(plp, scene, kScaleAlpha);
  std::size_t k = 0;
  for (const auto pi : iota_range<PhaseIndex>(0, num_phases)) {
    const auto* svar = find_alpha_state_var(sim, scene, pi);
    REQUIRE(svar != nullptr);
    CHECK(svar->col() == first_call_cols[k]);
    ++k;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// SimulationLP::scene_uid / phase_uid accessors
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("SimulationLP::scene_uid / phase_uid match scenes/phases subscript")
{
  auto planning = make_nphase_simple_hydro_planning(3);
  PlanningLP plp(std::move(planning));
  const auto& sim = plp.simulation();

  SUBCASE("scene_uid matches sim.scenes()[k].uid() for every k")
  {
    const auto num_scenes = sim.scene_count();
    for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
      CHECK(sim.uid_of(si) == sim.scenes()[static_cast<std::size_t>(si)].uid());
    }
  }

  SUBCASE("phase_uid matches sim.phases()[k].uid() for every k")
  {
    const auto num_phases = sim.phase_count();
    for (const auto pi : iota_range<PhaseIndex>(0, num_phases)) {
      CHECK(sim.uid_of(pi) == sim.phases()[static_cast<std::size_t>(pi)].uid());
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// SDDPMethod::solve() advances iteration_offset so re-entry is collision-free
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(
    "SDDPMethod::solve advances iteration_offset past the last executed "
    "iteration")
{
  auto planning = make_nphase_simple_hydro_planning(3);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-6;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);

  // Before solve, offset is 0 (cold start, no hot-start cuts).
  CHECK(sddp.iteration_offset() == IterationIndex {0});

  auto results1 = sddp.solve();
  REQUIRE(results1.has_value());
  REQUIRE_FALSE(results1->empty());

  // After solve, offset has advanced past the last executed
  // iteration so a second solve() lands its first new cut at a
  // disjoint `IterationContext` — otherwise the eager dup detector
  // in `LinearInterface::add_row` would throw on the repeated
  // `(class=Sddp, cons=scut, uid, scene, phase, iter=0, offset=0)`.
  const auto last_iter1 = results1->back().iteration_index;
  CHECK(sddp.iteration_offset() > last_iter1);
  CHECK(sddp.iteration_offset() == next(last_iter1));
}

TEST_CASE("SDDPMethod::solve can be re-entered on the same instance")
{
  auto planning = make_nphase_simple_hydro_planning(3);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;
  sddp_opts.convergence_tol = 1e-6;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);

  auto results1 = sddp.solve();
  REQUIRE(results1.has_value());
  const auto offset_after_1 = sddp.iteration_offset();

  // A second solve() on the same instance must not throw on dup
  // metadata.  Prior to the offset-advance fix, the second run
  // re-emitted `(scene, phase, iter=0, offset=0)` — identical to
  // the first run's first cut — which the metadata-based dup
  // detector in `LinearInterface::add_row` would reject.
  sddp.clear_stop();
  auto results2 = sddp.solve();
  REQUIRE(results2.has_value());

  // The second run's iterations start strictly past the first
  // run's highest iteration.
  if (!results2->empty()) {
    CHECK(results2->front().iteration_index >= offset_after_1);
  }
}

}  // namespace
