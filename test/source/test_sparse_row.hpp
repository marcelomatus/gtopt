#include <doctest/doctest.h>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;

TEST_SUITE("SparseRow")
{
  TEST_CASE("Default Construction")
  {
    const SparseRow row;
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
      CHECK(row.get_coeff(ColIndex {0}) == 0.0);
      CHECK(row[ColIndex {0}] == 0.0);

      row.set_coeff(ColIndex {0}, 1.5);
      CHECK(row.get_coeff(ColIndex {0}) == 1.5);
      CHECK(row[ColIndex {0}] == 1.5);

      row[ColIndex {1}] = 2.5;
      CHECK(row.get_coeff(ColIndex {1}) == 2.5);
      CHECK(row[ColIndex {1}] == 2.5);
    }

    SUBCASE("Size and Reserve")
    {
      CHECK(row.size() == 0);
      row.reserve(10);
      row.set_coeff(ColIndex {5}, 1.0);
      CHECK(row.size() == 1);
    }
  }

  TEST_CASE("Const Access")
  {
    SparseRow row;
    row.set_coeff(ColIndex {1}, 3.0);
    row.set_coeff(ColIndex {2}, 4.0);

    const SparseRow& const_row = row;
    CHECK(const_row.get_coeff(ColIndex {1}) == 3.0);
    CHECK(const_row[ColIndex {2}] == 4.0);
    CHECK(const_row.size() == 2);
  }

  TEST_CASE("To Flat Conversion")
  {
    SparseRow row;
    row.set_coeff(ColIndex {1}, 1.0);
    row.set_coeff(ColIndex {3}, 2.0);
    row.set_coeff(ColIndex {5}, 0.001);  // Below default epsilon

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
    row.set_coeff(ColIndex {0}, 1.0);

    CHECK(row.lowb == -1.0);
    CHECK(row.uppb == 1.0);
    CHECK(row.size() == 1);
  }

  TEST_CASE("Overwrite Coefficients")
  {
    SparseRow row;

    row.set_coeff(ColIndex {0}, 1.0);
    CHECK(row.get_coeff(ColIndex {0}) == 1.0);
    CHECK(row.size() == 1);

    row.set_coeff(ColIndex {0}, 5.0);
    CHECK(row.get_coeff(ColIndex {0}) == 5.0);
    CHECK(row.size() == 1);

    row[ColIndex {0}] = -3.0;
    CHECK(row.get_coeff(ColIndex {0}) == -3.0);
    CHECK(row.size() == 1);
  }

  TEST_CASE("Empty Row To Flat")
  {
    const SparseRow row;
    auto [indices, values] = row.to_flat();
    CHECK(indices.empty());
    CHECK(values.empty());
  }

  TEST_CASE("To Flat With Zero Epsilon")
  {
    SparseRow row;
    row.set_coeff(ColIndex {0}, 0.0);
    row.set_coeff(ColIndex {1}, 1.0);
    row.set_coeff(ColIndex {2}, -1.0);

    auto [indices, values] = row.to_flat(0.0);
    CHECK(indices.size() == 3);
    CHECK(values.size() == 3);
  }

  TEST_CASE("Negative Coefficients")
  {
    SparseRow row;
    row.set_coeff(ColIndex {0}, -10.5);
    row.set_coeff(ColIndex {1}, -0.001);

    CHECK(row.get_coeff(ColIndex {0}) == -10.5);
    CHECK(row.get_coeff(ColIndex {1}) == -0.001);

    auto [indices, values] = row.to_flat(0.01);
    CHECK(indices.size() == 1);
    CHECK(values[0] == -10.5);
  }

  TEST_CASE("Bound Method Chaining")
  {
    SparseRow row;

    SUBCASE("less_equal overwrites equal")
    {
      row.equal(5.0);
      row.less_equal(10.0);
      CHECK(row.lowb == -SparseRow::CoinDblMax);
      CHECK(row.uppb == 10.0);
    }

    SUBCASE("greater_equal overwrites less_equal")
    {
      row.less_equal(10.0);
      row.greater_equal(3.0);
      CHECK(row.lowb == 3.0);
      CHECK(row.uppb == SparseRow::CoinDblMax);
    }

    SUBCASE("bound with negative values")
    {
      row.bound(-100.0, -50.0);
      CHECK(row.lowb == -100.0);
      CHECK(row.uppb == -50.0);
    }
  }

  TEST_CASE("Large Number of Coefficients")
  {
    SparseRow row;
    row.reserve(100);

    for (int i = 0; i < 100; ++i) {
      row.set_coeff(ColIndex {i}, static_cast<double>(i));
    }

    CHECK(row.size() == 100);
    CHECK(row.get_coeff(ColIndex {0}) == 0.0);
    CHECK(row.get_coeff(ColIndex {50}) == 50.0);
    CHECK(row.get_coeff(ColIndex {99}) == 99.0);

    auto [indices, values] = row.to_flat(0.5);
    CHECK(indices.size() == 99);
  }

  TEST_CASE("To Flat Fast Path No Filtering")
  {
    SparseRow row;
    row.set_coeff(ColIndex {0}, 1.0);
    row.set_coeff(ColIndex {2}, -3.5);
    row.set_coeff(ColIndex {5}, 0.001);

    // Default eps=0.0 takes the fast path (no filtering)
    auto [indices, values] = row.to_flat();
    CHECK(indices.size() == 3);
    CHECK(values.size() == 3);
    CHECK(indices[0] == 0);
    CHECK(indices[1] == 2);
    CHECK(indices[2] == 5);
    CHECK(values[0] == 1.0);
    CHECK(values[1] == -3.5);
    CHECK(values[2] == 0.001);
  }

  TEST_CASE("To Flat Negative Epsilon Fast Path")
  {
    SparseRow row;
    row.set_coeff(ColIndex {0}, 0.0);
    row.set_coeff(ColIndex {1}, 1e-15);
    row.set_coeff(ColIndex {2}, 1.0);

    // Negative eps also takes the fast path
    auto [indices, values] = row.to_flat(-1.0);
    CHECK(indices.size() == 3);
    CHECK(values.size() == 3);
    CHECK(values[0] == 0.0);
    CHECK(values[1] == 1e-15);
    CHECK(values[2] == 1.0);
  }
}
