// SPDX-License-Identifier: BSD-3-Clause
#include <cmath>
#include <thread>

#include <doctest/doctest.h>
#include <gtopt/hardware_info.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("physical_concurrency returns a positive value")  // NOLINT
{
  const auto phys = physical_concurrency();
  CHECK(phys > 0);
}

TEST_CASE("physical_concurrency <= hardware_concurrency")  // NOLINT
{
  const auto phys = physical_concurrency();
  const auto logical = std::thread::hardware_concurrency();
  CHECK(phys <= logical);
}

TEST_CASE("smt_ratio is at least 1")  // NOLINT
{
  const auto ratio = smt_ratio();
  CHECK(ratio >= 1);
}

TEST_CASE("smt_ratio * physical_concurrency ~ hardware_concurrency")  // NOLINT
{
  const auto phys = physical_concurrency();
  const auto logical = std::thread::hardware_concurrency();
  const auto ratio = smt_ratio();

  // The product should approximate the logical count within rounding
  const auto product = ratio * phys;
  CHECK(product >= logical / 2);
  CHECK(product <= logical * 2);
}

TEST_CASE("physical_concurrency is consistent across calls")  // NOLINT
{
  const auto first = physical_concurrency();
  const auto second = physical_concurrency();
  CHECK(first == second);
}

TEST_CASE(
    "detected_physical_concurrency is stable and matches default")  // NOLINT
{
  // No quota set: physical_concurrency() must equal the detected value.
  set_cpu_quota_pct(0.0);  // ensure clean state
  const auto detected = detected_physical_concurrency();
  CHECK(detected > 0);
  CHECK(physical_concurrency() == detected);
  CHECK(get_cpu_quota_pct() == 0.0);
}

TEST_CASE("set_cpu_quota_pct clamps physical_concurrency")  // NOLINT
{
  const auto detected = detected_physical_concurrency();
  REQUIRE(detected > 0);

  SUBCASE("30% clamps to ceil(detected * 0.30), at least 1")
  {
    set_cpu_quota_pct(30.0);
    const auto expected = std::max(
        1U,
        static_cast<unsigned>(std::ceil(static_cast<double>(detected) * 0.30)));
    CHECK(physical_concurrency() == expected);
    CHECK(get_cpu_quota_pct() == doctest::Approx(30.0));
    // detected_physical_concurrency() must NOT change.
    CHECK(detected_physical_concurrency() == detected);
  }

  SUBCASE("50% on a multi-core box yields a clamped count")
  {
    set_cpu_quota_pct(50.0);
    const auto expected = std::max(
        1U,
        static_cast<unsigned>(std::ceil(static_cast<double>(detected) * 0.50)));
    CHECK(physical_concurrency() == expected);
    CHECK(physical_concurrency() <= detected);
  }

  SUBCASE("Tiny percentages floor to at least 1 thread")
  {
    set_cpu_quota_pct(0.001);
    CHECK(physical_concurrency() >= 1);
    CHECK(get_cpu_quota_pct() == doctest::Approx(0.001));
  }

  // Restore default for subsequent test cases.
  set_cpu_quota_pct(0.0);
  CHECK(physical_concurrency() == detected);
}

TEST_CASE("set_cpu_quota_pct rejects out-of-range values")  // NOLINT
{
  const auto detected = detected_physical_concurrency();
  set_cpu_quota_pct(0.0);  // start clean

  SUBCASE("Zero disables clamping")
  {
    set_cpu_quota_pct(50.0);
    REQUIRE(physical_concurrency() != 0);
    set_cpu_quota_pct(0.0);
    CHECK(physical_concurrency() == detected);
    CHECK(get_cpu_quota_pct() == 0.0);
  }

  SUBCASE("Negative values are ignored")
  {
    set_cpu_quota_pct(50.0);  // active clamp
    set_cpu_quota_pct(-10.0);  // should reset
    CHECK(physical_concurrency() == detected);
    CHECK(get_cpu_quota_pct() == 0.0);
  }

  SUBCASE("Values >= 100 are ignored (no clamp)")
  {
    set_cpu_quota_pct(50.0);
    set_cpu_quota_pct(100.0);
    CHECK(physical_concurrency() == detected);
    CHECK(get_cpu_quota_pct() == 0.0);

    set_cpu_quota_pct(150.0);
    CHECK(physical_concurrency() == detected);
    CHECK(get_cpu_quota_pct() == 0.0);
  }

  SUBCASE("NaN is ignored")
  {
    set_cpu_quota_pct(50.0);
    set_cpu_quota_pct(std::nan(""));
    CHECK(physical_concurrency() == detected);
    CHECK(get_cpu_quota_pct() == 0.0);
  }
}

TEST_CASE("smt_ratio uses detected count, ignores CPU quota")  // NOLINT
{
  set_cpu_quota_pct(0.0);
  const auto ratio_before = smt_ratio();

  set_cpu_quota_pct(25.0);
  const auto ratio_after = smt_ratio();
  CHECK(ratio_after == ratio_before);

  set_cpu_quota_pct(0.0);  // restore
}
