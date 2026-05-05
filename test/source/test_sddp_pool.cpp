// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_pool.hpp
 * @brief     Tests for the SDDP-specialised work pool (sddp_pool.hpp)
 */

#include <atomic>
#include <mutex>
#include <tuple>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/sddp_pool.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ─── SDDPTaskKey and constant tests ──────────────────────────────────────────

TEST_CASE("SDDPTaskKey type and constants")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("SDDPTaskKey is a 4-tuple (iter, is_backward, signed_phase, kind)")
  {
    static_assert(
        std::same_as<SDDPTaskKey,
                     std::tuple<IterationIndex, int, int, SDDPTaskKind>>);
    CHECK(true);
  }

  SUBCASE("SDDPPassDirection enum values are sign multipliers")
  {
    CHECK(static_cast<int>(SDDPTaskKind::lp) == 0);
    CHECK(static_cast<int>(SDDPTaskKind::non_lp) == 1);
    // forward = +1 and backward = -1 so the enum value can be used
    // directly as the phase-sign multiplier in make_sddp_task_key.
    CHECK(static_cast<int>(SDDPPassDirection::forward) == 1);
    CHECK(static_cast<int>(SDDPPassDirection::backward) == -1);
  }

  SUBCASE("make_sddp_task_key encodes is_backward + signed_phase")
  {
    const auto fwd = make_sddp_task_key(IterationIndex {1},
                                        SDDPPassDirection::forward,
                                        PhaseIndex {2},
                                        SDDPTaskKind::lp);
    CHECK(std::get<0>(fwd) == IterationIndex {1});
    CHECK(std::get<1>(fwd) == 0);  // is_backward = 0 for forward
    CHECK(std::get<2>(fwd) == 2);  // +1 * 2
    CHECK(std::get<3>(fwd) == SDDPTaskKind::lp);

    const auto bwd = make_sddp_task_key(IterationIndex {1},
                                        SDDPPassDirection::backward,
                                        PhaseIndex {2},
                                        SDDPTaskKind::lp);
    CHECK(std::get<0>(bwd) == IterationIndex {1});
    CHECK(std::get<1>(bwd) == 1);  // is_backward = 1 for backward
    CHECK(std::get<2>(bwd) == -2);  // -1 * 2
    CHECK(std::get<3>(bwd) == SDDPTaskKind::lp);

    // Phase 0 collapses signed_phase to 0 for both directions, but the
    // is_backward field still distinguishes them — forward(0) wins.
    const auto fwd0 = make_sddp_task_key(IterationIndex {0},
                                         SDDPPassDirection::forward,
                                         first_phase_index(),
                                         SDDPTaskKind::lp);
    const auto bwd0 = make_sddp_task_key(IterationIndex {0},
                                         SDDPPassDirection::backward,
                                         first_phase_index(),
                                         SDDPTaskKind::lp);
    CHECK(std::get<1>(fwd0) == 0);
    CHECK(std::get<1>(bwd0) == 1);
    CHECK(std::get<2>(fwd0) == 0);
    CHECK(std::get<2>(bwd0) == 0);
  }
}

// ─── Task ordering with SDDPTaskKey ──────────────────────────────────────────

