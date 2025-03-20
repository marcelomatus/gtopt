#include <string>

#include <doctest/doctest.h>
#include <gtopt/basic_types.hpp>

TEST_CASE("Bus")
{
  using namespace gtopt;

  Name name = "bus_1";
  Uid uid = 1;

  CHECK(name == "bus_1");
  CHECK(uid == 1);
}

TEST_CASE("Bus json") {}
