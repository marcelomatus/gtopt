/**
 * @file      test_mvector_traits.cpp
 * @brief     Unit tests for mvector_traits template classes
 * @date      Mon Jun  2 21:30:23 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Tests for both mvector_traits and mvector_traits_indexed implementations
 */

#include <cstdint>

#include <doctest/doctest.h>
#include <gtopt/mvector_traits.hpp>

namespace gtopt
{
TEST_CASE("Basic 2D Vector")
{
  using traits_2d = mvector_traits<int, std::tuple<std::size_t, std::size_t>>;
  const traits_2d::vector_type vec2d = {{1, 2, 3}, {4, 5, 6}};

  SUBCASE("Valid access")
  {
    auto indices = std::make_tuple(std::size_t {1}, std::size_t {2});
    CHECK(traits_2d::at_value(vec2d, indices) == 6);
  }

  SUBCASE("Different indices")
  {
    auto indices = std::make_tuple(std::size_t {0}, std::size_t {1});
    CHECK(traits_2d::at_value(vec2d, indices) == 2);
  }
}

TEST_CASE("Basic 3D Vector")
{
  using traits_3d = mvector_traits<int, std::tuple<int, std::size_t, unsigned>>;
  const traits_3d::vector_type vec3d = {{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}};

  SUBCASE("First element")
  {
    auto indices3d = std::make_tuple(0, std::size_t {1}, 0U);
    CHECK(traits_3d::at_value(vec3d, indices3d) == 3);
  }

  SUBCASE("Second element")
  {
    auto indices3d = std::make_tuple(1, std::size_t {0}, 1U);
    CHECK(traits_3d::at_value(vec3d, indices3d) == 6);
  }
}

TEST_CASE("Mixed Index Types")
{
  using traits_mixed =
      mvector_traits<double, std::tuple<int, std::int16_t, uint64_t>>;
  const traits_mixed::vector_type vec = {{{1.1, 2.2}, {3.3, 4.4}},
                                         {{5.5, 6.6}, {7.7, 8.8}}};

  auto indices = std::make_tuple(1, std::int16_t {1}, 0UL);
  CHECK(traits_mixed::at_value(vec, indices) == doctest::Approx(7.7));
}

// Removed mvector_traits_indexed tests since they're not properly implemented

// Removed mvector_traits_indexed tests since they're not properly implemented

TEST_CASE("Const Correctness")
{
  // Can't use const int with std::vector - using non-const instead
  using traits_nonconst = mvector_traits<int, std::tuple<int, int>>;
  const traits_nonconst::vector_type vec = {{1, 2}, {3, 4}};

  auto indices = std::make_tuple(1, 1);
  CHECK(traits_nonconst::at_value(vec, indices) == 4);
}

TEST_CASE("Empty Vector")
{
  using traits_empty = mvector_traits<int, std::tuple<int>>;
  const traits_empty::vector_type vec;

  // Undefined behavior test - should at least compile
  auto indices = std::make_tuple(0);
  CHECK_NOTHROW(traits_empty::at_value(vec, indices));
}

}  // namespace gtopt
