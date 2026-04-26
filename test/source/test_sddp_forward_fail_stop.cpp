// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_forward_fail_stop.cpp
 * @brief     Unit tests for SDDPOptions::forward_fail_stop control flow.
 * @date      2026-04-25
 *
 * The new (default) `forward_fail_stop = true` behaviour: when the
 * forward pass installs an elastic feasibility cut on phase p-1, the
 * scene's forward pass STOPS for the current iteration (returns Error).
 * The next iteration restarts from p1 with the freshly accumulated
 * cuts, available globally via the cut store.
 *
 * The legacy `forward_fail_stop = false` behaviour restores PLP-style
 * cascade backtracking: `phase_idx` is decremented after the fcut is
 * installed and p-1 is re-solved under the new cut, recursing until a
 * feasible phase is reached or `forward_max_attempts` is exhausted.
 *
 * The two modes are distinguished at the INFO log level by the
 * trailing tag on the "elastic → fcut on p{N}" line:
 *   * fail-stop:   "... fail-stop scene"
 *   * backtrack:   "... backtrack→p{N}"
 *
 * Fixture: `make_forced_infeasibility_planning` (3-phase 1-reservoir
 * with a forced waterway minimum that drains the reservoir during
 * phase 0 and renders phase 1 infeasible at the inherited trial value).
 * This fixture is the same one driving the elastic-filter / fcut audit
 * suites — known to install ≥ 1 fcut deterministically on a single
 * iteration regardless of mode.
 */

#include <algorithm>

#include <doctest/doctest.h>
#include <gtopt/error.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_types.hpp>

#include "log_capture.hpp"
#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Common SDDPOptions used by both Branch 1 / Branch 2 tests.  Pinned
/// to the same single_cut + forward_max_attempts knobs the existing
/// fcut_audit suite uses against the same fixture, so the cascade /
/// fail-stop entry path is reachable deterministically on a single
/// iteration.
auto make_fail_stop_options(bool fail_stop) -> SDDPOptions
{
  SDDPOptions opts;
  opts.max_iterations = 1;  // single iteration — enough to install the
                            // first fcut and exit; we measure scene-level
                            // forward-pass behaviour, not convergence.
  opts.elastic_filter_mode = ElasticFilterMode::single_cut;
  opts.multi_cut_threshold = -1;  // disable auto-switch to multi_cut
  opts.forward_max_attempts = 50;  // small cap — both paths exit fast
  opts.enable_api = false;
  opts.forward_fail_stop = fail_stop;
  return opts;
}

// ─── Branch 1 — forward_fail_stop = true: scene stops after one fcut ───────

TEST_CASE(  // NOLINT
    "SDDP sddp_forward_fail_stop=true — scene exits after first elastic fcut")
{
  auto planning = make_forced_infeasibility_planning();
  PlanningLP plp(std::move(planning));

  auto sddp_opts = make_fail_stop_options(/*fail_stop=*/true);

  gtopt::test::LogCapture logs;
  SDDPMethod sddp(plp, sddp_opts);
  [[maybe_unused]] auto results = sddp.solve();

  // The scene-level fail-stop tag must appear in the captured log
  // (emitted from sddp_forward_pass.cpp around the "fail-stop scene"
  // INFO line).  The legacy "backtrack→p" tag must NOT appear — that
  // is the contrapositive that proves we did not run the cascade.
  CHECK(logs.contains("fail-stop scene"));
  CHECK_FALSE(logs.contains("backtrack→p"));

  // Even though the scene's forward pass exited via Error, the
  // freshly-installed fcut is preserved in the cut store across the
  // iteration boundary.  Iter 1 (had it run) would see the new cut
  // through `add_cut_row`'s `m_active_cuts_` replay path.
  const auto cuts = sddp.stored_cuts();
  int n_feas = 0;
  for (const auto& c : cuts) {
    if (c.type == CutType::Feasibility) {
      ++n_feas;
    }
  }
  CAPTURE(n_feas);
  CAPTURE(cuts.size());
  CHECK(n_feas >= 1);

  // Each stored fcut carries valid metadata so the next iteration's
  // replay path can recover it.  In particular, every fcut must
  // have a non-empty coefficient list and a recognised row index.
  for (const auto& c : cuts) {
    if (c.type == CutType::Feasibility) {
      CHECK_FALSE(c.coefficients.empty());
      CHECK(c.row != RowIndex {unknown_index});
    }
  }
}

