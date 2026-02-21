/**
 * @file      test_fmap.cpp
 * @brief     Unit tests for flat_map wrapper
 * @date      Sat Feb  8 07:38:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Tests for the flat_map type alias and tuple_hash (when applicable)
 */

#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/fmap.hpp>

using namespace gtopt;

TEST_CASE("flat_map - Basic operations")
{
  flat_map<int, std::string> map;

  SUBCASE("Empty map")
  {
    CHECK(map.empty());
    CHECK(map.size() == 0);
  }

  SUBCASE("Insert and find")
  {
    map[1] = "one";
    map[2] = "two";
    map[3] = "three";

    CHECK(map.size() == 3);
    CHECK(map[1] == "one");
    CHECK(map[2] == "two");
    CHECK(map[3] == "three");

    auto it = map.find(2);
    CHECK(it != map.end());
    CHECK(it->second == "two");

    auto it_missing = map.find(99);
    CHECK(it_missing == map.end());
  }

  SUBCASE("Erase")
  {
    map[1] = "one";
    map[2] = "two";

    CHECK(map.size() == 2);

    map.erase(1);
    CHECK(map.size() == 1);
    CHECK(map.find(1) == map.end());
    CHECK(map.find(2) != map.end());
  }

  SUBCASE("Clear")
  {
    map[1] = "one";
    map[2] = "two";

    CHECK_FALSE(map.empty());
    map.clear();
    CHECK(map.empty());
  }
}

TEST_CASE("flat_map - String keys")
{
  flat_map<std::string, int> map;

  map["first"] = 1;
  map["second"] = 2;
  map["third"] = 3;

  CHECK(map.size() == 3);
  CHECK(map["first"] == 1);
  CHECK(map["second"] == 2);
  CHECK(map["third"] == 3);

  auto it = map.find("second");
  CHECK(it != map.end());
  CHECK(it->second == 2);
}

TEST_CASE("flat_map - Iteration")
{
  flat_map<int, std::string> map;
  map[1] = "one";
  map[2] = "two";
  map[3] = "three";

  size_t count = 0;
  for (const auto& [key, value] : map) {
    CHECK(key >= 1);
    CHECK(key <= 3);
    CHECK_FALSE(value.empty());
    ++count;
  }

  CHECK(count == 3);
}

TEST_CASE("flat_map - Structured bindings")
{
  flat_map<int, std::string> map;

  auto [it, inserted] = map.emplace(1, "one");
  CHECK(inserted);
  CHECK(it->first == 1);
  CHECK(it->second == "one");

  auto [it2, inserted2] = map.emplace(1, "one_duplicate");
  CHECK_FALSE(inserted2);  // Key already exists
  CHECK(it2->first == 1);
  CHECK(it2->second == "one");  // Original value unchanged
}

TEST_CASE("flat_map - Copy and move semantics")
{
  flat_map<int, std::string> map1;
  map1[1] = "one";
  map1[2] = "two";

  SUBCASE("Copy construction")
  {
    flat_map<int, std::string> map2 {map1};
    CHECK(map2.size() == 2);
    CHECK(map2[1] == "one");
    CHECK(map2[2] == "two");
  }

  SUBCASE("Copy assignment")
  {
    flat_map<int, std::string> map2;
    map2[99] = "ninety-nine";
    map2 = map1;
    CHECK(map2.size() == 2);
    CHECK(map2[1] == "one");
    CHECK(map2[2] == "two");
  }

  SUBCASE("Move construction")
  {
    flat_map<int, std::string> map2 {std::move(map1)};
    CHECK(map2.size() == 2);
    CHECK(map2[1] == "one");
    CHECK(map2[2] == "two");

    map1 = map2;
  }

  SUBCASE("Move assignment")
  {
    flat_map<int, std::string> map2;
    map2 = std::move(map1);
    CHECK(map2.size() == 2);
    CHECK(map2[1] == "one");
    CHECK(map2[2] == "two");
  }
}

TEST_CASE("flat_map - Complex value types")
{
  struct ComplexValue
  {
    std::string name {};
    int value {};
    std::vector<double> data {};

    auto operator==(const ComplexValue& other) const -> bool
    {
      return name == other.name && value == other.value && data == other.data;
    }
  };

  flat_map<int, ComplexValue> map;

  map[1] = ComplexValue {.name = "first", .value = 10, .data = {1.0, 2.0}};
  map[2] = ComplexValue {.name = "second", .value = 20, .data = {3.0, 4.0}};

  CHECK(map.size() == 2);
  CHECK(map[1].name == "first");
  CHECK(map[1].value == 10);
  CHECK(map[1].data.size() == 2);
  CHECK(map[2].name == "second");
  CHECK(map[2].value == 20);
  CHECK(map[2].data.size() == 2);
}

TEST_CASE("flat_map - Tuple keys")
{
  using TupleKey = std::tuple<int, std::string>;
  flat_map<TupleKey, double> map;

  const TupleKey key1 {1, "first"};
  const TupleKey key2 {2, "second"};
  const TupleKey key3 {1, "different"};

  map[key1] = 1.5;
  map[key2] = 2.5;
  map[key3] = 3.5;

  CHECK(map.size() == 3);
  CHECK(map[key1] == 1.5);
  CHECK(map[key2] == 2.5);
  CHECK(map[key3] == 3.5);

  auto it = map.find(key1);
  CHECK(it != map.end());
  CHECK(it->second == 1.5);
}

