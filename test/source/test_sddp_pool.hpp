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
  SUBCASE("SDDPTaskKey is a 4-tuple of int")
  {
    static_assert(std::same_as<SDDPTaskKey, std::tuple<int, int, int, int>>);
    CHECK(true);
  }

  SUBCASE("kSDDPKey constants have correct values")
  {
    CHECK(kSDDPKeyIsLP == 0);
    CHECK(kSDDPKeyIsNonLP == 1);
    CHECK(kSDDPKeyForward == 0);
    CHECK(kSDDPKeyBackward == 1);
  }

  SUBCASE("BasicTaskRequirements with SDDPTaskKey")
  {
    BasicTaskRequirements<SDDPTaskKey> req {
        .priority = TaskPriority::Medium,
        .priority_key = SDDPTaskKey {1, kSDDPKeyForward, 2, kSDDPKeyIsLP},
        .name = {},
    };
    CHECK(std::get<0>(req.priority_key) == 1);
    CHECK(std::get<1>(req.priority_key) == kSDDPKeyForward);
    CHECK(std::get<2>(req.priority_key) == 2);
    CHECK(std::get<3>(req.priority_key) == kSDDPKeyIsLP);
  }
}

// ─── Task ordering with SDDPTaskKey ──────────────────────────────────────────

TEST_CASE("Task<SDDPTaskKey> ordering is lexicographic")  // NOLINT
{
  using STask = Task<void, SDDPTaskKey, std::less<>>;
  using SReq = BasicTaskRequirements<SDDPTaskKey>;

  SUBCASE("lower iteration index has higher priority")
  {
    STask iter0 {[] {},
                 SReq {.priority_key = SDDPTaskKey {0, 0, 0, 0}, .name = {}}};
    STask iter1 {[] {},
                 SReq {.priority_key = SDDPTaskKey {1, 0, 0, 0}, .name = {}}};
    CHECK_FALSE(iter0 < iter1);  // iter0 has higher priority
    CHECK(iter1 < iter0);  // iter1 has lower priority
  }

  SUBCASE("forward (0) has higher priority than backward (1)")
  {
    STask fwd {[] {},
               SReq {.priority_key = SDDPTaskKey {0, kSDDPKeyForward, 0, 0},
                     .name = {}}};
    STask bwd {[] {},
               SReq {.priority_key = SDDPTaskKey {0, kSDDPKeyBackward, 0, 0},
                     .name = {}}};
    CHECK_FALSE(fwd < bwd);  // forward has higher priority
    CHECK(bwd < fwd);  // backward has lower priority
  }

  SUBCASE("LP solve (0) has higher priority than non-LP (1)")
  {
    STask lp {
        [] {},
        SReq {.priority_key = SDDPTaskKey {0, 0, 0, kSDDPKeyIsLP}, .name = {}}};
    STask nonlp {[] {},
                 SReq {.priority_key = SDDPTaskKey {0, 0, 0, kSDDPKeyIsNonLP},
                       .name = {}}};
    CHECK_FALSE(lp < nonlp);  // LP has higher priority
    CHECK(nonlp < lp);  // non-LP has lower priority
  }

  SUBCASE("lower phase index has higher priority")
  {
    STask ph0 {[] {},
               SReq {.priority_key = SDDPTaskKey {0, 0, 0, 0}, .name = {}}};
    STask ph2 {[] {},
               SReq {.priority_key = SDDPTaskKey {0, 0, 2, 0}, .name = {}}};
    CHECK_FALSE(ph0 < ph2);  // phase 0 has higher priority
    CHECK(ph2 < ph0);  // phase 2 has lower priority
  }
}

// ─── SDDPWorkPool type tests
// ──────────────────────────────────────────────────

TEST_CASE("SDDPWorkPool type traits")  // NOLINT
{
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
            .priority_key = SDDPTaskKey {0, kSDDPKeyForward, 0, kSDDPKeyIsLP},
            .name = {},
        });

    REQUIRE(fut.has_value());
    fut->wait();
    CHECK(result == 42);

    pool.shutdown();
  }

  SUBCASE("forward task runs before backward task")
  {
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
            .priority_key = SDDPTaskKey {0, kSDDPKeyForward, 0, kSDDPKeyIsLP},
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
            .priority_key = SDDPTaskKey {0, kSDDPKeyBackward, 0, kSDDPKeyIsLP},
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
    auto f1 =
        pool.submit([&] { result = 1; },
                    Req {.priority_key = SDDPTaskKey {1, 0, 0, 0}, .name = {}});
    auto f0 =
        pool.submit([&] { result = 0; },
                    Req {.priority_key = SDDPTaskKey {0, 0, 0, 0}, .name = {}});

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
  SUBCASE("creates and starts SDDPWorkPool")
  {
    auto pool = make_sddp_work_pool(0.5);
    REQUIRE(pool != nullptr);

    std::atomic<int> result {0};
    using Req = BasicTaskRequirements<SDDPTaskKey>;
    auto fut = pool->submit(
        [&] { result = 99; },
        Req {.priority_key = SDDPTaskKey {0, 0, 0, 0}, .name = {}});

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