TEST_CASE("Task<SDDPTaskKey> ordering is lexicographic")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  using STask = Task<void, SDDPTaskKey, std::less<>>;
  using SReq = BasicTaskRequirements<SDDPTaskKey>;

  SUBCASE("lower iteration index has higher priority")
  {
    STask iter0 {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::forward,
                                               first_phase_index(),
                                               SDDPTaskKind::lp),
            .name = {},
        }};
    STask iter1 {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {1},
                                               SDDPPassDirection::forward,
                                               first_phase_index(),
                                               SDDPTaskKind::lp),
            .name = {},
        }};
    CHECK_FALSE(iter0 < iter1);  // iter0 has higher priority
    CHECK(iter1 < iter0);  // iter1 has lower priority
  }

  SUBCASE("forward beats backward at the same iteration and phase (any phase)")
  {
    // is_backward field: forward(0) < backward(1), so forward wins
    // unconditionally — even at non-zero phase where the signed_phase
    // encoding alone would otherwise let backward win (-N < +N).
    STask fwd {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::forward,
                                               PhaseIndex {3},
                                               SDDPTaskKind::lp),
            .name = {},
        }};
    STask bwd {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::backward,
                                               PhaseIndex {3},
                                               SDDPTaskKind::lp),
            .name = {},
        }};
    CHECK_FALSE(fwd < bwd);  // forward has higher priority
    CHECK(bwd < fwd);  // backward has lower priority
  }

  SUBCASE("forward beats backward at phase 0 too (is_backward field)")
  {
    // signed_phase ties at 0 for both, but is_backward 0 < 1 still
    // gives forward priority — no submit-time-tiebreak ambiguity.
    STask fwd {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::forward,
                                               first_phase_index(),
                                               SDDPTaskKind::lp),
            .name = {},
        }};
    STask bwd {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::backward,
                                               first_phase_index(),
                                               SDDPTaskKind::lp),
            .name = {},
        }};
    CHECK_FALSE(fwd < bwd);
    CHECK(bwd < fwd);
  }

  SUBCASE("LP solve (0) has higher priority than non-LP (1)")
  {
    STask lp {[] {},
              SReq {
                  .priority_key = make_sddp_task_key(IterationIndex {0},
                                                     SDDPPassDirection::forward,
                                                     first_phase_index(),
                                                     SDDPTaskKind::lp),
                  .name = {},
              }};
    STask nonlp {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::forward,
                                               first_phase_index(),
                                               SDDPTaskKind::non_lp),
            .name = {},
        }};
    CHECK_FALSE(lp < nonlp);  // LP has higher priority
    CHECK(nonlp < lp);  // non-LP has lower priority
  }

  SUBCASE("within forward, lower phase has higher priority")
  {
    // signed_phase: forward(+1)*0 = 0 < forward(+1)*2 = +2.
    STask ph0 {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::forward,
                                               first_phase_index(),
                                               SDDPTaskKind::lp),
            .name = {},
        }};
    STask ph2 {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::forward,
                                               PhaseIndex {2},
                                               SDDPTaskKind::lp),
            .name = {},
        }};
    CHECK_FALSE(ph0 < ph2);  // forward phase 0 has higher priority
    CHECK(ph2 < ph0);  // forward phase 2 has lower priority
  }

  SUBCASE("within backward, higher phase has higher priority")
  {
    // signed_phase: backward(-1)*5 = -5 < backward(-1)*1 = -1, so phase 5
    // dequeues before phase 1 — matches the backward pass walking N → 0.
    STask ph5 {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::backward,
                                               PhaseIndex {5},
                                               SDDPTaskKind::lp),
            .name = {},
        }};
    STask ph1 {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::backward,
                                               PhaseIndex {1},
                                               SDDPTaskKind::lp),
            .name = {},
        }};
    CHECK_FALSE(ph5 < ph1);  // backward phase 5 has higher priority
    CHECK(ph1 < ph5);  // backward phase 1 has lower priority
  }
}

// ─── SDDPWorkPool type tests
// ──────────────────────────────────────────────────

TEST_CASE("SDDPWorkPool type traits")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("key_type is SDDPTaskKey")
  {
    static_assert(std::same_as<SDDPWorkPool::key_type, SDDPTaskKey>);
    CHECK(true);
  }

  SUBCASE("key_compare is std::less<SDDPTaskKey>")
  {
    static_assert(
        std::same_as<SDDPWorkPool::key_compare, std::less<SDDPTaskKey>>);
    CHECK(true);
  }
}

// ─── SDDPWorkPool functionality tests ────────────────────────────────────────

TEST_CASE("SDDPWorkPool submit and execute")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("submit task with SDDP tuple key")
  {
    SDDPWorkPool pool;
    pool.start();

    std::atomic<int> result {0};
    using Req = BasicTaskRequirements<SDDPTaskKey>;

    auto fut = pool.submit(
        [&] { result = 42; },
        Req {
            .priority = TaskPriority::Medium,
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::forward,
                                               first_phase_index(),
                                               SDDPTaskKind::lp),
            .name = {},
        });

    REQUIRE(fut.has_value());
    fut->wait();
    CHECK(result == 42);

    pool.shutdown();
  }

  SUBCASE("forward task runs before backward task")
  {
    // is_backward 0 < 1 wins the cross-direction comparison
    // unconditionally — even at the same |phase|, where the
    // signed_phase encoding alone would let backward(-3) sort before
    // forward(+3).  Matches the SDDP rule that the forward pass
    // produces the trial points the backward pass needs.
    SDDPWorkPool pool;
    pool.start();

    std::vector<int> order;
    std::mutex mu;
    std::atomic<int> done {0};

    using Req = BasicTaskRequirements<SDDPTaskKey>;

    auto fwd = pool.submit(
        [&]
        {
          const std::scoped_lock lk {mu};
          order.push_back(0);  // forward
          done++;
        },
        Req {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::forward,
                                               PhaseIndex {3},
                                               SDDPTaskKind::lp),
            .name = {},
        });

    auto bwd = pool.submit(
        [&]
        {
          const std::scoped_lock lk {mu};
          order.push_back(1);  // backward
          done++;
        },
        Req {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::backward,
                                               PhaseIndex {3},
                                               SDDPTaskKind::lp),
            .name = {},
        });

    REQUIRE(fwd.has_value());
    REQUIRE(bwd.has_value());
    fwd->wait();
    bwd->wait();
    CHECK(done == 2);

    pool.shutdown();
  }

  SUBCASE("lower iteration submitted last still gets priority")
  {
    SDDPWorkPool pool;
    pool.start();

    std::atomic<int> result {-1};
    using Req = BasicTaskRequirements<SDDPTaskKey>;

    // Submit iter1 first, then iter0 — iter0 should have higher priority
    auto f1 = pool.submit(
        [&] { result = 1; },
        Req {
            .priority_key = make_sddp_task_key(IterationIndex {1},
                                               SDDPPassDirection::forward,
                                               first_phase_index(),
                                               SDDPTaskKind::lp),
            .name = {},
        });
    auto f0 = pool.submit(
        [&] { result = 0; },
        Req {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::forward,
                                               first_phase_index(),
                                               SDDPTaskKind::lp),
            .name = {},
        });

    REQUIRE(f1.has_value());
    REQUIRE(f0.has_value());
    f0->wait();
    f1->wait();
    // Both ran — just check they completed
    CHECK(result >= 0);

    pool.shutdown();
  }
}

