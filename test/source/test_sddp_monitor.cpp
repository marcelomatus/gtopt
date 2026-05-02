/**
 * @file      test_solver_status.hpp
 * @brief     Unit tests for the SDDP monitoring API
 * @date      2026-03-18
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. SolverStatusSnapshot default construction
 *  2. SDDPIterationResult default construction
 *  3. write_solver_status produces valid JSON with expected keys
 *  4. write_solver_status handles empty results vector
 */

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/sddp_types.hpp>
#include <gtopt/solver_monitor.hpp>
#include <gtopt/solver_status.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── SolverStatusSnapshot tests
// ───────────────────────────────────────────────

TEST_CASE("SolverStatusSnapshot default construction")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const SolverStatusSnapshot snap {};

  CHECK(snap.iteration_index == 0);
  CHECK(snap.gap == doctest::Approx(0.0));
  CHECK(snap.lower_bound == doctest::Approx(0.0));
  CHECK(snap.upper_bound == doctest::Approx(0.0));
  CHECK_FALSE(snap.converged);
  CHECK(snap.max_iterations == 0);
  CHECK(snap.min_iterations == 0);
  CHECK(snap.current_pass == 0);
  CHECK(snap.scenes_done == 0);
}

TEST_CASE("SolverStatusSnapshot with custom values")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const SolverStatusSnapshot snap {
      .iteration_index = IterationIndex {5},
      .gap = 0.01,
      .lower_bound = 1000.0,
      .upper_bound = 1010.0,
      .converged = true,
      .max_iterations = 100,
      .min_iterations = 2,
      .current_pass = 1,
      .scenes_done = 3,
  };

  CHECK(snap.iteration_index == 5);
  CHECK(snap.gap == doctest::Approx(0.01));
  CHECK(snap.lower_bound == doctest::Approx(1000.0));
  CHECK(snap.upper_bound == doctest::Approx(1010.0));
  CHECK(snap.converged);
  CHECK(snap.max_iterations == 100);
  CHECK(snap.min_iterations == 2);
  CHECK(snap.current_pass == 1);
  CHECK(snap.scenes_done == 3);
}

// ─── SDDPIterationResult default construction ───────────────────────────────

TEST_CASE("SDDPIterationResult default construction")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const SDDPIterationResult result {};

  CHECK(result.iteration_index == IterationIndex {0});
  CHECK(result.lower_bound == doctest::Approx(0.0));
  CHECK(result.upper_bound == doctest::Approx(0.0));
  CHECK(result.gap == doctest::Approx(0.0));
  CHECK_FALSE(result.converged);
  CHECK(result.cuts_added == 0);
  CHECK_FALSE(result.feasibility_issue);
  CHECK(result.forward_pass_s == doctest::Approx(0.0));
  CHECK(result.backward_pass_s == doctest::Approx(0.0));
  CHECK(result.iteration_s == doctest::Approx(0.0));
  CHECK(result.infeasible_cuts_added == 0);
  CHECK(result.scene_upper_bounds.empty());
  CHECK(result.scene_lower_bounds.empty());
  CHECK(result.scene_feasible.empty());
}

// ─── Scene-feasibility headline propagation ─────────────────────────────────
//
// `SDDPIterationResult::scene_feasible` is the structural backing for the
// `feasible=K/N` clause in the per-iteration log headline (sddp_iteration.cpp
// commit 4dd87e17).  It is populated by copy from
// `ForwardPassOutcome::scene_feasible` so that downstream code can still
// read the outcome's own copy after `ir` takes its snapshot.  These tests
// pin the propagation contract and the K/N-count semantics that the log
// formatter depends on.

