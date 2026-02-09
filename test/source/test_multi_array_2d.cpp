#include <vector>

#include <doctest/doctest.h>
#include <gtopt/multi_array_2d.hpp>

using namespace gtopt;

TEST_CASE("MultiArray2D default construction")
{
  const MultiArray2D<double> arr;
  CHECK(arr.empty());
  CHECK(arr.size() == 0);
}

TEST_CASE("MultiArray2D construction with dimensions")
{
  const MultiArray2D<int> arr(3, 4);
  CHECK_FALSE(arr.empty());
  CHECK(arr.size() == 3);
}

TEST_CASE("MultiArray2D element access")
{
  MultiArray2D<double> arr(2, 3);
  arr[0][0] = 1.0;
  arr[0][1] = 2.0;
  arr[0][2] = 3.0;
  arr[1][0] = 4.0;
  arr[1][1] = 5.0;
  arr[1][2] = 6.0;

  CHECK(arr[0][0] == doctest::Approx(1.0));
  CHECK(arr[0][1] == doctest::Approx(2.0));
  CHECK(arr[0][2] == doctest::Approx(3.0));
  CHECK(arr[1][0] == doctest::Approx(4.0));
  CHECK(arr[1][1] == doctest::Approx(5.0));
  CHECK(arr[1][2] == doctest::Approx(6.0));
}

TEST_CASE("MultiArray2D const access")
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

TEST_CASE("MultiArray2D row size")
{
  MultiArray2D<int> arr(3, 5);
  CHECK(arr[0].size() == 5);
  CHECK(arr[1].size() == 5);
  CHECK(arr[2].size() == 5);
}

TEST_CASE("MultiArray2D with vector element type")
{
  MultiArray2D<std::vector<double>> arr(1, 1);
  arr[0][0] = {2.0, 3.0};

  CHECK(arr[0][0].size() == 2);
  CHECK(arr[0][0][0] == doctest::Approx(2.0));
  CHECK(arr[0][0][1] == doctest::Approx(3.0));
}
