/**
 * @file      test_benchmark_map.cpp
 * @brief     Benchmark comparing std::map vs flat_map
 * (boost::container::flat_map)
 * @date      Tue Feb 11 16:43:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Measures insertion, iteration, random search, sorted search, and
 * flat_map reserve impact for:
 *   - Small maps (4, 8, 12 elements) with sorted and random integer keys
 *   - Large maps (~1000 elements) with sorted and random integer keys
 */

#include <algorithm>
#include <chrono>
#include <map>
#include <numeric>
#include <random>
#include <vector>

#include <boost/container/flat_map.hpp>
#include <doctest/doctest.h>

namespace
{

using int64_t = std::int64_t;

constexpr int kWarmupIterations = 100;
constexpr int kSmallMapIterations = 10000;
constexpr int kLargeMapIterations = 1000;

auto sorted_keys(int n) -> std::vector<int>
{
  std::vector<int> keys(static_cast<size_t>(n));
  std::ranges::iota(keys.begin(), keys.end(), 0);  // NOLINT
  return keys;
}

auto random_keys(int n) -> std::vector<int>
{
  auto keys = sorted_keys(n);
  std::mt19937 gen(42);  // NOLINT fixed seed for reproducibility
  std::ranges::shuffle(keys, gen);
  return keys;
}

template<typename Map>
void insert_keys(Map& map, const std::vector<int>& keys)
{
  for (auto key : keys) {
    map[key] = key;
  }
}

template<typename Map>
auto iterate_sum(const Map& map) -> int64_t
{
  int64_t sum = 0;
  for (const auto& [key, value] : map) {
    sum += value;
  }
  return sum;
}

template<typename Map>
auto bench_insert(const std::vector<int>& keys, int iterations) -> double
{
  // warmup
  for (int i = 0; i < kWarmupIterations; ++i) {
    Map map;
    insert_keys(map, keys);
  }

  auto start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iterations; ++i) {
    Map map;
    insert_keys(map, keys);
  }
  auto end = std::chrono::high_resolution_clock::now();

  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
  return static_cast<double>(ns.count()) / iterations;
}

template<typename Map>
auto bench_iterate(const std::vector<int>& keys, int iterations) -> double
{
  Map map;
  insert_keys(map, keys);

  // warmup
  volatile int64_t sink = 0;
  for (int i = 0; i < kWarmupIterations; ++i) {
    sink = iterate_sum(map);
  }

  auto start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iterations; ++i) {
    sink = iterate_sum(map);
  }
  auto end = std::chrono::high_resolution_clock::now();

  (void)sink;
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
  return static_cast<double>(ns.count()) / iterations;
}

template<typename Map>
auto bench_search(const std::vector<int>& keys,
                  const std::vector<int>& search_keys,
                  int iterations) -> double
{
  Map map;
  insert_keys(map, keys);

  // warmup
  volatile int64_t sink = 0;
  for (int i = 0; i < kWarmupIterations; ++i) {
    for (auto key : search_keys) {
      auto it = map.find(key);
      if (it != map.end()) {
        sink = it->second;
      }
    }
  }

  auto start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iterations; ++i) {
    for (auto key : search_keys) {
      auto it = map.find(key);
      if (it != map.end()) {
        sink = it->second;
      }
    }
  }
  auto end = std::chrono::high_resolution_clock::now();

  (void)sink;
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
  return static_cast<double>(ns.count()) / iterations;
}

using StdMap = std::map<int, int>;
using FlatMap = boost::container::flat_map<int, int>;

void report(const char* label, double std_map_ns, double flat_map_ns)
{
  const double ratio = (flat_map_ns > 0) ? (std_map_ns / flat_map_ns) : 0.0;
  MESSAGE(label << ": std::map=" << std_map_ns << " ns, flat_map="
                << flat_map_ns << " ns, ratio(std/flat)=" << ratio);
}

