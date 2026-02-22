/**
 * @file      test_element_index.cpp
 * @brief     Unit tests for ElementIndex type-safe index
 * @date      Sat Feb  8 07:36:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Tests for the ElementIndex strongly-typed index wrapper
 */

#include <algorithm>
#include <ranges>
#include <type_traits>

#include <doctest/doctest.h>
#include <gtopt/element_index.hpp>

using namespace gtopt;

namespace
{
// Test element types for strong typing
struct TestElement1
{
  int value;
};

struct TestElement2
{
  double value;
};
}  // namespace

TEST_CASE("ElementIndex - Default construction")
{
  const ElementIndex<TestElement1> idx1;
  CHECK(idx1 == ElementIndex<TestElement1>::Unknown);
  CHECK(idx1 == unknown_index);
}

TEST_CASE("ElementIndex - Value construction")
{
  const ElementIndex<TestElement1> idx1 {5};
  CHECK(idx1 == 5);
  CHECK(idx1 != ElementIndex<TestElement1>::Unknown);

  const ElementIndex<TestElement1> idx2 {0};
  CHECK(idx2 == 0);
  CHECK(idx2 != ElementIndex<TestElement1>::Unknown);
}

TEST_CASE("ElementIndex - Copy and move semantics")
{
  SUBCASE("Copy construction")
  {
    const ElementIndex<TestElement1> idx1 {10};
    ElementIndex<TestElement1> idx2 {idx1};
    CHECK(idx1 == idx2);
    CHECK(idx2 == 10);
  }

  SUBCASE("Copy assignment")
  {
    const ElementIndex<TestElement1> idx1 {10};
    ElementIndex<TestElement1> idx2 {5};
    idx2 = idx1;
    CHECK(idx1 == idx2);
    CHECK(idx2 == 10);
  }

  SUBCASE("Move construction")
  {
    ElementIndex<TestElement1> idx1 {10};
    const ElementIndex<TestElement1> idx2 {std::move(  // NOLINT(hicpp-move-const-arg,performance-move-const-arg,hicpp-invalid-access-moved,bugprone-use-after-move)
        idx1)};
    CHECK(idx2 == 10);
  }

  SUBCASE("Move assignment")
  {
    ElementIndex<TestElement1> idx1 {10};
    ElementIndex<TestElement1> idx2 {5};
    idx2 = std::move(  // NOLINT(hicpp-move-const-arg,performance-move-const-arg,hicpp-invalid-access-moved,bugprone-use-after-move)
        idx1);
    CHECK(idx2 == 10);
  }
}

TEST_CASE("ElementIndex - Type safety")
{
  // These should be different types and not convertible
  static_assert(!std::is_convertible_v<ElementIndex<TestElement1>,
                                       ElementIndex<TestElement2>>,
                "ElementIndex should enforce type safety");

  static_assert(!std::is_convertible_v<ElementIndex<TestElement2>,
                                       ElementIndex<TestElement1>>,
                "ElementIndex should enforce type safety");

  // Can construct from size_t
  static_assert(std::is_constructible_v<ElementIndex<TestElement1>, size_t>,
                "ElementIndex should be constructible from size_t");
}

TEST_CASE("ElementIndex - Comparison operators")
{
  ElementIndex<TestElement1> idx1 {5};
  ElementIndex<TestElement1> idx2 {5};
  ElementIndex<TestElement1> idx3 {10};

  SUBCASE("Equality")
  {
    CHECK(idx1 == idx2);
    CHECK_FALSE(idx1 == idx3);
  }

  SUBCASE("Inequality")
  {
    CHECK(idx1 != idx3);
    CHECK_FALSE(idx1 != idx2);
  }

  SUBCASE("Less than")
  {
    CHECK(idx1 < idx3);
    CHECK_FALSE(idx3 < idx1);
    CHECK_FALSE(idx1 < idx2);
  }

  SUBCASE("Greater than")
  {
    CHECK(idx3 > idx1);
    CHECK_FALSE(idx1 > idx3);
    CHECK_FALSE(idx1 > idx2);
  }

  SUBCASE("Less than or equal")
  {
    CHECK(idx1 <= idx2);
    CHECK(idx1 <= idx3);
    CHECK_FALSE(idx3 <= idx1);
  }

  SUBCASE("Greater than or equal")
  {
    CHECK(idx1 >= idx2);
    CHECK(idx3 >= idx1);
    CHECK_FALSE(idx1 >= idx3);
  }
}

TEST_CASE("ElementIndex - Comparison with size_t")
{
  ElementIndex<TestElement1> idx {5};

  CHECK(idx == 5);
  CHECK(5 == idx);
  CHECK(idx != 10);
  CHECK(10 != idx);
  CHECK(idx < 10);
  CHECK(3 < idx);
  CHECK(idx > 3);
  CHECK(10 > idx);
}

TEST_CASE("ElementIndex - Unknown constant")
{
  constexpr auto unknown = ElementIndex<TestElement1>::Unknown;
  CHECK(unknown == unknown_index);

  const ElementIndex<TestElement1> idx;
  CHECK(idx == unknown);
}

TEST_CASE("ElementIndex - Constexpr support")
{
  // Test that ElementIndex can be used in constexpr contexts
  constexpr ElementIndex<TestElement1> idx1 {5};
  constexpr ElementIndex<TestElement1> idx2 {10};

  static_assert(idx1 == 5);
  static_assert(idx2 == 10);
  static_assert(idx1 < idx2);
  static_assert(idx1 != idx2);
}

TEST_CASE("ElementIndex - Conversion to size_t")
{
  const ElementIndex<TestElement1> idx {42};

  // Should be able to convert back to size_t
  auto value = static_cast<size_t>(idx);
  CHECK(value == 42);
}

TEST_CASE("ElementIndex - Use in containers")
{
  std::vector<ElementIndex<TestElement1>> indices;
  indices.emplace_back(0);
  indices.emplace_back(1);
  indices.emplace_back(2);

  CHECK(indices.size() == 3);
  CHECK(indices[0] == 0);
  CHECK(indices[1] == 1);
  CHECK(indices[2] == 2);
}

TEST_CASE("ElementIndex - Sorting")
{
  std::vector<ElementIndex<TestElement1>> indices {
      ElementIndex<TestElement1> {5},
      ElementIndex<TestElement1> {2},
      ElementIndex<TestElement1> {8},
      ElementIndex<TestElement1> {1},
  };

  std::ranges::sort(indices);

  CHECK(indices[0] == 1);
  CHECK(indices[1] == 2);
  CHECK(indices[2] == 5);
  CHECK(indices[3] == 8);
}
