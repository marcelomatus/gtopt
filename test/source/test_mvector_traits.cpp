/**
 * @file      test_mvector_traits.cpp
 * @brief     Unit tests for mvector_traits template classes
 * @date      Mon Jun  2 21:30:23 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Tests for both mvector_traits and mvector_traits implementations
 */

#include <string>

#include <doctest/doctest.h>
#include <gtopt/block.hpp>
#include <gtopt/mvector_traits.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/stage.hpp>

namespace gtopt
{

TEST_CASE("Indexed Alternative Implementation")
{
  using traits_idx =
      mvector_traits<int, std::tuple<std::size_t, std::size_t>, 2>;
  const traits_idx::vector_type vec = {{1, 2, 3}, {4, 5, 6}};

  SUBCASE("Basic access")
  {
    auto indices = std::make_tuple(std::size_t {0}, std::size_t {2});
    CHECK(traits_idx::at_value(vec, indices) == 3);
  }

  SUBCASE("Different indices")
  {
    auto indices = std::make_tuple(std::size_t {1}, std::size_t {1});
    CHECK(traits_idx::at_value(vec, indices) == 5);
  }
}

TEST_CASE("Deep Nesting with Indexed Alternative")
{
  using traits_deep = mvector_traits<float, std::tuple<int, int, int, int>, 4>;
  const traits_deep::vector_type vec = {
      {{{{1.1, 2.2}, {3.3, 4.4}}, {{5.5, 6.6}, {7.7, 8.8}}}}};

  SUBCASE("First element")
  {
    auto indices = std::make_tuple(0, 0, 0, 0);
    CHECK(traits_deep::at_value(vec, indices) == doctest::Approx(1.1));
  }

  SUBCASE("Last element")
  {
    auto indices = std::make_tuple(0, 1, 1, 1);
    CHECK(traits_deep::at_value(vec, indices) == doctest::Approx(8.8));
  }
}

TEST_SUITE("mvector_traits")
{
  TEST_CASE("Type traits verification")
  {
    SUBCASE("1D vector traits")
    {
      using traits_1d = mvector_traits<int, std::tuple<std::size_t>, 1>;

      CHECK(std::is_same_v<traits_1d::value_type, int>);
      CHECK(std::is_same_v<traits_1d::vector_type, std::vector<int>>);
      CHECK(std::is_same_v<traits_1d::tuple_type, std::tuple<std::size_t>>);
    }

    SUBCASE("1D vector traits Uid")
    {
      using traits_1d = mvector_traits<int, std::tuple<BlockUid>, 1>;

      CHECK(std::is_same_v<traits_1d::value_type, int>);
      CHECK(std::is_same_v<traits_1d::vector_type, std::vector<int>>);
      CHECK(std::is_same_v<traits_1d::tuple_type, std::tuple<BlockUid>>);
    }

    SUBCASE("2D vector traits")
    {
      using traits_2d =
          mvector_traits_auto<double, std::tuple<std::size_t, int>>;

      CHECK(std::is_same_v<traits_2d::value_type, double>);
      CHECK(std::is_same_v<traits_2d::vector_type,
                           std::vector<std::vector<double>>>);
      CHECK(
          std::is_same_v<traits_2d::tuple_type, std::tuple<std::size_t, int>>);
    }

    SUBCASE("3D vector traits")
    {
      using traits_3d =
          mvector_traits_auto<char, std::tuple<int, std::size_t, unsigned>>;
      using expected_type = std::vector<std::vector<std::vector<char>>>;

      CHECK(std::is_same_v<traits_3d::value_type, char>);
      CHECK(std::is_same_v<traits_3d::vector_type, expected_type>);
    }
  }

  TEST_CASE("1D vector access")
  {
    using traits_1d = mvector_traits<int, std::tuple<std::size_t>, 1>;
    traits_1d::vector_type vec = {10, 20, 30, 40, 50};

    SUBCASE("Valid indices")
    {
      CHECK(traits_1d::at_value(vec, std::make_tuple(std::size_t {0})) == 10);
      CHECK(traits_1d::at_value(vec, std::make_tuple(std::size_t {2})) == 30);
      CHECK(traits_1d::at_value(vec, std::make_tuple(std::size_t {4})) == 50);
    }

    SUBCASE("Edge cases")
    {
      CHECK(traits_1d::at_value(vec, std::make_tuple(std::size_t {0}))
            == vec.front());
      CHECK(traits_1d::at_value(vec, std::make_tuple(std::size_t {4}))
            == vec.back());
    }
  }

  TEST_CASE("1D vector access Uid")
  {
    using traits_1d = mvector_traits<int, std::tuple<BlockUid>, 1>;
    traits_1d::vector_type vec = {10, 20, 30, 40, 50};

    SUBCASE("Valid indices")
    {
      CHECK(traits_1d::at_value(vec, std::make_tuple(BlockUid {0})) == 10);
      CHECK(traits_1d::at_value(vec, std::make_tuple(BlockUid {2})) == 30);
      CHECK(traits_1d::at_value(vec, std::make_tuple(BlockUid {4})) == 50);
    }

    SUBCASE("Edge cases")
    {
      CHECK(traits_1d::at_value(vec, std::make_tuple(BlockUid {0}))
            == vec.front());
      CHECK(traits_1d::at_value(vec, std::make_tuple(BlockUid {4}))
            == vec.back());
    }
  }

  TEST_CASE("2D vector access")
  {
    using traits_2d =
        mvector_traits_auto<int, std::tuple<std::size_t, std::size_t>>;
    traits_2d::vector_type vec = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};

    SUBCASE("Various positions")
    {
      CHECK(traits_2d::at_value(
                vec, std::make_tuple(std::size_t {0}, std::size_t {0}))
            == 1);
      CHECK(traits_2d::at_value(
                vec, std::make_tuple(std::size_t {0}, std::size_t {3}))
            == 4);
      CHECK(traits_2d::at_value(
                vec, std::make_tuple(std::size_t {1}, std::size_t {1}))
            == 6);
      CHECK(traits_2d::at_value(
                vec, std::make_tuple(std::size_t {2}, std::size_t {3}))
            == 12);
    }

    SUBCASE("Corner elements")
    {
      // Top-left
      CHECK(traits_2d::at_value(
                vec, std::make_tuple(std::size_t {0}, std::size_t {0}))
            == 1);
      // Top-right
      CHECK(traits_2d::at_value(
                vec, std::make_tuple(std::size_t {0}, std::size_t {3}))
            == 4);
      // Bottom-left
      CHECK(traits_2d::at_value(
                vec, std::make_tuple(std::size_t {2}, std::size_t {0}))
            == 9);
      // Bottom-right
      CHECK(traits_2d::at_value(
                vec, std::make_tuple(std::size_t {2}, std::size_t {3}))
            == 12);
    }
  }