template<typename Map>
auto bench_insert_reserved(const std::vector<int>& keys, int iterations)
    -> double
{
  const auto n = keys.size();

  // warmup
  for (int i = 0; i < kWarmupIterations; ++i) {
    Map map;
    map.reserve(n);
    insert_keys(map, keys);
  }

  auto start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iterations; ++i) {
    Map map;
    map.reserve(n);
    insert_keys(map, keys);
  }
  auto end = std::chrono::high_resolution_clock::now();

  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
  return static_cast<double>(ns.count()) / iterations;
}

void report_reserve(const char* label, double no_reserve_ns, double reserved_ns)
{
  const double ratio = (reserved_ns > 0) ? (no_reserve_ns / reserved_ns) : 0.0;
  MESSAGE(label << ": no_reserve=" << no_reserve_ns << " ns, reserved="
                << reserved_ns << " ns, ratio(no_rsv/rsv)=" << ratio);
}

}  // namespace

TEST_CASE("Benchmark - small maps with sorted keys")
{
  for (const int n : {4, 8, 12}) {
    auto keys = sorted_keys(n);

    SUBCASE(("insert sorted n=" + std::to_string(n)).c_str())
    {
      auto std_ns = bench_insert<StdMap>(keys, kSmallMapIterations);
      auto flat_ns = bench_insert<FlatMap>(keys, kSmallMapIterations);
      report(("insert sorted n=" + std::to_string(n)).c_str(), std_ns, flat_ns);
      CHECK(std_ns > 0);
      CHECK(flat_ns > 0);
    }

    SUBCASE(("iterate sorted n=" + std::to_string(n)).c_str())
    {
      auto std_ns = bench_iterate<StdMap>(keys, kSmallMapIterations);
      auto flat_ns = bench_iterate<FlatMap>(keys, kSmallMapIterations);
      report(
          ("iterate sorted n=" + std::to_string(n)).c_str(), std_ns, flat_ns);
      CHECK(std_ns > 0);
      CHECK(flat_ns > 0);
    }
  }
}

TEST_CASE("Benchmark - small maps with random keys")
{
  for (const int n : {4, 8, 12}) {
    auto keys = random_keys(n);

    SUBCASE(("insert random n=" + std::to_string(n)).c_str())
    {
      auto std_ns = bench_insert<StdMap>(keys, kSmallMapIterations);
      auto flat_ns = bench_insert<FlatMap>(keys, kSmallMapIterations);
      report(("insert random n=" + std::to_string(n)).c_str(), std_ns, flat_ns);
      CHECK(std_ns > 0);
      CHECK(flat_ns > 0);
    }

    SUBCASE(("iterate random n=" + std::to_string(n)).c_str())
    {
      auto std_ns = bench_iterate<StdMap>(keys, kSmallMapIterations);
      auto flat_ns = bench_iterate<FlatMap>(keys, kSmallMapIterations);
      report(
          ("iterate random n=" + std::to_string(n)).c_str(), std_ns, flat_ns);
      CHECK(std_ns > 0);
      CHECK(flat_ns > 0);
    }
  }
}

TEST_CASE("Benchmark - large maps with sorted keys")
{
  constexpr int n = 1000;
  auto keys = sorted_keys(n);

  SUBCASE("insert sorted n=1000")
  {
    auto std_ns = bench_insert<StdMap>(keys, kLargeMapIterations);
    auto flat_ns = bench_insert<FlatMap>(keys, kLargeMapIterations);
    report("insert sorted n=1000", std_ns, flat_ns);
    CHECK(std_ns > 0);
    CHECK(flat_ns > 0);
  }

  SUBCASE("iterate sorted n=1000")
  {
    auto std_ns = bench_iterate<StdMap>(keys, kLargeMapIterations);
    auto flat_ns = bench_iterate<FlatMap>(keys, kLargeMapIterations);
    report("iterate sorted n=1000", std_ns, flat_ns);
    CHECK(std_ns > 0);
    CHECK(flat_ns > 0);
  }
}