TEST_CASE(  // NOLINT
    "SDDPIterationResult scene_feasible — populated by copy from "
    "ForwardPassOutcome")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // Synthesise a forward outcome with a 4-scene mix:  s0 / s2 / s3 feasible,
  // s1 infeasible.  The training-pass code path in
  // `sddp_iteration.cpp::solve` sets `ir.scene_feasible = fwd->scene_feasible`
  // (copy, not move — see commit 4dd87e17).  Mimic that here.
  ForwardPassOutcome fwd {};
  fwd.scene_feasible = std::vector<uint8_t> {1, 0, 1, 1};
  fwd.scene_upper_bounds = {100.0, 0.0, 200.0, 300.0};

  SDDPIterationResult ir {};
  ir.scene_feasible = fwd.scene_feasible;  // mirrors the copy in solve()
  ir.scene_upper_bounds = fwd.scene_upper_bounds;

  // Forward outcome's vector must survive the copy — many downstream call
  // sites (`run_backward_pass_all_scenes`, `save_cuts_for_iteration`, the
  // sim-pass `output_skipped` loop) still read `fwd->scene_feasible` after
  // `ir` is populated.  An earlier draft moved instead of copying and
  // crashed on `std::span` out-of-bounds when the bounds-computation read
  // the moved-from vector.
  REQUIRE(fwd.scene_feasible.size() == 4);
  CHECK(fwd.scene_feasible[0] == 1);
  CHECK(fwd.scene_feasible[1] == 0);
  CHECK(fwd.scene_feasible[2] == 1);
  CHECK(fwd.scene_feasible[3] == 1);

  // Result mirrors the source.
  REQUIRE(ir.scene_feasible.size() == 4);
  CHECK(ir.scene_feasible[0] == 1);
  CHECK(ir.scene_feasible[1] == 0);
  CHECK(ir.scene_feasible[2] == 1);
  CHECK(ir.scene_feasible[3] == 1);
}

TEST_CASE(  // NOLINT
    "SDDPIterationResult scene_feasible — K/N count semantics for the "
    "feasible=K/N log clause")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // The log headline emits ``feasible=K/N`` only when K < N.  K is computed
  // as ``std::ranges::count(scene_feasible, uint8_t{1})``; N is
  // ``scene_feasible.size()``.  Pin both edge cases the formatter cares
  // about.

  SUBCASE("all-feasible — clause suppressed (K == N)")
  {
    SDDPIterationResult ir {};
    ir.scene_feasible = {1, 1, 1};
    const auto n_total = ir.scene_feasible.size();
    const auto n_feasible = static_cast<std::size_t>(
        std::ranges::count(ir.scene_feasible, uint8_t {1}));
    CHECK(n_feasible == 3);
    CHECK(n_total == 3);
    // Clause-suppression condition is `n_total > 0 && n_feasible < n_total`
    // → false here, so the headline stays terse.
    CHECK_FALSE(n_feasible < n_total);
  }

  SUBCASE("partial — clause present (K < N), juan-like 7/16")
  {
    SDDPIterationResult ir {};
    // Mirror juan/gtopt_iplp 2026-05-02 trace_30 single-bus: 7/16 feasible.
    ir.scene_feasible = {0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0};
    // Wait — that's 5/16.  Use the trace_30 mix: feasible scenes were
    // s2, s4, s5, s6, s8, s9, s14 → 7 of 16.
    ir.scene_feasible = {0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0, 0, 0, 1, 0};
    const auto n_total = ir.scene_feasible.size();
    const auto n_feasible = static_cast<std::size_t>(
        std::ranges::count(ir.scene_feasible, uint8_t {1}));
    CHECK(n_total == 16);
    CHECK(n_feasible == 7);
    CHECK(n_feasible < n_total);  // clause emitted
  }

  SUBCASE("all-infeasible — clause present (K = 0)")
  {
    SDDPIterationResult ir {};
    ir.scene_feasible = {0, 0, 0, 0};
    const auto n_total = ir.scene_feasible.size();
    const auto n_feasible = static_cast<std::size_t>(
        std::ranges::count(ir.scene_feasible, uint8_t {1}));
    CHECK(n_total == 4);
    CHECK(n_feasible == 0);
    CHECK(n_feasible < n_total);
  }

  SUBCASE("empty (no scenes recorded) — clause suppressed")
  {
    // Async-mode path can leave `scene_feasible` empty (scenes complete
    // out-of-order with no aggregate snapshot).  The formatter must emit
    // no clause in this case — `n_total > 0` guards against it.
    SDDPIterationResult ir {};
    REQUIRE(ir.scene_feasible.empty());
    const auto n_total = ir.scene_feasible.size();
    CHECK(n_total == 0);
    // Headline condition `n_total > 0 && n_feasible < n_total` short-circuits
    // → no clause.
    CHECK_FALSE(n_total > 0);
  }
}

// ─── write_solver_status tests ────────────────────────────────────────────

