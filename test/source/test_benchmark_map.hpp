/**
 * @file      test_benchmark_map.cpp
 * @brief     Benchmark comparing std::map vs flat_map
 * (boost::container::flat_map and std::flat_map when available)
 * @date      Tue Feb 11 16:43:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Measures insertion, iteration, random search, sorted search, and
 * flat_map reserve impact for:
 *   - Small maps (4, 8, 12 elements) with sorted and random integer keys
 *   - Large maps (~500 elements) with sorted and random integer keys
 *   - boost::flat_map vs std::flat_map reserve comparison (when available)
 */

#include <algorithm>
#include <chrono>
#include <map>
#include <numeric>
#include <random>
#include <string_view>
#include <vector>

#include <boost/container/flat_map.hpp>
#include <doctest/doctest.h>
#include <gtopt/fmap.hpp>

// this is only needed to mix std abd boost flat_map benchmarks in the same test
// suite; if boost::flat_map is not
#ifndef GTOPT_USE_BOOST_FLAT_MAP
namespace gtopt
{

template<typename key_type, typename value_type, typename Size>
void map_reserve(  // NOLINT
    boost::container::flat_map<key_type, value_type>& map,
    Size n)
{
  if (n == 0) {
    return;
  }
  map.reserve(n);
}

}  // namespace gtopt
#endif

namespace
{

using int64_t = std::int64_t;

constexpr int kWarmupIterations = 10;
constexpr int kSmallMapIterations = 1000;
constexpr int kLargeMapIterations = 100;

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

void report(std::string_view label, double std_map_ns, double flat_map_ns)
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

void report_reserve(std::string_view label,
                    double no_reserve_ns,
                    double reserved_ns)
{
  const double ratio = (reserved_ns > 0) ? (no_reserve_ns / reserved_ns) : 0.0;
  MESSAGE(label << ": no_reserve=" << no_reserve_ns << " ns, reserved="
                << reserved_ns << " ns, ratio(no_rsv/rsv)=" << ratio);
}

template<typename Map>
auto bench_insert_map_reserved(const std::vector<int>& keys, int iterations)
    -> double
{
  const auto n = keys.size();

  // warmup
  for (int i = 0; i < kWarmupIterations; ++i) {
    Map map;
    gtopt::map_reserve(map, n);
    insert_keys(map, keys);
  }

  auto start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iterations; ++i) {
    Map map;
    gtopt::map_reserve(map, n);
    insert_keys(map, keys);
  }
  auto end = std::chrono::high_resolution_clock::now();

  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
  return static_cast<double>(ns.count()) / iterations;
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
      report(("insert sorted n=" + std::to_string(n)), std_ns, flat_ns);
      CHECK(std_ns > 0);
      CHECK(flat_ns > 0);
    }

    SUBCASE(("iterate sorted n=" + std::to_string(n)).c_str())
    {
      auto std_ns = bench_iterate<StdMap>(keys, kSmallMapIterations);
      auto flat_ns = bench_iterate<FlatMap>(keys, kSmallMapIterations);
      report(("iterate sorted n=" + std::to_string(n)), std_ns, flat_ns);
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
      report(("insert random n=" + std::to_string(n)), std_ns, flat_ns);
      CHECK(std_ns > 0);
      CHECK(flat_ns > 0);
    }

    SUBCASE(("iterate random n=" + std::to_string(n)).c_str())
    {
      auto std_ns = bench_iterate<StdMap>(keys, kSmallMapIterations);
      auto flat_ns = bench_iterate<FlatMap>(keys, kSmallMapIterations);
      report(("iterate random n=" + std::to_string(n)), std_ns, flat_ns);
      CHECK(std_ns > 0);
      CHECK(flat_ns > 0);
    }
  }
}

TEST_CASE("Benchmark - large maps with sorted keys")
{
  constexpr int n = 500;
  auto keys = sorted_keys(n);

  SUBCASE("insert sorted n=500")
  {
    auto std_ns = bench_insert<StdMap>(keys, kLargeMapIterations);
    auto flat_ns = bench_insert<FlatMap>(keys, kLargeMapIterations);
    report("insert sorted n=500", std_ns, flat_ns);
    CHECK(std_ns > 0);
    CHECK(flat_ns > 0);
  }

  SUBCASE("iterate sorted n=500")
  {
    auto std_ns = bench_iterate<StdMap>(keys, kLargeMapIterations);
    auto flat_ns = bench_iterate<FlatMap>(keys, kLargeMapIterations);
    report("iterate sorted n=500", std_ns, flat_ns);
    CHECK(std_ns > 0);
    CHECK(flat_ns > 0);
  }
}