  TEST_CASE("2D vector access Uid")
  {
    using traits_2d = mvector_traits_auto<int, std::tuple<StageUid, BlockUid>>;
    traits_2d::vector_type vec = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};

    SUBCASE("Various positions")
    {
      CHECK(
          traits_2d::at_value(vec, std::make_tuple(StageUid {0}, BlockUid {0}))
          == 1);
      CHECK(
          traits_2d::at_value(vec, std::make_tuple(StageUid {0}, BlockUid {3}))
          == 4);
      CHECK(
          traits_2d::at_value(vec, std::make_tuple(StageUid {1}, BlockUid {1}))
          == 6);
      CHECK(
          traits_2d::at_value(vec, std::make_tuple(StageUid {2}, BlockUid {3}))
          == 12);
    }

    SUBCASE("Corner elements")
    {
      // Top-left
      CHECK(
          traits_2d::at_value(vec, std::make_tuple(StageUid {0}, BlockUid {0}))
          == 1);
      // Top-right
      CHECK(
          traits_2d::at_value(vec, std::make_tuple(StageUid {0}, BlockUid {3}))
          == 4);
      // Bottom-left
      CHECK(
          traits_2d::at_value(vec, std::make_tuple(StageUid {2}, BlockUid {0}))
          == 9);
      // Bottom-right
      CHECK(
          traits_2d::at_value(vec, std::make_tuple(StageUid {2}, BlockUid {3}))
          == 12);

      // CHECK(
      // traits_2d::at_value(vec, std::make_tuple(BlockUid {2}, BlockUid {3}))
      //  == 12);
    }
  }

  TEST_CASE("3D vector access")
  {
    using traits_3d =
        mvector_traits_auto<int,
                            std::tuple<std::size_t, std::size_t, std::size_t>>;
    traits_3d::vector_type vec = {{{1, 2, 3}, {4, 5, 6}},
                                  {{7, 8, 9}, {10, 11, 12}}};

    SUBCASE("All corners of 3D cube")
    {
      CHECK(traits_3d::at_value(
                vec,
                std::make_tuple(
                    std::size_t {0}, std::size_t {0}, std::size_t {0}))
            == 1);
      CHECK(traits_3d::at_value(
                vec,
                std::make_tuple(
                    std::size_t {0}, std::size_t {0}, std::size_t {2}))
            == 3);
      CHECK(traits_3d::at_value(
                vec,
                std::make_tuple(
                    std::size_t {0}, std::size_t {1}, std::size_t {0}))
            == 4);
      CHECK(traits_3d::at_value(
                vec,
                std::make_tuple(
                    std::size_t {0}, std::size_t {1}, std::size_t {2}))
            == 6);
      CHECK(traits_3d::at_value(
                vec,
                std::make_tuple(
                    std::size_t {1}, std::size_t {0}, std::size_t {0}))
            == 7);
      CHECK(traits_3d::at_value(
                vec,
                std::make_tuple(
                    std::size_t {1}, std::size_t {0}, std::size_t {2}))
            == 9);
      CHECK(traits_3d::at_value(
                vec,
                std::make_tuple(
                    std::size_t {1}, std::size_t {1}, std::size_t {0}))
            == 10);
      CHECK(traits_3d::at_value(
                vec,
                std::make_tuple(
                    std::size_t {1}, std::size_t {1}, std::size_t {2}))
            == 12);
    }
  }

  TEST_CASE("Mixed index types")
  {
    using traits_mixed =
        mvector_traits_auto<std::string,
                            std::tuple<int, std::size_t, unsigned>>;
    traits_mixed::vector_type vec = {{{"a", "b"}, {"c", "d"}},
                                     {{"e", "f"}, {"g", "h"}}};

    SUBCASE("Different index type combinations")
    {
      CHECK(traits_mixed::at_value(vec, std::make_tuple(0, std::size_t {0}, 0U))
            == "a");
      CHECK(traits_mixed::at_value(vec, std::make_tuple(0, std::size_t {1}, 1U))
            == "d");
      CHECK(traits_mixed::at_value(vec, std::make_tuple(1, std::size_t {0}, 0U))
            == "e");
      CHECK(traits_mixed::at_value(vec, std::make_tuple(1, std::size_t {1}, 1U))
            == "h");
    }
  }

  TEST_CASE("Custom types")
  {
    struct Point
    {
      int x, y;
      bool operator==(const Point& other) const
      {
        return x == other.x && y == other.y;
      }
    };

    using traits_custom =
        mvector_traits_auto<Point, std::tuple<std::size_t, std::size_t>>;
    traits_custom::vector_type vec = {{{0, 0}, {1, 0}, {2, 0}},
                                      {{0, 1}, {1, 1}, {2, 1}}};

    SUBCASE("Custom type access")
    {
      auto point = traits_custom::at_value(
          vec, std::make_tuple(std::size_t {1}, std::size_t {2}));
      CHECK(point == Point {2, 1});

      point = traits_custom::at_value(
          vec, std::make_tuple(std::size_t {0}, std::size_t {0}));
      CHECK(point == Point {0, 0});
    }
  }

  TEST_CASE("Constexpr evaluation")
  {
    SUBCASE("Compile-time constant evaluation")
    {
      constexpr auto indices =
          std::make_tuple(std::size_t {1}, std::size_t {2});
      // The following should compile (constexpr context)
      constexpr bool is_constexpr =
          std::is_same_v<decltype(indices),
                         const std::tuple<std::size_t, std::size_t>>;
      CHECK(is_constexpr);
    }
  }

  TEST_CASE("Empty containers edge case")
  {
    using traits_1d = mvector_traits<int, std::tuple<std::size_t>, 1>;
    traits_1d::vector_type empty_vec;

    SUBCASE("Empty vector behavior")
    {
      // Note: Accessing empty vector is undefined behavior, but we can test the
      // type
      CHECK(empty_vec.empty());
      CHECK(empty_vec.size() == 0);
    }
  }

  TEST_CASE("Large dimensions")
  {
    using traits_4d = mvector_traits_auto<
        int,
        std::tuple<std::size_t, std::size_t, std::size_t, std::size_t>>;

    // Create a small 4D vector: 2x2x2x2
    traits_4d::vector_type vec(
        2,
        std::vector<std::vector<std::vector<int>>>(
            2, std::vector<std::vector<int>>(2, std::vector<int>(2, 42))));

    SUBCASE("4D access")
    {
      auto value = traits_4d::at_value(vec,
                                       std::make_tuple(std::size_t {1},
                                                       std::size_t {0},
                                                       std::size_t {1},
                                                       std::size_t {0}));
      CHECK(value == 42);
    }
  }
}

TEST_SUITE("Compilation tests")
{
  TEST_CASE("Static assertions should prevent invalid usage")
  {
    // These are compile-time tests - they verify that certain code patterns
    // would fail to compile (we can't actually test compilation failures in
    // doctest)

    SUBCASE("Valid template instantiations")
    {
      using valid_1d = mvector_traits<int, std::tuple<std::size_t>, 1>;
      using valid_2d = mvector_traits_auto<int, std::tuple<std::size_t, int>>;
      using valid_3d =
          mvector_traits_auto<int, std::tuple<int, std::size_t, unsigned>>;

      // If these compile, the static_asserts are working correctly
      CHECK(std::is_same_v<valid_1d::value_type, int>);
      CHECK(std::is_same_v<valid_2d::value_type, int>);
      CHECK(std::is_same_v<valid_3d::value_type, int>);
    }

    // Note: The following would cause compilation errors due to static_assert:
    // using invalid_depth_0 = mvector_traits<int,
    // std::tuple<std::size_t>, 0>; using invalid_depth_too_high =
    // mvector_traits<int, std::tuple<std::size_t>, 2>;
  }
}
}  // namespace gtopt
