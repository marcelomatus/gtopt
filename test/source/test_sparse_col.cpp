#include <doctest/doctest.h>
#include <gtopt/linear_problem.hpp>
#include <gtopt/sparse_col.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_SUITE("SparseCol")
{
  using namespace gtopt;
  TEST_CASE("Default Construction")
  {
    const SparseCol col;
    CHECK(col.lowb == 0.0);
    CHECK(col.uppb == LinearProblem::DblMax);
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
      CHECK(col.lowb == -LinearProblem::DblMax);
      CHECK(col.uppb == LinearProblem::DblMax);
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

  TEST_CASE("Cost Setting")
  {
    SparseCol col;
    col.cost = 2.5;
    CHECK(col.cost == 2.5);
  }

  TEST_CASE("Combined Operations")
  {
    SparseCol col;
    col.cost = 3.0;
    col.equal(5.0).integer();

    CHECK(col.cost == 3.0);
    CHECK(col.lowb == 5.0);
    CHECK(col.uppb == 5.0);
    CHECK(col.is_integer == true);
  }

  TEST_CASE("Negative Bounds")
  {
    SparseCol col;

    SUBCASE("Negative equal")
    {
      col.equal(-10.0);
      CHECK(col.lowb == -10.0);
      CHECK(col.uppb == -10.0);
    }

    SUBCASE("Negative cost")
    {
      col.cost = -5.5;
      CHECK(col.cost == -5.5);
    }

    SUBCASE("Zero equal")
    {
      col.equal(0.0);
      CHECK(col.lowb == 0.0);
      CHECK(col.uppb == 0.0);
    }
  }

  TEST_CASE("Method Chaining Order")
  {
    SparseCol col;

    SUBCASE("free then integer")
    {
      col.free().integer();
      CHECK(col.lowb == -LinearProblem::DblMax);
      CHECK(col.uppb == LinearProblem::DblMax);
      CHECK(col.is_integer == true);
    }

    SUBCASE("equal overwrites free")
    {
      col.free();
      col.equal(42.0);
      CHECK(col.lowb == 42.0);
      CHECK(col.uppb == 42.0);
    }

    SUBCASE("free overwrites equal")
    {
      col.equal(42.0);
      col.free();
      CHECK(col.lowb == -LinearProblem::DblMax);
      CHECK(col.uppb == LinearProblem::DblMax);
    }
  }

  TEST_CASE("Copy Semantics")
  {
    SparseCol col1;
    col1.cost = 7.0;
    col1.equal(3.0).integer();

    const SparseCol col2 = col1;
    CHECK(col2.cost == 7.0);
    CHECK(col2.lowb == 3.0);
    CHECK(col2.uppb == 3.0);
    CHECK(col2.is_integer == true);
  }
}