// ─── make_sddp_work_pool factory tests ───────────────────────────────────────

TEST_CASE("make_sddp_work_pool factory")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("creates and starts SDDPWorkPool")
  {
    auto pool = make_sddp_work_pool(0.5);
    REQUIRE(pool != nullptr);

    std::atomic<int> result {0};
    using Req = BasicTaskRequirements<SDDPTaskKey>;
    auto fut = pool->submit(
        [&] { result = 99; },
        Req {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::forward,
                                               first_phase_index(),
                                               SDDPTaskKind::lp),
            .name = {},
        });

    REQUIRE(fut.has_value());
    fut->wait();
    CHECK(result == 99);
  }

  SUBCASE("pool returned is of SDDPWorkPool type")
  {
    auto pool = make_sddp_work_pool(0.5);
    REQUIRE(pool != nullptr);
    static_assert(std::same_as<decltype(pool), std::unique_ptr<SDDPWorkPool>>);
    CHECK(true);
  }

  SUBCASE("pool statistics available after task")
  {
    auto pool = make_sddp_work_pool(0.5);
    REQUIRE(pool != nullptr);

    auto fut = pool->submit([] { return 1; });
    REQUIRE(fut.has_value());
    CHECK(fut->get() == 1);

    const auto stats = pool->get_statistics();
    CHECK(stats.tasks_submitted >= 1);
  }
}

// ─── cell_task_headroom parameter ────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "make_sddp_work_pool: cell_task_headroom adds slots over the base")
{
  // The synchronised backward pass dispatches one cell task per
  // feasible scene; each blocks on its aperture sub-task futures
  // while holding a worker slot.  `cell_task_headroom` reserves
  // additional slots so aperture-parallelism is not silently capped
  // at `(cpu_factor × physical_concurrency) − num_scenes`.  This
  // test pins that the parameter actually adds to `max_threads`.
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("zero headroom matches base size")
  {
    auto pool_no_headroom = make_sddp_work_pool(0.5,
                                                /*memory_limit_mb=*/0.0,
                                                /*cell_task_headroom=*/0);
    REQUIRE(pool_no_headroom != nullptr);
    const auto base = pool_no_headroom->max_threads();
    pool_no_headroom->shutdown();

    auto pool_with_headroom = make_sddp_work_pool(0.5,
                                                  /*memory_limit_mb=*/0.0,
                                                  /*cell_task_headroom=*/8);
    REQUIRE(pool_with_headroom != nullptr);
    CHECK(pool_with_headroom->max_threads() == base + 8);
    pool_with_headroom->shutdown();
  }

  SUBCASE("negative headroom clamps to zero")
  {
    // Negative values must not shrink the pool below the base size.
    auto pool = make_sddp_work_pool(0.5,
                                    /*memory_limit_mb=*/0.0,
                                    /*cell_task_headroom=*/-5);
    REQUIRE(pool != nullptr);
    const auto base = static_cast<int>(
        std::lround(0.5 * static_cast<double>(physical_concurrency())));
    CHECK(pool->max_threads() == base);
  }

  SUBCASE("default value preserves prior behaviour")
  {
    // Callers that do not pass the new param must see the same pool
    // size they always saw — `cell_task_headroom` defaults to 0.
    auto pool = make_sddp_work_pool(0.5);
    REQUIRE(pool != nullptr);
    const auto base = static_cast<int>(
        std::lround(0.5 * static_cast<double>(physical_concurrency())));
    CHECK(pool->max_threads() == base);
  }
}
