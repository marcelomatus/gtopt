/**
 * @file      test_multi_array_2d.cpp
 * @brief     Unit tests for MultiArray2D (boost::multi_array replacement)
 * @date      Sun Feb  9 07:14:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Tests for the MultiArray2D class that replaced boost::multi_array
 */

#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/multi_array_2d.hpp>

using namespace gtopt;

TEST_CASE("MultiArray2D - Default construction")
{
  MultiArray2D<double> arr;

  CHECK(arr.empty());
  CHECK(arr.size() == 0);
}

TEST_CASE("MultiArray2D - Construction with dimensions")
{
  MultiArray2D<double> arr(3, 4);

  CHECK_FALSE(arr.empty());
  CHECK(arr.size() == 3);
}

TEST_CASE("MultiArray2D - Element access and modification")
{
  MultiArray2D<int> arr(2, 3);

  arr[0][0] = 1;
  arr[0][1] = 2;
  arr[0][2] = 3;
  arr[1][0] = 4;
  arr[1][1] = 5;
  arr[1][2] = 6;

  CHECK(arr[0][0] == 1);
  CHECK(arr[0][1] == 2);
  CHECK(arr[0][2] == 3);
  CHECK(arr[1][0] == 4);
  CHECK(arr[1][1] == 5);
  CHECK(arr[1][2] == 6);
}

TEST_CASE("MultiArray2D - Const access")
{
  MultiArray2D<int> arr(2, 2);
  arr[0][0] = 10;
  arr[0][1] = 20;
  arr[1][0] = 30;
  arr[1][1] = 40;

  const auto& const_arr = arr;

  CHECK(const_arr[0][0] == 10);
  CHECK(const_arr[0][1] == 20);
  CHECK(const_arr[1][0] == 30);
  CHECK(const_arr[1][1] == 40);
}

TEST_CASE("MultiArray2D - Row size")
{
  MultiArray2D<int> arr(3, 5);

  CHECK(arr[0].size() == 5);
  CHECK(arr[1].size() == 5);
  CHECK(arr[2].size() == 5);

  const auto& const_arr = arr;
  CHECK(const_arr[0].size() == 5);
}

TEST_CASE("MultiArray2D - Default initialization to zero")
{
  MultiArray2D<int> arr(2, 3);

  for (size_t i = 0; i < 2; ++i) {
    for (size_t j = 0; j < 3; ++j) {
      CHECK(arr[i][j] == 0);
    }
  }
}

TEST_CASE("MultiArray2D - Default initialization for doubles")
{
  MultiArray2D<double> arr(2, 2);

  CHECK(arr[0][0] == doctest::Approx(0.0));
  CHECK(arr[0][1] == doctest::Approx(0.0));
  CHECK(arr[1][0] == doctest::Approx(0.0));
  CHECK(arr[1][1] == doctest::Approx(0.0));
}

TEST_CASE("MultiArray2D - Copy semantics")
{
  MultiArray2D<int> arr1(2, 2);
  arr1[0][0] = 1;
  arr1[0][1] = 2;
  arr1[1][0] = 3;
  arr1[1][1] = 4;

  SUBCASE("Copy construction")
  {
    MultiArray2D<int> arr2 {arr1};
    CHECK(arr2.size() == 2);
    CHECK(arr2[0][0] == 1);
    CHECK(arr2[1][1] == 4);

    // Modify copy, original unchanged
    arr2[0][0] = 99;
    CHECK(arr1[0][0] == 1);
  }

  SUBCASE("Copy assignment")
  {
    MultiArray2D<int> arr2;
    arr2 = arr1;
    CHECK(arr2.size() == 2);
    CHECK(arr2[0][0] == 1);
    CHECK(arr2[1][1] == 4);
  }
}

TEST_CASE("MultiArray2D - Move semantics")
{
  MultiArray2D<int> arr1(2, 2);
  arr1[0][0] = 1;
  arr1[1][1] = 4;

  SUBCASE("Move construction")
  {
    MultiArray2D<int> arr2 {std::move(arr1)};
    CHECK(arr2.size() == 2);
    CHECK(arr2[0][0] == 1);
    CHECK(arr2[1][1] == 4);
  }

  SUBCASE("Move assignment")
  {
    MultiArray2D<int> arr2;
    arr2 = std::move(arr1);
    CHECK(arr2.size() == 2);
    CHECK(arr2[0][0] == 1);
    CHECK(arr2[1][1] == 4);
  }
}

TEST_CASE("MultiArray2D - Complex types")
{
  MultiArray2D<std::string> arr(2, 2);

  arr[0][0] = "hello";
  arr[0][1] = "world";
  arr[1][0] = "foo";
  arr[1][1] = "bar";

  CHECK(arr[0][0] == "hello");
  CHECK(arr[0][1] == "world");
  CHECK(arr[1][0] == "foo");
  CHECK(arr[1][1] == "bar");
}

TEST_CASE("MultiArray2D - Vector element type")
{
  // This matches actual usage: block_factor_matrix_t = MultiArray2D<vector<double>>
  MultiArray2D<std::vector<double>> arr(2, 3);

  arr[0][0] = {1.0, 2.0, 3.0};
  arr[1][2] = {4.0, 5.0};

  CHECK(arr[0][0].size() == 3);
  CHECK(arr[0][0][0] == doctest::Approx(1.0));
  CHECK(arr[1][2].size() == 2);
  CHECK(arr[1][2][1] == doctest::Approx(5.0));

  // Uninitialized vector elements should be empty
  CHECK(arr[0][1].empty());
}

TEST_CASE("MultiArray2D - Single row")
{
  MultiArray2D<int> arr(1, 5);

  for (size_t j = 0; j < 5; ++j) {
    arr[0][j] = static_cast<int>(j * 10);
  }

  CHECK(arr[0][0] == 0);
  CHECK(arr[0][4] == 40);
  CHECK(arr.size() == 1);
}

TEST_CASE("MultiArray2D - Single column")
{
  MultiArray2D<int> arr(5, 1);

  for (size_t i = 0; i < 5; ++i) {
    arr[i][0] = static_cast<int>(i * 10);
  }

  CHECK(arr[0][0] == 0);
  CHECK(arr[4][0] == 40);
  CHECK(arr.size() == 5);
}
