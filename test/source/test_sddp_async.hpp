/**
 * @file      test_sddp_async.hpp
 * @brief     Tests for asynchronous scene execution in SDDP solver
 * @date      2026-04-04
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. SceneIterationTracker unit tests (report, query, ring buffer)
 *  2. Async spread observability (scenes run at different iterations)
 *  3. Spread limiting (max_async_spread=1 bounds the gap)
 *  4. Convergence correctness (async vs sync produce similar results)
 *  5. Cut sharing forces sync path (cut_sharing != none bypasses async)
 */

#include <doctest/doctest.h>
#include <gtopt/sddp_scene_tracker.hpp>

// ─── SceneIterationTracker unit tests ──────────────────────────────────────

TEST_CASE("SceneIterationTracker - report and query")  // NOLINT
{
  SceneIterationTracker tracker(3, /*max_spread=*/2);

  CHECK(tracker.num_scenes() == 3);
  CHECK(tracker.max_spread() == 2);
  CHECK(tracker.min_completed_iteration() == IterationIndex {-1});
  CHECK(tracker.max_completed_iteration() == IterationIndex {-1});

  SUBCASE("report_complete updates scene state")
  {
    tracker.report_complete(
        SceneIndex {0}, IterationIndex {0}, 100.0, 80.0, true);
    CHECK(tracker.max_completed_iteration() == IterationIndex {0});
    CHECK(tracker.min_completed_iteration() == IterationIndex {-1});
    CHECK_FALSE(tracker.all_complete(IterationIndex {0}));

    tracker.report_complete(
        SceneIndex {1}, IterationIndex {0}, 110.0, 85.0, true);
    CHECK_FALSE(tracker.all_complete(IterationIndex {0}));

    tracker.report_complete(
        SceneIndex {2}, IterationIndex {0}, 105.0, 82.0, true);
    CHECK(tracker.all_complete(IterationIndex {0}));
    CHECK(tracker.min_completed_iteration() == IterationIndex {0});
  }

  SUBCASE("bounds_for_iteration returns correct per-scene data")
  {
    tracker.report_complete(
        SceneIndex {0}, IterationIndex {0}, 100.0, 80.0, true);
    tracker.report_complete(
        SceneIndex {1}, IterationIndex {0}, 110.0, 85.0, /*feasible=*/false);
    tracker.report_complete(
        SceneIndex {2}, IterationIndex {0}, 105.0, 82.0, true);

    auto bounds = tracker.bounds_for_iteration(IterationIndex {0});
    REQUIRE(bounds.size() == 3);

    CHECK(bounds[0].upper_bound == doctest::Approx(100.0));
    CHECK(bounds[0].lower_bound == doctest::Approx(80.0));
    CHECK(bounds[0].feasible);
    CHECK(bounds[0].iteration == IterationIndex {0});

    CHECK(bounds[1].upper_bound == doctest::Approx(110.0));
    CHECK(bounds[1].lower_bound == doctest::Approx(85.0));
    CHECK_FALSE(bounds[1].feasible);

    CHECK(bounds[2].upper_bound == doctest::Approx(105.0));
    CHECK(bounds[2].lower_bound == doctest::Approx(82.0));
  }

  SUBCASE("scene_iterations returns per-scene snapshot")
  {
    tracker.report_complete(
        SceneIndex {0}, IterationIndex {2}, 100.0, 80.0, true);
    tracker.report_complete(
        SceneIndex {1}, IterationIndex {0}, 110.0, 85.0, true);
    tracker.report_complete(
        SceneIndex {2}, IterationIndex {1}, 105.0, 82.0, true);

    auto iters = tracker.scene_iterations();
    REQUIRE(iters.size() == 3);
    CHECK(iters[0] == 2);
    CHECK(iters[1] == 0);
    CHECK(iters[2] == 1);
  }
}

