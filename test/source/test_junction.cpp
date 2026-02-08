#include <doctest/doctest.h>
#include <gtopt/junction.hpp>

using namespace gtopt;

TEST_CASE("Junction construction and default values")
{
  const Junction junction;

  CHECK(junction.uid == Uid {unknown_uid});
  CHECK(junction.name == Name {});
  CHECK_FALSE(junction.active.has_value());
  CHECK_FALSE(junction.drain.has_value());
}

TEST_CASE("Junction attribute assignment")
{
  Junction junction;

  junction.uid = 7001;
  junction.name = "TestJunction";
  junction.active = true;
  junction.drain = true;

  CHECK(junction.uid == 7001);
  CHECK(junction.name == "TestJunction");
  CHECK(std::get<IntBool>(junction.active.value()) == 1);
  REQUIRE(junction.drain.has_value());
  CHECK(junction.drain.value() == true);
}

TEST_CASE("Junction with drain disabled")
{
  Junction junction;

  junction.uid = 7002;
  junction.name = "NonDrainJunction";
  junction.drain = false;

  CHECK(junction.uid == 7002);
  REQUIRE(junction.drain.has_value());
  CHECK(junction.drain.value() == false);
}
