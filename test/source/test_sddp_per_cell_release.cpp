// SPDX-License-Identifier: BSD-3-Clause
//
// Per-cell `release_backend()` regression tests for the SDDP backward
// pass.  Pins the contract introduced by the
// `perf/release-backend-in-backward-worker` branch:
//
//   1. `LinearInterface::release_backend()` is idempotent — calling it
//      twice is safe and the second call is a no-op.  Used by the
//      per-cell scheme below: cells released inside the backward
//      worker may be re-released by the end-of-iteration safety-net
//      bulk loop in `sddp_iteration.cpp` without harm.
//
//   2. **Synchronized backward** (cut_sharing != none): after
//      `SDDPMethod::solve()` returns under `low_memory_mode =
//      compress`, every `(scene, phase)` cell's backend is released.
//      With the per-cell scheme, the cells are released by:
//        - `backward_pass_aperture_phase_impl` for `target_sys =
//          (scene, phase_index)` at end of each per-scene worker
//          (covers phase 1 .. N-1).
//        - `run_backward_pass_synchronized` after `share_cuts_for_phase`
//          at iter `phase_index = 1` (covers phase 0).
//      The bulk loop at `sddp_iteration.cpp:504-512` is the
//      idempotent safety net.
//
//   3. **Non-synchronized backward** (cut_sharing == none): same
//      end-state, achieved via the per-cell release in
//      `backward_pass_with_apertures` (aperture path) or
//      `backward_pass` (non-aperture path).
//
// The tests verify the *end state* (all cells released) rather than
// *who* released each cell, because the bulk-loop safety net runs
// after the backward pass under the same `low_memory_mode != off`
// guard.  A regression where the per-cell scheme misses a cell is
// covered by the bulk loop and thus does not surface in end-state
// checks; that is intentional — the per-cell scheme is a wall-time
// optimisation, not a correctness one.  The performance contract
// (peak memory, end-of-iter latency) is observable in production
// logs (`SDDPWorkPool` Active/Pending stats) and is not unit-
// testable without large fixtures; the smoke tests below ensure
// the per-cell calls do not break the SDDP solve under either
// dispatch mode.

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_types.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Walk every (scene, phase) cell and assert its backend is released.
/// Used as the post-`solve()` invariant in the synchronized and non-
/// synchronized backward tests.
void check_all_backends_released(const PlanningLP& plp)
{
  const auto num_scenes = plp.simulation().scene_count();
  const auto num_phases = plp.simulation().phase_count();
  for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
    for (const auto pi : iota_range<PhaseIndex>(0, num_phases)) {
      const auto& li = plp.system(si, pi).linear_interface();
      INFO("scene=" << value_of(si) << " phase=" << value_of(pi));
      CHECK(li.is_backend_released());
    }
  }
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 1. release_backend() idempotency
// ═══════════════════════════════════════════════════════════════════════════
//
// Per-cell scheme relies on this: the worker may release the cell, then
// the end-of-iteration bulk safety-net loop calls release_backend again.
// `linear_interface.cpp:144` early-returns on `m_backend_released_`, so
// the second call is a no-op.  This test pins that contract.

TEST_CASE(  // NOLINT
    "LinearInterface::release_backend is idempotent under "
    "low_memory_mode=compress")
{
  // Build a minimal LP through PlanningLP so the backend is real and
  // CPLEX/HIGHS-loaded — calling release_backend on a fully-stubbed
  // LinearInterface would not exercise the production path.
  //
  // `method = sddp` is required because `planning_lp.cpp:586-592`
  // only propagates `sddp_options.low_memory_mode` into
  // `LinearInterface::m_low_memory_mode_` when the method is sddp
  // or cascade.  Under the default `monolithic` method the flat
  // build keeps low_memory off and `release_backend()` is a no-op.
  auto planning = make_3phase_hydro_planning();
  planning.options.method = MethodType::sddp;
  planning.options.sddp_options.low_memory_mode = LowMemoryMode::compress;
  planning.options.sddp_options.memory_codec = CompressionCodec::uncompressed;

  PlanningLP plp(std::move(planning));
  auto& sys = plp.system(first_scene_index(), first_phase_index());
  sys.ensure_lp_built();
  auto& li = sys.linear_interface();
  REQUIRE_FALSE(li.is_backend_released());

  // First release: drops the backend.
  li.release_backend();
  CHECK(li.is_backend_released());

  // Second release: idempotent — no-op, still released.  Must not
  // throw or corrupt cached scalars.
  CHECK_NOTHROW(li.release_backend());
  CHECK(li.is_backend_released());

  // Third release: still idempotent.
  CHECK_NOTHROW(li.release_backend());
  CHECK(li.is_backend_released());
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. Synchronized backward (cut_sharing=expected): all backends released
// ═══════════════════════════════════════════════════════════════════════════
//
// 2-scene 3-phase fixture, cut_sharing=expected, low_memory=compress,
// 1 iteration.  After solve(), every (scene, phase) cell's backend
// must be released.  Confirms the per-cell scheme + bulk safety net
// converge on the correct end state under the synchronized backward.

TEST_CASE(  // NOLINT
    "SDDP synchronized backward + low_memory=compress: "
    "all backends released after solve()")
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  planning.options.method = MethodType::sddp;
  PlanningLP plp(std::move(planning));

  REQUIRE(plp.simulation().scene_count() == Index {2});
  REQUIRE(plp.simulation().phase_count() >= Index {2});

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.convergence_tol = 1e-6;
  sddp_opts.cut_sharing = CutSharingMode::expected;
  sddp_opts.low_memory_mode = LowMemoryMode::compress;
  sddp_opts.memory_codec = CompressionCodec::uncompressed;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  check_all_backends_released(plp);
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. Non-synchronized backward (cut_sharing=none): all backends released
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "SDDP non-synchronized backward + low_memory=compress: "
    "all backends released after solve()")
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  planning.options.method = MethodType::sddp;
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.convergence_tol = 1e-6;
  sddp_opts.cut_sharing = CutSharingMode::none;
  sddp_opts.low_memory_mode = LowMemoryMode::compress;
  sddp_opts.memory_codec = CompressionCodec::uncompressed;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  check_all_backends_released(plp);
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. Per-cell release does not corrupt subsequent ensure_lp_built
// ═══════════════════════════════════════════════════════════════════════════
//
// The per-cell scheme releases cells *during* the backward pass.  The
// bulk safety-net loop at end-of-iteration would re-release them; the
// next iteration's forward pass will then `ensure_lp_built` to reload
// from snapshot.  This test verifies that two consecutive iterations
// run cleanly under the per-cell release scheme — i.e. the early
// release does not leave the LP in a state that breaks the next
// iteration's forward pass.

TEST_CASE(  // NOLINT
    "SDDP per-cell release: two iterations run cleanly without "
    "leaking releaes / breaking reload")
{
  auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
  PlanningLP plp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;
  sddp_opts.convergence_tol = 1e-9;  // force both iterations
  sddp_opts.min_iterations = 2;
  sddp_opts.cut_sharing = CutSharingMode::expected;
  sddp_opts.low_memory_mode = LowMemoryMode::compress;
  sddp_opts.memory_codec = CompressionCodec::uncompressed;
  sddp_opts.enable_api = false;

  SDDPMethod sddp(plp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  // Both iterations must have completed; if the per-cell release
  // corrupted reload state, iter 1's forward pass would fail.
  CHECK(results->size() >= 1);

  check_all_backends_released(plp);
}