// ─── Branch 2 — forward_fail_stop = false: legacy backtracking cascade ─────

TEST_CASE(  // NOLINT
    "SDDP sddp_forward_fail_stop=false — legacy cascade re-enters phases")
{
  auto planning = make_forced_infeasibility_planning();
  PlanningLP plp(std::move(planning));

  auto sddp_opts = make_fail_stop_options(/*fail_stop=*/false);

  gtopt::test::LogCapture logs;
  SDDPMethod sddp(plp, sddp_opts);
  [[maybe_unused]] auto results = sddp.solve();

  // The legacy backtracking tag must appear in the log; the
  // fail-stop tag must NOT.  This is the symmetric contrapositive
  // of Branch 1 above and is the cleanest evidence the cascade was
  // taken (the alternative — manually counting LP solves — would
  // require instrumented hooks the project does not provide).
  CHECK(logs.contains("backtrack→p"));
  CHECK_FALSE(logs.contains("fail-stop scene"));

  // At least one feasibility cut must have been installed during
  // the cascade — same lower-bound invariant as Branch 1.  The
  // fixture geometry guarantees the elastic path fires.
  const auto cuts = sddp.stored_cuts();
  int n_feas = 0;
  for (const auto& c : cuts) {
    if (c.type == CutType::Feasibility) {
      ++n_feas;
    }
  }
  CAPTURE(n_feas);
  CAPTURE(cuts.size());
  CHECK(n_feas >= 1);
}

// ─── Branch 3 — Cross-check: both modes reach the elastic fcut path ────────

TEST_CASE(  // NOLINT
    "SDDP sddp_forward_fail_stop modes — both emit at least one elastic fcut")
{
  auto run = [](bool fail_stop) -> int
  {
    auto planning = make_forced_infeasibility_planning();
    PlanningLP plp(std::move(planning));

    auto sddp_opts = make_fail_stop_options(fail_stop);

    SDDPMethod sddp(plp, sddp_opts);
    [[maybe_unused]] auto results = sddp.solve();

    int n_feas = 0;
    for (const auto& c : sddp.stored_cuts()) {
      if (c.type == CutType::Feasibility) {
        ++n_feas;
      }
    }
    return n_feas;
  };

  const int n_feas_failstop = run(true);
  const int n_feas_legacy = run(false);

  CAPTURE(n_feas_failstop);
  CAPTURE(n_feas_legacy);

  // Both branches install ≥ 1 feasibility cut on the same fixture —
  // the difference between them is control-flow (when to stop), not
  // whether cuts are emitted.
  CHECK(n_feas_failstop >= 1);
  CHECK(n_feas_legacy >= 1);
}

// ─── Branch 4 — Option / JSON wiring ───────────────────────────────────────

TEST_CASE(  // NOLINT
    "SDDPOptions::sddp_forward_fail_stop default is true; can be flipped")
{
  // SDDPOptions struct default is `true`; the SddpOptions JSON wrapper
  // is `OptBool` (nullopt by default) and resolves through
  // `PlanningOptionsLP::sddp_forward_fail_stop()` to `true`.
  const SDDPOptions defaults {};
  CHECK(defaults.forward_fail_stop == true);

  SDDPOptions flipped;
  flipped.forward_fail_stop = false;
  CHECK(flipped.forward_fail_stop == false);
}

TEST_CASE(  // NOLINT
    "PlanningOptionsLP::sddp_forward_fail_stop default true; honours OptBool")
{
  // Default-constructed PlanningOptionsLP — no `forward_fail_stop` set
  // anywhere in the JSON-derived option bag.
  const PlanningOptionsLP defaults_lp;
  CHECK(defaults_lp.sddp_forward_fail_stop() == true);

  // Explicit override via `PlanningOptions::sddp_options` → wraps
  // through `value_or(default_sddp_forward_fail_stop)`.
  PlanningOptions opts;
  opts.sddp_options.forward_fail_stop = OptBool {false};
  const PlanningOptionsLP flipped_lp(std::move(opts));
  CHECK(flipped_lp.sddp_forward_fail_stop() == false);

  PlanningOptions opts_true;
  opts_true.sddp_options.forward_fail_stop = OptBool {true};
  const PlanningOptionsLP flipped_true_lp(std::move(opts_true));
  CHECK(flipped_true_lp.sddp_forward_fail_stop() == true);
}

}  // namespace