TEST_CASE("Benchmark - large maps with random keys")
{
  constexpr int n = 1000;
  auto keys = random_keys(n);

  SUBCASE("insert random n=1000")
  {
    auto std_ns = bench_insert<StdMap>(keys, kLargeMapIterations);
    auto flat_ns = bench_insert<FlatMap>(keys, kLargeMapIterations);
    report("insert random n=1000", std_ns, flat_ns);
    CHECK(std_ns > 0);
    CHECK(flat_ns > 0);
  }

  SUBCASE("iterate random n=1000")
  {
    auto std_ns = bench_iterate<StdMap>(keys, kLargeMapIterations);
    auto flat_ns = bench_iterate<FlatMap>(keys, kLargeMapIterations);
    report("iterate random n=1000", std_ns, flat_ns);
    CHECK(std_ns > 0);
    CHECK(flat_ns > 0);
  }
}

TEST_CASE("Benchmark - small maps random search with sorted keys")
{
  for (const int n : {4, 8, 12}) {
    auto keys = sorted_keys(n);
    auto search_keys = random_keys(n);

    SUBCASE(("search sorted n=" + std::to_string(n)).c_str())
    {
      auto std_ns =
          bench_search<StdMap>(keys, search_keys, kSmallMapIterations);
      auto flat_ns =
          bench_search<FlatMap>(keys, search_keys, kSmallMapIterations);
      report(("search sorted n=" + std::to_string(n)).c_str(), std_ns, flat_ns);
      CHECK(std_ns > 0);
      CHECK(flat_ns > 0);
    }
  }
}

TEST_CASE("Benchmark - small maps random search with random keys")
{
  for (const int n : {4, 8, 12}) {
    auto keys = random_keys(n);
    auto search_keys = random_keys(n);

    SUBCASE(("search random n=" + std::to_string(n)).c_str())
    {
      auto std_ns =
          bench_search<StdMap>(keys, search_keys, kSmallMapIterations);
      auto flat_ns =
          bench_search<FlatMap>(keys, search_keys, kSmallMapIterations);
      report(("search random n=" + std::to_string(n)).c_str(), std_ns, flat_ns);
      CHECK(std_ns > 0);
      CHECK(flat_ns > 0);
    }
  }
}

TEST_CASE("Benchmark - large maps random search with sorted keys")
{
  constexpr int n = 1000;
  auto keys = sorted_keys(n);
  auto search_keys = random_keys(n);

  SUBCASE("search sorted n=1000")
  {
    auto std_ns = bench_search<StdMap>(keys, search_keys, kLargeMapIterations);
    auto flat_ns =
        bench_search<FlatMap>(keys, search_keys, kLargeMapIterations);
    report("search sorted n=1000", std_ns, flat_ns);
    CHECK(std_ns > 0);
    CHECK(flat_ns > 0);
  }
}

TEST_CASE("Benchmark - large maps random search with random keys")
{
  constexpr int n = 1000;
  auto keys = random_keys(n);
  auto search_keys = random_keys(n);

  SUBCASE("search random n=1000")
  {
    auto std_ns = bench_search<StdMap>(keys, search_keys, kLargeMapIterations);
    auto flat_ns =
        bench_search<FlatMap>(keys, search_keys, kLargeMapIterations);
    report("search random n=1000", std_ns, flat_ns);
    CHECK(std_ns > 0);
    CHECK(flat_ns > 0);
  }
}

TEST_CASE("Benchmark - small maps sorted search with sorted keys")
{
  for (const int n : {4, 8, 12}) {
    auto keys = sorted_keys(n);
    auto search_keys = sorted_keys(n);

    SUBCASE(("sorted search sorted n=" + std::to_string(n)).c_str())
    {
      auto std_ns =
          bench_search<StdMap>(keys, search_keys, kSmallMapIterations);
      auto flat_ns =
          bench_search<FlatMap>(keys, search_keys, kSmallMapIterations);
      report(("sorted search sorted n=" + std::to_string(n)).c_str(),
             std_ns,
             flat_ns);
      CHECK(std_ns > 0);
      CHECK(flat_ns > 0);
    }
  }
}

TEST_CASE("Benchmark - small maps sorted search with random keys")
{
  for (const int n : {4, 8, 12}) {
    auto keys = random_keys(n);
    auto search_keys = sorted_keys(n);

    SUBCASE(("sorted search random n=" + std::to_string(n)).c_str())
    {
      auto std_ns =
          bench_search<StdMap>(keys, search_keys, kSmallMapIterations);
      auto flat_ns =
          bench_search<FlatMap>(keys, search_keys, kSmallMapIterations);
      report(("sorted search random n=" + std::to_string(n)).c_str(),
             std_ns,
             flat_ns);
      CHECK(std_ns > 0);
      CHECK(flat_ns > 0);
    }
  }
}