TEST_CASE("flat_map - Pair keys")
{
  using PairKey = std::pair<int, int>;
  flat_map<PairKey, std::string> map;

  map[{1, 2}] = "pair_1_2";
  map[{3, 4}] = "pair_3_4";
  map[{5, 6}] = "pair_5_6";

  CHECK(map.size() == 3);
  CHECK(map[{1, 2}] == "pair_1_2");
  CHECK(map[{3, 4}] == "pair_3_4");
  CHECK(map[{5, 6}] == "pair_5_6");
}

#ifdef GTOPT_USE_UNORDERED_MAP
TEST_CASE("flat_map (unordered) - tuple_hash functionality")
{
  // This test specifically validates tuple_hash when using unordered_map
  using TupleKey = std::tuple<int, std::string, double>;
  flat_map<TupleKey, int> map;

  TupleKey key1 {1, "test", 3.14};
  TupleKey key2 {2, "test", 3.14};
  TupleKey key3 {1, "different", 3.14};

  map[key1] = 100;
  map[key2] = 200;
  map[key3] = 300;

  CHECK(map.size() == 3);

  // Verify hash function produces consistent results
  hash_type hasher;
  auto hash1a = hasher(key1);
  auto hash1b = hasher(key1);
  CHECK(hash1a == hash1b);

  // Different keys should (likely) produce different hashes
  auto hash2 = hasher(key2);
  auto hash3 = hasher(key3);
  CHECK(hash1a != hash2);
  CHECK(hash1a != hash3);
}

TEST_CASE("flat_map (unordered) - tuple_hash with different types")
{
  SUBCASE("Simple tuple")
  {
    hash_type hasher;
    auto hash = hasher(std::tuple {1, 2, 3});
    CHECK(hash != 0);  // Should produce non-zero hash
  }

  SUBCASE("String tuple")
  {
    hash_type hasher;
    auto hash = hasher(std::tuple {std::string("hello"), std::string("world")});
    CHECK(hash != 0);
  }

  SUBCASE("Mixed type tuple")
  {
    hash_type hasher;
    auto hash = hasher(std::tuple {42, "mixed", 3.14, 'c'});
    CHECK(hash != 0);
  }
}
#endif

TEST_CASE("flat_map - map_reserve on empty map")
{
  flat_map<int, std::string> map;

  map_reserve(map, 100);
  CHECK(map.empty());

  map[1] = "one";
  map[2] = "two";
  map[3] = "three";

  CHECK(map.size() == 3);
  CHECK(map[1] == "one");
  CHECK(map[2] == "two");
  CHECK(map[3] == "three");
}

TEST_CASE("flat_map - map_reserve on non-empty map")
{
  flat_map<int, std::string> map;

  map[1] = "one";
  map[2] = "two";

  map_reserve(map, 100);

  CHECK(map.size() == 2);
  CHECK(map[1] == "one");
  CHECK(map[2] == "two");

  map[3] = "three";
  map[4] = "four";

  CHECK(map.size() == 4);
  CHECK(map[3] == "three");
  CHECK(map[4] == "four");
}

TEST_CASE("flat_map - map_reserve with zero")
{
  flat_map<int, std::string> map;

  map_reserve(map, 0);
  CHECK(map.empty());

  map[1] = "one";
  CHECK(map.size() == 1);
  CHECK(map[1] == "one");
}

TEST_CASE("flat_map - map_reserve with zero on non-empty map")
{
  flat_map<int, std::string> map;
  map[1] = "one";
  map[2] = "two";

  map_reserve(map, 0);

  CHECK(map.size() == 2);
  CHECK(map[1] == "one");
  CHECK(map[2] == "two");

  map[3] = "three";
  CHECK(map.size() == 3);
  CHECK(map[3] == "three");
}

TEST_CASE("flat_map - map_reserve preserves order and lookup")
{
  flat_map<int, int> map;

  for (int i = 0; i < 50; ++i) {
    map[i * 2] = i;
  }

  map_reserve(map, 200);

  CHECK(map.size() == 50);
  for (int i = 0; i < 50; ++i) {
    auto it = map.find(i * 2);
    CHECK(it != map.end());
    CHECK(it->second == i);
  }
}

TEST_CASE("flat_map - Count and contains")
{
  flat_map<int, std::string> map;
  map[1] = "one";
  map[2] = "two";

  CHECK(map.count(1) == 1);
  CHECK(map.count(2) == 1);
  CHECK(map.count(99) == 0);

#if defined(__cpp_lib_generic_unordered_lookup) \
    && __cpp_lib_generic_unordered_lookup >= 201811L
  // C++20 feature: contains()
  if constexpr (requires { map.contains(1); }) {
    CHECK(map.contains(1));
    CHECK(map.contains(2));
    CHECK_FALSE(map.contains(99));
  }
#endif
}