TEST_CASE("Benchmark - large maps with random keys")
{
  constexpr int n = 500;
  auto keys = random_keys(n);

  SUBCASE("insert random n=500")
  {
    auto std_ns = bench_insert<StdMap>(keys, kLargeMapIterations);
    auto flat_ns = bench_insert<FlatMap>(keys, kLargeMapIterations);
    report("insert random n=500", std_ns, flat_ns);
    CHECK(std_ns > 0);
    CHECK(flat_ns > 0);
  }

  SUBCASE("iterate random n=500")
  {
    auto std_ns = bench_iterate<StdMap>(keys, kLargeMapIterations);
    auto flat_ns = bench_iterate<FlatMap>(keys, kLargeMapIterations);
    report("iterate random n=500", std_ns, flat_ns);
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
      report(("search sorted n=" + std::to_string(n)), std_ns, flat_ns);
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
      report(("search random n=" + std::to_string(n)), std_ns, flat_ns);
      CHECK(std_ns > 0);
      CHECK(flat_ns > 0);
    }
  }
}

TEST_CASE("Benchmark - large maps random search with sorted keys")
{
  constexpr int n = 500;
  auto keys = sorted_keys(n);
  auto search_keys = random_keys(n);

  SUBCASE("search sorted n=500")
  {
    auto std_ns = bench_search<StdMap>(keys, search_keys, kLargeMapIterations);
    auto flat_ns =
        bench_search<FlatMap>(keys, search_keys, kLargeMapIterations);
    report("search sorted n=500", std_ns, flat_ns);
    CHECK(std_ns > 0);
    CHECK(flat_ns > 0);
  }
}

TEST_CASE("Benchmark - large maps random search with random keys")
{
  constexpr int n = 500;
  auto keys = random_keys(n);
  auto search_keys = random_keys(n);

  SUBCASE("search random n=500")
  {
    auto std_ns = bench_search<StdMap>(keys, search_keys, kLargeMapIterations);
    auto flat_ns =
        bench_search<FlatMap>(keys, search_keys, kLargeMapIterations);
    report("search random n=500", std_ns, flat_ns);
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
      report(("sorted search sorted n=" + std::to_string(n)), std_ns, flat_ns);
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
      report(("sorted search random n=" + std::to_string(n)), std_ns, flat_ns);
      CHECK(std_ns > 0);
      CHECK(flat_ns > 0);
    }
  }
}

TEST_CASE("Benchmark - large maps sorted search with sorted keys")
{
  constexpr int n = 500;
  auto keys = sorted_keys(n);
  auto search_keys = sorted_keys(n);

  SUBCASE("sorted search sorted n=500")
  {
    auto std_ns = bench_search<StdMap>(keys, search_keys, kLargeMapIterations);
    auto flat_ns =
        bench_search<FlatMap>(keys, search_keys, kLargeMapIterations);
    report("sorted search sorted n=500", std_ns, flat_ns);
    CHECK(std_ns > 0);
    CHECK(flat_ns > 0);
  }
}

TEST_CASE("Benchmark - large maps sorted search with random keys")
{
  constexpr int n = 500;
  auto keys = random_keys(n);
  auto search_keys = sorted_keys(n);

  SUBCASE("sorted search random n=500")
  {
    auto std_ns = bench_search<StdMap>(keys, search_keys, kLargeMapIterations);
    auto flat_ns =
        bench_search<FlatMap>(keys, search_keys, kLargeMapIterations);
    report("sorted search random n=500", std_ns, flat_ns);
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
          ("reserve sorted n=" + std::to_string(n)), no_rsv_ns, rsv_ns);
      CHECK(no_rsv_ns > 0);
      CHECK(rsv_ns > 0);
    }
  }
}

TEST_CASE("Benchmark - flat_map reserve effect with small random keys")
{
  for (const int n : {4, 8, 12}) {
    auto keys = random_keys(n);

    SUBCASE(("reserve random n=" + std::to_string(n)).c_str())
    {
      auto no_rsv_ns = bench_insert<FlatMap>(keys, kSmallMapIterations);
      auto rsv_ns = bench_insert_reserved<FlatMap>(keys, kSmallMapIterations);
      report_reserve(
          ("reserve random n=" + std::to_string(n)), no_rsv_ns, rsv_ns);
      CHECK(no_rsv_ns > 0);
      CHECK(rsv_ns > 0);
    }
  }
}

TEST_CASE("Benchmark - flat_map reserve effect with large sorted keys")
{
  constexpr int n = 500;
  auto keys = sorted_keys(n);

  SUBCASE("reserve sorted n=500")
  {
    auto no_rsv_ns = bench_insert<FlatMap>(keys, kLargeMapIterations);
    auto rsv_ns = bench_insert_reserved<FlatMap>(keys, kLargeMapIterations);
    report_reserve("reserve sorted n=500", no_rsv_ns, rsv_ns);
    CHECK(no_rsv_ns > 0);
    CHECK(rsv_ns > 0);
  }
}

TEST_CASE("Benchmark - flat_map reserve effect with large random keys")
{
  constexpr int n = 500;
  auto keys = random_keys(n);

  SUBCASE("reserve random n=500")
  {
    auto no_rsv_ns = bench_insert<FlatMap>(keys, kLargeMapIterations);
    auto rsv_ns = bench_insert_reserved<FlatMap>(keys, kLargeMapIterations);
    report_reserve("reserve random n=500", no_rsv_ns, rsv_ns);
    CHECK(no_rsv_ns > 0);
    CHECK(rsv_ns > 0);
  }
}