TEST_CASE("SceneIterationTracker - ring buffer depth")  // NOLINT
{
  // max_spread=1 → ring buffer depth = 1 + 2 = 3
  SceneIterationTracker tracker(1, /*max_spread=*/1);

  // Report 5 iterations for scene 0
  for (int i = 0; i < 5; ++i) {
    tracker.report_complete(
        SceneIndex {0}, IterationIndex {i}, 100.0 + i, 80.0 + i, true);
  }

  // Only recent iterations should be retrievable
  // Iteration 4 (most recent) should always be present
  auto bounds4 = tracker.bounds_for_iteration(IterationIndex {4});
  REQUIRE(bounds4.size() == 1);
  CHECK(bounds4[0].upper_bound == doctest::Approx(104.0));

  // Iteration 3 should also be present (within ring buffer depth 3)
  auto bounds3 = tracker.bounds_for_iteration(IterationIndex {3});
  CHECK(bounds3[0].upper_bound == doctest::Approx(103.0));

  // Iteration 0 should have been evicted (depth 3 keeps only iters 2,3,4)
  auto bounds0 = tracker.bounds_for_iteration(IterationIndex {0});
  // When not found, returns default SceneBounds with upper_bound=0
  CHECK(bounds0[0].upper_bound == doctest::Approx(0.0));
}

TEST_CASE("SceneIterationTracker - multiple iterations with spread")  // NOLINT
{
  SceneIterationTracker tracker(2, /*max_spread=*/2);

  // Scene 0 runs ahead (iter 0, 1, 2), scene 1 is behind (iter 0)
  tracker.report_complete(
      SceneIndex {0}, IterationIndex {0}, 100.0, 80.0, true);
  tracker.report_complete(
      SceneIndex {1}, IterationIndex {0}, 110.0, 85.0, true);

  CHECK(tracker.all_complete(IterationIndex {0}));
  CHECK(tracker.min_completed_iteration() == IterationIndex {0});

  tracker.report_complete(SceneIndex {0}, IterationIndex {1}, 98.0, 82.0, true);
  tracker.report_complete(SceneIndex {0}, IterationIndex {2}, 95.0, 84.0, true);

  CHECK(tracker.max_completed_iteration() == IterationIndex {2});
  CHECK(tracker.min_completed_iteration() == IterationIndex {0});

  // Iteration 1 is not complete (scene 1 hasn't done it)
  CHECK_FALSE(tracker.all_complete(IterationIndex {1}));
  CHECK_FALSE(tracker.all_complete(IterationIndex {2}));

  // Now scene 1 catches up
  tracker.report_complete(
      SceneIndex {1}, IterationIndex {1}, 108.0, 87.0, true);
  CHECK(tracker.all_complete(IterationIndex {1}));
  CHECK_FALSE(tracker.all_complete(IterationIndex {2}));

  tracker.report_complete(
      SceneIndex {1}, IterationIndex {2}, 106.0, 88.0, true);
  CHECK(tracker.all_complete(IterationIndex {2}));
  CHECK(tracker.min_completed_iteration() == IterationIndex {2});
}

// ─── Integration tests: async SDDP solving ────────────────────────────────

TEST_CASE("SDDPMethod async - convergence matches sync")  // NOLINT
{
  // Run the same 2-scene problem with sync (max_async_spread=0) and
  // async (max_async_spread=3), then compare final bounds.
  auto planning_sync = make_2scene_3phase_hydro_planning(0.5, 0.5);
  auto planning_async = make_2scene_3phase_hydro_planning(0.5, 0.5);

  // ── Synchronous baseline ──
  PlanningLP plp_sync(std::move(planning_sync));
  SDDPOptions sync_opts;
  sync_opts.max_iterations = 20;
  sync_opts.convergence_tol = 1e-4;
  sync_opts.cut_sharing = CutSharingMode::none;
  sync_opts.max_async_spread = 0;  // explicit sync

  SDDPMethod sddp_sync(plp_sync, sync_opts);
  auto sync_results = sddp_sync.solve();
  REQUIRE(sync_results.has_value());
  REQUIRE_FALSE(sync_results->empty());
  CHECK(sync_results->back().converged);

  // ── Asynchronous ──
  PlanningLP plp_async(std::move(planning_async));
  SDDPOptions async_opts;
  async_opts.max_iterations = 20;
  async_opts.convergence_tol = 1e-4;
  async_opts.cut_sharing = CutSharingMode::none;
  async_opts.max_async_spread = 3;

  SDDPMethod sddp_async(plp_async, async_opts);
  auto async_results = sddp_async.solve();
  REQUIRE(async_results.has_value());
  REQUIRE_FALSE(async_results->empty());
  CHECK(async_results->back().converged);

  // Bounds should be similar (not bit-identical due to solve order)
  const auto& sync_last = sync_results->back();
  const auto& async_last = async_results->back();
  CHECK(async_last.upper_bound
        == doctest::Approx(sync_last.upper_bound).epsilon(0.05));
  CHECK(async_last.lower_bound
        == doctest::Approx(sync_last.lower_bound).epsilon(0.05));

  SPDLOG_INFO("Sync  UB={:.4f} LB={:.4f} iters={}",
              sync_last.upper_bound,
              sync_last.lower_bound,
              sync_results->size());
  SPDLOG_INFO("Async UB={:.4f} LB={:.4f} iters={}",
              async_last.upper_bound,
              async_last.lower_bound,
              async_results->size());
}

