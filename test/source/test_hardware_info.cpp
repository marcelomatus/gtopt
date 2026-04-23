// SPDX-License-Identifier: BSD-3-Clause
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