TEST_CASE("Benchmark - map_reserve for boost::flat_map")
{
  for (const int n : {4, 8, 12, 500}) {
    auto keys = random_keys(n);

    SUBCASE(("map_reserve random n=" + std::to_string(n)).c_str())
    {
      auto no_rsv_ns = bench_insert<FlatMap>(keys, kSmallMapIterations);
      auto rsv_ns =
          bench_insert_map_reserved<FlatMap>(keys, kSmallMapIterations);
      report_reserve(
          ("map_reserve random n=" + std::to_string(n)), no_rsv_ns, rsv_ns);
      CHECK(no_rsv_ns > 0);
      CHECK(rsv_ns > 0);
    }
  }
}

#if __has_include(<flat_map>)

#  include <flat_map>

using StdFlatMap = std::flat_map<int, int>;

namespace
{
void report_boost_vs_std(std::string_view label, double boost_ns, double std_ns)
{
  const double ratio = (std_ns > 0) ? (boost_ns / std_ns) : 0.0;
  MESSAGE(label << ": boost::flat_map=" << boost_ns << " ns, std::flat_map="
                << std_ns << " ns, ratio(boost/std)=" << ratio);
}
}  // namespace

TEST_CASE("Benchmark - std::flat_map vs boost::flat_map insert")
{
  for (const int n : {4, 8, 12, 500}) {
    auto keys = random_keys(n);
    const int iters = (n <= 12) ? kSmallMapIterations : kLargeMapIterations;

    SUBCASE(("insert random n=" + std::to_string(n)).c_str())
    {
      auto boost_ns = bench_insert<FlatMap>(keys, iters);
      auto std_ns = bench_insert<StdFlatMap>(keys, iters);
      report_boost_vs_std(
          ("insert random n=" + std::to_string(n)), boost_ns, std_ns);
      CHECK(boost_ns > 0);
      CHECK(std_ns > 0);
    }
  }
}

TEST_CASE("Benchmark - std::flat_map vs boost::flat_map iterate")
{
  for (const int n : {4, 8, 12, 500}) {
    auto keys = random_keys(n);
    const int iters = (n <= 12) ? kSmallMapIterations : kLargeMapIterations;

    SUBCASE(("iterate random n=" + std::to_string(n)).c_str())
    {
      auto boost_ns = bench_iterate<FlatMap>(keys, iters);
      auto std_ns = bench_iterate<StdFlatMap>(keys, iters);
      report_boost_vs_std(
          ("iterate random n=" + std::to_string(n)), boost_ns, std_ns);
      CHECK(boost_ns > 0);
      CHECK(std_ns > 0);
    }
  }
}

TEST_CASE("Benchmark - std::flat_map vs boost::flat_map search")
{
  for (const int n : {4, 8, 12, 500}) {
    auto keys = random_keys(n);
    auto search_keys = random_keys(n);
    const int iters = (n <= 12) ? kSmallMapIterations : kLargeMapIterations;

    SUBCASE(("search random n=" + std::to_string(n)).c_str())
    {
      auto boost_ns = bench_search<FlatMap>(keys, search_keys, iters);
      auto std_ns = bench_search<StdFlatMap>(keys, search_keys, iters);
      report_boost_vs_std(
          ("search random n=" + std::to_string(n)), boost_ns, std_ns);
      CHECK(boost_ns > 0);
      CHECK(std_ns > 0);
    }
  }
}

TEST_CASE("Benchmark - std::flat_map vs boost::flat_map reserve effect")
{
  for (const int n : {4, 8, 12, 500}) {
    auto keys = random_keys(n);
    const int iters = (n <= 12) ? kSmallMapIterations : kLargeMapIterations;

    SUBCASE(("reserve random n=" + std::to_string(n)).c_str())
    {
      auto boost_rsv_ns = bench_insert_reserved<FlatMap>(keys, iters);
      auto std_rsv_ns = bench_insert_map_reserved<StdFlatMap>(keys, iters);
      report_boost_vs_std(
          ("reserve random n=" + std::to_string(n)), boost_rsv_ns, std_rsv_ns);
      CHECK(boost_rsv_ns > 0);
      CHECK(std_rsv_ns > 0);
    }
  }
}

TEST_CASE("Benchmark - std::flat_map map_reserve effect")
{
  for (const int n : {4, 8, 12, 500}) {
    auto keys = random_keys(n);
    const int iters = (n <= 12) ? kSmallMapIterations : kLargeMapIterations;

    SUBCASE(("std::flat_map reserve random n=" + std::to_string(n)).c_str())
    {
      auto no_rsv_ns = bench_insert<StdFlatMap>(keys, iters);
      auto rsv_ns = bench_insert_map_reserved<StdFlatMap>(keys, iters);
      report_reserve(("std::flat_map reserve random n=" + std::to_string(n)),
                     no_rsv_ns,
                     rsv_ns);
      CHECK(no_rsv_ns > 0);
      CHECK(rsv_ns > 0);
    }
  }
}

#endif
