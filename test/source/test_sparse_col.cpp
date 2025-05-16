#include <doctest/doctest.h>
#include <gtopt/sparse_col.hpp>

using namespace gtopt;

TEST_SUITE("SparseCol")
{
  TEST_CASE("Default Construction")
  {
    SparseCol col;
    CHECK(col.name.empty());
    CHECK(col.lowb == 0.0);
    CHECK(col.uppb == CoinDblMax);
    CHECK(col.cost == 0.0);
    CHECK(col.is_integer == false);
  }

  TEST_CASE("Bound Setting")
  {
    SparseCol col;

    SUBCASE("Equal")
    {
      col.equal(5.0);
      CHECK(col.lowb == 5.0);
      CHECK(col.uppb == 5.0);
    }

    SUBCASE("Free")
    {
      col.free();
      CHECK(col.lowb == -CoinDblMax);
      CHECK(col.uppb == CoinDblMax);
    }

    SUBCASE("Integer")
    {
      CHECK(col.is_integer == false);
      col.integer();
      CHECK(col.is_integer == true);
    }
  }

  TEST_CASE("Compile Time Evaluation")
  {
    // Test that methods can be called at compile time
    constexpr double test_val = []()
    {
      SparseCol c;
      c.equal(10.0);
      return c.lowb;
    }();
    static_assert(test_val == 10.0);
  }

  TEST_CASE("Name Setting")
  {
    SparseCol col;
    col.name = "test_var";
    CHECK(col.name == "test_var");
  }

  TEST_CASE("Cost Setting")
  {
    SparseCol col;
    col.cost = 2.5;
    CHECK(col.cost == 2.5);
  }

  TEST_CASE("Combined Operations")
  {
    SparseCol col;
    col.name = "x1";
    col.cost = 3.0;
    col.equal(5.0).integer();

    CHECK(col.name == "x1");
    CHECK(col.cost == 3.0);
    CHECK(col.lowb == 5.0);
    CHECK(col.uppb == 5.0);
    CHECK(col.is_integer == true);
  }
}
