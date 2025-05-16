#include <doctest/doctest.h>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;

TEST_SUITE("SparseRow")
{
  TEST_CASE("Default Construction")
  {
    SparseRow row;
    CHECK(row.name.empty());
    CHECK(row.lowb == 0.0);
    CHECK(row.uppb == 0.0);
    CHECK(row.size() == 0);
  }

  TEST_CASE("Bound Setting")
  {
    SparseRow row;

    SUBCASE("Bound")
    {
      row.bound(-1.0, 1.0);
      CHECK(row.lowb == -1.0);
      CHECK(row.uppb == 1.0);
    }

    SUBCASE("Less Equal")
    {
      row.less_equal(5.0);
      CHECK(row.lowb == -SparseRow::CoinDblMax);
      CHECK(row.uppb == 5.0);
    }

    SUBCASE("Greater Equal")
    {
      row.greater_equal(3.0);
      CHECK(row.lowb == 3.0);
      CHECK(row.uppb == SparseRow::CoinDblMax);
    }

    SUBCASE("Equal")
    {
      row.equal(2.5);
      CHECK(row.lowb == 2.5);
      CHECK(row.uppb == 2.5);
    }
  }

  TEST_CASE("Coefficient Access")
  {
    SparseRow row;

    SUBCASE("Get/Set Coefficients")
    {
      CHECK(row.get_coeff(0) == 0.0);
      CHECK(row[0] == 0.0);

      row.set_coeff(0, 1.5);
      CHECK(row.get_coeff(0) == 1.5);
      CHECK(row[0] == 1.5);

      row[1] = 2.5;
      CHECK(row.get_coeff(1) == 2.5);
      CHECK(row[1] == 2.5);
    }

    SUBCASE("Size and Reserve")
    {
      CHECK(row.size() == 0);
      row.reserve(10);
      row.set_coeff(5, 1.0);
      CHECK(row.size() == 1);
    }
  }

  TEST_CASE("Const Access")
  {
    SparseRow row;
    row.set_coeff(1, 3.0);
    row.set_coeff(2, 4.0);

    const SparseRow& const_row = row;
    CHECK(const_row.get_coeff(1) == 3.0);
    CHECK(const_row[2] == 4.0);
    CHECK(const_row.size() == 2);
  }

  TEST_CASE("To Flat Conversion")
  {
    SparseRow row;
    row.set_coeff(1, 1.0);
    row.set_coeff(3, 2.0);
    row.set_coeff(5, 0.001);  // Below default epsilon

    SUBCASE("Default Epsilon")
    {
      auto [indices, values] = row.to_flat(0.01);
      CHECK(indices.size() == 2);
      CHECK(values.size() == 2);
      CHECK(indices[0] == 1);
      CHECK(values[0] == 1.0);
      CHECK(indices[1] == 3);
      CHECK(values[1] == 2.0);
    }

    SUBCASE("Custom Epsilon")
    {
      auto [indices, values] = row.to_flat(0.0001);
      CHECK(indices.size() == 3);
      CHECK(values[2] == 0.001);
    }

    SUBCASE("Different Types")
    {
      auto [indices, values] = row.to_flat<int, float>();
      static_assert(std::is_same_v<decltype(indices)::value_type, int>);
      static_assert(std::is_same_v<decltype(values)::value_type, float>);
    }
  }

  TEST_CASE("Compile Time Evaluation")
  {
    SparseRow row;
    row.bound(-1.0, 1.0);
    row.set_coeff(0, 1.0);

    CHECK(row.lowb == -1.0);
    CHECK(row.uppb == 1.0);
    CHECK(row.size() == 1);
  }
}
