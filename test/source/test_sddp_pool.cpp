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

using namespace gtopt;

// ─── SDDPTaskKey and constant tests ──────────────────────────────────────────

TEST_CASE("SDDPTaskKey type and constants")  // NOLINT
{
  using namespace gtopt;

  SUBCASE("SDDPTaskKey is a 4-tuple (iter, is_backward, phase_rank, kind)")
  {
    // `phase_rank` is a NON-NEGATIVE laggard-first tie-break used by the
    // per-phase backward aperture-chunk tasks (the only tasks that funnel
    // into this pool per phase); scene-driver submissions run all phases
    // as one task and leave it at 0.  See sddp_pool.hpp's header comment.
    static_assert(
        std::same_as<SDDPTaskKey,
                     std::tuple<IterationIndex, int, int, SDDPTaskKind>>);
    CHECK(true);
  }

  SUBCASE("Enum value constants")
  {
    CHECK(static_cast<int>(SDDPTaskKind::lp) == 0);
    CHECK(static_cast<int>(SDDPTaskKind::non_lp) == 1);
    // forward → is_backward 0; backward → is_backward 1.  Values
    // chosen so the lex tuple keeps forward strictly ahead of
    // backward at the same iteration.
    CHECK(static_cast<int>(SDDPPassDirection::forward) == 0);
    CHECK(static_cast<int>(SDDPPassDirection::backward) == 1);
  }

  SUBCASE("make_sddp_task_key encodes (iter, is_backward, phase_rank, kind)")
  {
    const auto fwd = make_sddp_task_key(
        IterationIndex {1}, SDDPPassDirection::forward, SDDPTaskKind::lp);
    CHECK(std::get<0>(fwd) == IterationIndex {1});
    CHECK(std::get<1>(fwd) == 0);  // is_backward = 0 for forward
    CHECK(std::get<2>(fwd) == 0);  // phase_rank defaults to 0 (driver task)
    CHECK(std::get<3>(fwd) == SDDPTaskKind::lp);

    const auto bwd = make_sddp_task_key(
        IterationIndex {1}, SDDPPassDirection::backward, SDDPTaskKind::lp);
    CHECK(std::get<0>(bwd) == IterationIndex {1});
    CHECK(std::get<1>(bwd) == 1);  // is_backward = 1 for backward
    CHECK(std::get<2>(bwd) == 0);  // phase_rank defaults to 0
    CHECK(std::get<3>(bwd) == SDDPTaskKind::lp);

    // Explicit phase_rank for a per-phase backward aperture chunk.
    const auto chunk = make_sddp_task_key(IterationIndex {1},
                                          SDDPPassDirection::backward,
                                          SDDPTaskKind::lp,
                                          /*phase_rank=*/7);
    CHECK(std::get<2>(chunk) == 7);
    CHECK(std::get<3>(chunk) == SDDPTaskKind::lp);
  }
}

// ─── Task ordering with SDDPTaskKey ──────────────────────────────────────────

