// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/iteration.hpp>

TEST_CASE("Iteration construction and default values")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Iteration iter;

  CHECK(iter.index == 0);
  CHECK_FALSE(iter.update_lp.has_value());
  CHECK(iter.class_name == "iteration");
}

TEST_CASE("Iteration should_update_lp default behaviour")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("default is true when unset")
  {
    const Iteration iter;
    CHECK(iter.should_update_lp() == true);
  }

  SUBCASE("explicitly true")
  {
    Iteration iter;
    iter.update_lp = true;
    CHECK(iter.should_update_lp() == true);
  }

  SUBCASE("explicitly false")
  {
    Iteration iter;
    iter.update_lp = false;
    CHECK(iter.should_update_lp() == false);
  }
}

TEST_CASE("Iteration attribute assignment")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Iteration iter;

  iter.index = 5;
  iter.update_lp = true;

  CHECK(iter.index == 5);
  REQUIRE(iter.update_lp.has_value());
  CHECK(iter.update_lp.value() == true);
}

TEST_CASE("Iteration designated initializer construction")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Iteration iter {
      .index = 10,
      .update_lp = false,
  };

  CHECK(iter.index == 10);
  REQUIRE(iter.update_lp.has_value());
  CHECK(iter.update_lp.value() == false);
  CHECK(iter.should_update_lp() == false);
}

TEST_CASE("IterationIndex strong type")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const IterationIndex idx {3};
  CHECK(idx == IterationIndex {3});
}

TEST_CASE("Iteration array for SDDP control")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Iteration> iterations {
      {
          .index = 0,
          .update_lp = false,
      },
      {
          .index = 1,
      },
      {
          .index = 5,
          .update_lp = true,
      },
      {
          .index = 10,
          .update_lp = false,
      },
  };

  CHECK(iterations.size() == 4);

  // First iteration: skip LP update (warm start)
  CHECK(iterations[0].should_update_lp() == false);

  // Second iteration: default (update LP)
  CHECK(iterations[1].should_update_lp() == true);

  // Fifth iteration: explicitly update
  CHECK(iterations[2].should_update_lp() == true);

  // Tenth iteration: skip update again
  CHECK(iterations[3].should_update_lp() == false);
}