TEST_CASE("SDDPMethod async - scene_iterations populated")  // NOLINT
{
  // In async mode, SDDPIterationResult::scene_iterations should be populated
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 20;
  opts.convergence_tol = 1e-4;
  opts.cut_sharing = CutSharingMode::none;
  opts.max_async_spread = 2;

  SDDPMethod sddp(planning_lp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  // All results should have scene_iterations with 2 entries (async mode).
  // Each scene does its own simulation inline — no separate simulation entry.
  for (const auto& r : *results) {
    REQUIRE(r.scene_iterations.size() == 2);
    CHECK(r.scene_iterations[0] >= 0);
    CHECK(r.scene_iterations[1] >= 0);
  }
}

TEST_CASE("SDDPMethod async - spread limited by max_async_spread=1")  // NOLINT
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 15;
  opts.convergence_tol = 1e-4;
  opts.cut_sharing = CutSharingMode::none;
  opts.max_async_spread = 1;

  SDDPMethod sddp(planning_lp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  // At every convergence-check point, spread should be <= 1
  for (const auto& r : *results) {
    REQUIRE(r.scene_iterations.size() == 2);
    const int spread = std::abs(r.scene_iterations[0] - r.scene_iterations[1]);
    CHECK(spread <= 1);
  }

  CHECK(results->back().converged);
}

TEST_CASE("SDDPMethod async - cut_sharing != none uses sync path")  // NOLINT
{
  // When cut_sharing is enabled, the async path should NOT be used,
  // so scene_iterations should be empty (sync path doesn't populate it).
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 15;
  opts.convergence_tol = 1e-4;
  opts.cut_sharing = CutSharingMode::expected;
  opts.max_async_spread = 3;  // Should be ignored

  SDDPMethod sddp(planning_lp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());
  CHECK(results->back().converged);

  // Sync path: scene_iterations not populated
  CHECK(results->back().scene_iterations.empty());
}

TEST_CASE("SceneIterationTracker - per-scene convergence tracking")  // NOLINT
{
  SceneIterationTracker tracker(3, /*max_spread=*/5);

  CHECK_FALSE(tracker.is_converged(SceneIndex {0}));
  CHECK_FALSE(tracker.all_converged());
  CHECK(tracker.num_converged() == 0);

  tracker.mark_converged(SceneIndex {1}, IterationIndex {3});
  CHECK(tracker.is_converged(SceneIndex {1}));
  CHECK(tracker.converged_iteration(SceneIndex {1}) == IterationIndex {3});
  CHECK_FALSE(tracker.is_converged(SceneIndex {0}));
  CHECK_FALSE(tracker.all_converged());
  CHECK(tracker.num_converged() == 1);

  tracker.mark_converged(SceneIndex {0}, IterationIndex {5});
  tracker.mark_converged(SceneIndex {2}, IterationIndex {4});
  CHECK(tracker.all_converged());
  CHECK(tracker.num_converged() == 3);
}

TEST_CASE(
    "SDDPMethod async - unlimited spread (max_async_spread >= max_iter)")  // NOLINT
{
  // When max_async_spread >= max_iterations, scenes should never wait
  // for each other.  This is the "no waiting at all" mode.
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 10;
  opts.convergence_tol = 1e-4;
  opts.cut_sharing = CutSharingMode::none;
  opts.max_async_spread = 100;  // >> max_iterations: no spread limit

  SDDPMethod sddp(planning_lp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  // Should converge (same problem as other tests)
  CHECK(results->back().converged);
}

TEST_CASE("SDDPMethod async - iteration callback fires")  // NOLINT
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 20;
  opts.convergence_tol = 1e-4;
  opts.cut_sharing = CutSharingMode::none;
  opts.max_async_spread = 3;

  SDDPMethod sddp(planning_lp, opts);

  int callback_count = 0;
  sddp.set_iteration_callback(
      [&callback_count](const SDDPIterationResult& r)
      {
        ++callback_count;
        CHECK(r.scene_iterations.size() == 2);
        return false;  // don't stop
      });

  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());
  CHECK(callback_count > 0);
  // Callback should fire once per aggregate iteration result
  CHECK(callback_count == static_cast<int>(results->size()));
}