TEST_CASE("Benchmark - large maps sorted search with sorted keys")
{
  constexpr int n = 1000;
  auto keys = sorted_keys(n);
  auto search_keys = sorted_keys(n);

  SUBCASE("sorted search sorted n=1000")
  {
    auto std_ns = bench_search<StdMap>(keys, search_keys, kLargeMapIterations);
    auto flat_ns =
        bench_search<FlatMap>(keys, search_keys, kLargeMapIterations);
    report("sorted search sorted n=1000", std_ns, flat_ns);
    CHECK(std_ns > 0);
    CHECK(flat_ns > 0);
  }
}

TEST_CASE("Benchmark - large maps sorted search with random keys")
{
  constexpr int n = 1000;
  auto keys = random_keys(n);
  auto search_keys = sorted_keys(n);

  SUBCASE("sorted search random n=1000")
  {
    auto std_ns = bench_search<StdMap>(keys, search_keys, kLargeMapIterations);
    auto flat_ns =
        bench_search<FlatMap>(keys, search_keys, kLargeMapIterations);
    report("sorted search random n=1000", std_ns, flat_ns);
    CHECK(std_ns > 0);
    CHECK(flat_ns > 0);
  }
}

TEST_CASE("Benchmark - flat_map reserve effect with small sorted keys")
{
  for (const int n : {4, 8, 12}) {
    auto keys = sorted_keys(n);

    SUBCASE(("reserve sorted n=" + std::to_string(n)).c_str())
    {
      auto no_rsv_ns = bench_insert<FlatMap>(keys, kSmallMapIterations);
      auto rsv_ns = bench_insert_reserved<FlatMap>(keys, kSmallMapIterations);
      report_reserve(
          ("reserve sorted n=" + std::to_string(n)).c_str(), no_rsv_ns, rsv_ns);
      CHECK(no_rsv_ns > 0);
      CHECK(rsv_ns > 0);
    }
  }
}

TEST_CASE("Benchmark - flat_map reserve effect with small random keys")
{
  constexpr int n = 1000;
  auto keys = random_keys(n);

  SUBCASE(("reserve random n=" + std::to_string(n)).c_str())
  {
    auto no_rsv_ns = bench_insert<FlatMap>(keys, kSmallMapIterations);
    auto rsv_ns = bench_insert_reserved<FlatMap>(keys, kSmallMapIterations);
    report_reserve(
        ("reserve random n=" + std::to_string(n)).c_str(), no_rsv_ns, rsv_ns);
    CHECK(no_rsv_ns > 0);
    CHECK(rsv_ns > 0);
  }
}

TEST_CASE("Benchmark - flat_map reserve effect with large sorted keys")
{
  constexpr int n = 1000;
  auto keys = sorted_keys(n);

  SUBCASE("reserve sorted n=1000")
  {
    auto no_rsv_ns = bench_insert<FlatMap>(keys, kLargeMapIterations);
    auto rsv_ns = bench_insert_reserved<FlatMap>(keys, kLargeMapIterations);
    report_reserve("reserve sorted n=1000", no_rsv_ns, rsv_ns);
    CHECK(no_rsv_ns > 0);
    CHECK(rsv_ns > 0);
  }
}

TEST_CASE("Benchmark - flat_map reserve effect with large random keys")
{
  constexpr int n = 1000;
  auto keys = random_keys(n);

  SUBCASE("reserve random n=1000")
  {
    auto no_rsv_ns = bench_insert<FlatMap>(keys, kLargeMapIterations);
    auto rsv_ns = bench_insert_reserved<FlatMap>(keys, kLargeMapIterations);
    report_reserve("reserve random n=1000", no_rsv_ns, rsv_ns);
    CHECK(no_rsv_ns > 0);
    CHECK(rsv_ns > 0);
  }
}