TEST_CASE("write_solver_status produces valid JSON")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_sddp_status.json")
          .string();

  const SolverStatusSnapshot snap {
      .iteration_index = IterationIndex {3},
      .gap = 0.05,
      .lower_bound = 950.0,
      .upper_bound = 1000.0,
      .converged = false,
      .max_iterations = 100,
      .min_iterations = 2,
      .current_pass = 1,
      .scenes_done = 4,
  };

  std::vector<SDDPIterationResult> results;
  results.push_back(SDDPIterationResult {
      .iteration_index = IterationIndex {1},
      .lower_bound = 800.0,
      .upper_bound = 1200.0,
      .gap = 0.33,
      .converged = false,
      .cuts_added = 2,
      .feasibility_issue = false,
      .forward_pass_s = 0.5,
      .backward_pass_s = 0.3,
      .iteration_s = 0.8,
      .infeasible_cuts_added = 0,
      .scene_upper_bounds =
          {
              1200.0,
          },
      .scene_lower_bounds =
          {
              800.0,
          },
  });
  results.push_back(SDDPIterationResult {
      .iteration_index = IterationIndex {2},
      .lower_bound = 900.0,
      .upper_bound = 1050.0,
      .gap = 0.14,
      .converged = false,
      .cuts_added = 2,
      .feasibility_issue = false,
      .forward_pass_s = 0.4,
      .backward_pass_s = 0.2,
      .iteration_s = 0.6,
      .infeasible_cuts_added = 0,
      .scene_upper_bounds =
          {
              1050.0,
          },
      .scene_lower_bounds =
          {
              900.0,
          },
  });

  // Default-constructed SolverMonitor (no background thread started)
  const SolverMonitor monitor;

  write_solver_status(tmp_file, results, 2.5, snap, monitor);
  CHECK(std::filesystem::exists(tmp_file));

  // Read the file content
  const std::ifstream ifs(tmp_file);
  std::ostringstream oss;
  oss << ifs.rdbuf();
  const auto content = oss.str();

  // Verify key JSON fields are present
  CHECK(content.find("\"version\": 1") != std::string::npos);
  CHECK(content.find("\"iteration\": 3") != std::string::npos);
  CHECK(content.find("\"gap\": 0.05") != std::string::npos);
  CHECK(content.find("\"converged\": false") != std::string::npos);
  CHECK(content.find("\"max_iterations\": 100") != std::string::npos);
  CHECK(content.find("\"min_iterations\": 2") != std::string::npos);
  CHECK(content.find("\"lower_bound\": 950.0") != std::string::npos);
  CHECK(content.find("\"upper_bound\": 1000.0") != std::string::npos);
  CHECK(content.find("\"status\": \"running\"") != std::string::npos);
  CHECK(content.find("\"current_pass\": 1") != std::string::npos);
  CHECK(content.find("\"scenes_done\": 4") != std::string::npos);

  // Verify history array is present with iteration entries
  CHECK(content.find("\"history\"") != std::string::npos);
  CHECK(content.find("\"iteration\": 1") != std::string::npos);
  CHECK(content.find("\"iteration\": 2") != std::string::npos);

  // Verify realtime monitoring block is present
  CHECK(content.find("\"realtime\"") != std::string::npos);

  // Clean up
  std::filesystem::remove(tmp_file);
}

TEST_CASE("write_solver_status handles empty results")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_sddp_status_empty.json")
                            .string();

  const SolverStatusSnapshot snap {
      .iteration_index = IterationIndex {0},
      .converged = false,
      .max_iterations = 50,
  };

  const std::vector<SDDPIterationResult> results;
  const SolverMonitor monitor;

  write_solver_status(tmp_file, results, 0.0, snap, monitor);
  CHECK(std::filesystem::exists(tmp_file));

  const std::ifstream ifs(tmp_file);
  std::ostringstream oss;
  oss << ifs.rdbuf();
  const auto content = oss.str();

  // With iteration=0, status should be "initializing"
  CHECK(content.find("\"status\": \"initializing\"") != std::string::npos);
  CHECK(content.find("\"history\": [") != std::string::npos);

  std::filesystem::remove(tmp_file);
}

TEST_CASE(
    "write_solver_status shows converged status when "
    "converged")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_sddp_status_converged.json")
                            .string();

  const SolverStatusSnapshot snap {
      .iteration_index = IterationIndex {10},
      .gap = 1e-5,
      .lower_bound = 999.99,
      .upper_bound = 1000.0,
      .converged = true,
      .max_iterations = 100,
      .min_iterations = 2,
  };

  const std::vector<SDDPIterationResult> results;
  const SolverMonitor monitor;

  write_solver_status(tmp_file, results, 5.0, snap, monitor);

  const std::ifstream ifs(tmp_file);
  std::ostringstream oss;
  oss << ifs.rdbuf();
  const auto content = oss.str();

  CHECK(content.find("\"status\": \"converged\"") != std::string::npos);
  CHECK(content.find("\"converged\": true") != std::string::npos);

  std::filesystem::remove(tmp_file);
}