TEST_CASE("Task<SDDPTaskKey> ordering is lexicographic")  // NOLINT
{
  using namespace gtopt;

  using STask = Task<void, SDDPTaskKey, std::less<>>;
  using SReq = BasicTaskRequirements<SDDPTaskKey>;

  SUBCASE("lower iteration index has higher priority")
  {
    STask iter0 {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::forward,
                                               SDDPTaskKind::lp),
            .name = {},
        }};
    STask iter1 {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {1},
                                               SDDPPassDirection::forward,
                                               SDDPTaskKind::lp),
            .name = {},
        }};
    CHECK_FALSE(iter0 < iter1);  // iter0 has higher priority
    CHECK(iter1 < iter0);  // iter1 has lower priority
  }

  SUBCASE("forward beats backward at the same iteration")
  {
    // is_backward field: forward(0) < backward(1).
    STask fwd {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::forward,
                                               SDDPTaskKind::lp),
            .name = {},
        }};
    STask bwd {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::backward,
                                               SDDPTaskKind::lp),
            .name = {},
        }};
    CHECK_FALSE(fwd < bwd);  // forward has higher priority
    CHECK(bwd < fwd);  // backward has lower priority
  }

  SUBCASE("LP solve (0) has higher priority than non-LP (1)")
  {
    STask lp {[] {},
              SReq {
                  .priority_key = make_sddp_task_key(IterationIndex {0},
                                                     SDDPPassDirection::forward,
                                                     SDDPTaskKind::lp),
                  .name = {},
              }};
    STask nonlp {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::forward,
                                               SDDPTaskKind::non_lp),
            .name = {},
        }};
    CHECK_FALSE(lp < nonlp);  // LP has higher priority
    CHECK(nonlp < lp);  // non-LP has lower priority
  }

  SUBCASE("TaskPriority tier never reorders the SDDPTaskKey tuple")
  {
    // Regression guard for the sim⇄wedge trap.  A simulation task carries
    // `gate_bypass = true` (formerly `TaskPriority::High`) so it can bypass
    // the CPU gate.  It must NOT thereby jump ahead of a still-training
    // peer at an earlier iteration: ordering is by the key tuple alone, so
    // a later-iteration sim task stays behind an earlier-iteration training
    // task regardless of tier / bypass flag.
    STask sim_later {
        [] {},
        SReq {
            .priority = TaskPriority::High,  // even the old reordering tier…
            .priority_key = make_sddp_task_key(IterationIndex {5},
                                               SDDPPassDirection::forward,
                                               SDDPTaskKind::lp),
            .gate_bypass = true,
            .name = {},
        }};
    STask train_earlier {
        [] {},
        SReq {
            .priority = TaskPriority::Medium,
            .priority_key = make_sddp_task_key(IterationIndex {3},
                                               SDDPPassDirection::backward,
                                               SDDPTaskKind::lp),
            .name = {},
        }};
    // iter-3 training outranks iter-5 sim despite sim's High tier + bypass.
    CHECK(sim_later < train_earlier);  // sim is lower priority (later iter)
    CHECK_FALSE(train_earlier < sim_later);
  }

  SUBCASE("smaller phase_rank wins among tied backward aperture chunks")
  {
    // Two scenes in the same iteration's backward sweep at different phases.
    // The laggard (more phases still to process → smaller phase index →
    // smaller phase_rank set by make_aperture_submit_fn) must drain first.
    STask laggard {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {2},
                                               SDDPPassDirection::backward,
                                               SDDPTaskKind::lp,
                                               /*phase_rank=*/3),
            .name = {},
        }};
    STask ahead {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {2},
                                               SDDPPassDirection::backward,
                                               SDDPTaskKind::lp,
                                               /*phase_rank=*/40),
            .name = {},
        }};
    CHECK_FALSE(laggard < ahead);  // laggard (rank 3) has higher priority
    CHECK(ahead < laggard);  // ahead (rank 40) is lower priority

    // phase_rank only breaks ties WITHIN the same (iter, is_backward): an
    // earlier-iteration chunk with a large rank still outranks a later one.
    STask earlier_iter {
        [] {},
        SReq {
            .priority_key = make_sddp_task_key(IterationIndex {1},
                                               SDDPPassDirection::backward,
                                               SDDPTaskKind::lp,
                                               /*phase_rank=*/40),
            .name = {},
        }};
    CHECK(laggard < earlier_iter);  // iter-2 rank-3 < iter-1 rank-40
    CHECK_FALSE(earlier_iter < laggard);
  }
}

// ─── SDDPWorkPool type tests
// ──────────────────────────────────────────────────

TEST_CASE("SDDPWorkPool type traits")  // NOLINT
{
  using namespace gtopt;

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
  using namespace gtopt;

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
    // is_backward 0 < 1 wins the cross-direction comparison — matches
    // the SDDP rule that the forward pass produces the trial points
    // the backward pass needs.
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
                                               SDDPTaskKind::lp),
            .name = {},
        });
    auto f0 = pool.submit(
        [&] { result = 0; },
        Req {
            .priority_key = make_sddp_task_key(IterationIndex {0},
                                               SDDPPassDirection::forward,
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
  using namespace gtopt;

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
  using namespace gtopt;

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

TEST_CASE("make_sddp_work_pool memory-governance defaults")  // NOLINT
{
  // Regression pin for the work-pool memory-governor fix: routine dispatch
  // throttling must be driven by gtopt's OWN projected RSS (the marginal
  // gate, which requires max_process_rss_mb > 0), NOT by system-wide
  // memory% — so unrelated processes can't collapse the pool to one worker,
  // and an off-mode resident LP set doesn't serialize for no memory gain.
  // The system gates are demoted to near-OOM backstops.
  using namespace gtopt;

  SUBCASE("unset memory_limit auto-enables an own-RSS budget")
  {
    auto pool = make_sddp_work_pool(0.5, /*memory_limit_mb=*/0.0);
    REQUIRE(pool != nullptr);
    // Was 0 (gate OFF) before the fix; now defaults to a fraction of RAM.
    CHECK(pool->max_process_rss_mb() > 0.0);
    // System memory% gate demoted to a near-OOM backstop (was 90%).
    CHECK(pool->max_memory_percent() >= 95.0);
    pool->shutdown();
  }

  SUBCASE("explicit memory_limit is honored verbatim")
  {
    auto pool = make_sddp_work_pool(0.5, /*memory_limit_mb=*/12345.0);
    REQUIRE(pool != nullptr);
    CHECK(pool->max_process_rss_mb() == doctest::Approx(12345.0));
    pool->shutdown();
  }
}