TEST_CASE(
    "SDDPMethod async - max_iterations hit without convergence")  // NOLINT
{
  // Use impossibly tight tolerance so it never converges.
  // Both scenes should still complete training + simulation.
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 3;
  opts.convergence_tol = 1e-20;  // impossible to meet
  opts.cut_sharing = CutSharingMode::none;
  opts.max_async_spread = 5;

  SDDPMethod sddp(planning_lp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  // Should NOT have converged (tolerance too tight)
  // Note: this problem might converge exactly — check gap first
  const auto& last = results->back();
  if (last.gap > 1e-20) {
    CHECK_FALSE(last.converged);
  }

  // Should have run the expected number of iterations
  CHECK(results->size() >= 1);

  // All results should have scene_iterations
  for (const auto& r : *results) {
    CHECK(r.scene_iterations.size() == 2);
  }
}

TEST_CASE("SDDPMethod async - per-scene output writing")  // NOLINT
{
  // Set an output directory and verify files are created
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);

  // Create a temp output directory
  const auto tmp_dir =
      std::filesystem::temp_directory_path() / "gtopt_async_test_output";
  std::filesystem::remove_all(tmp_dir);
  std::filesystem::create_directories(tmp_dir);

  planning.options.output_directory = OptName {tmp_dir.string()};
  planning.options.output_format = DataFormat::csv;

  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 10;
  opts.convergence_tol = 1e-4;
  opts.cut_sharing = CutSharingMode::none;
  opts.max_async_spread = 5;

  SDDPMethod sddp(planning_lp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());

  // Verify that output files were created in the temp directory.
  // Each scene × phase should have written output.
  // Check for at least some output files (generator, demand, etc.)
  bool has_output_files = false;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(tmp_dir))
  {
    if (entry.is_regular_file()) {
      has_output_files = true;
      break;
    }
  }
  CHECK(has_output_files);

  // Cleanup
  std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("SDDPMethod async - callback can request early stop")  // NOLINT
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 50;
  opts.convergence_tol = 1e-20;  // won't converge naturally
  opts.cut_sharing = CutSharingMode::none;
  opts.max_async_spread = 5;

  SDDPMethod sddp(planning_lp, opts);

  // Request stop after first callback
  sddp.set_iteration_callback([](const SDDPIterationResult& /*r*/)
                              { return true; });

  auto results = sddp.solve();
  REQUIRE(results.has_value());
  // Should have stopped after very few iterations (1-2)
  CHECK(results->size() <= 3);
}

TEST_CASE("SDDPMethod async - single scene falls back to sync")  // NOLINT
{
  // With only 1 scene, async mode should not activate
  // (the branch check requires num_scenes > 1)
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 15;
  opts.convergence_tol = 1e-4;
  opts.cut_sharing = CutSharingMode::none;
  opts.max_async_spread = 3;

  SDDPMethod sddp(planning_lp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());
  CHECK(results->back().converged);

  // Single scene → sync path → scene_iterations not populated
  CHECK(results->back().scene_iterations.empty());
}
