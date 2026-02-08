#include <doctest/doctest.h>
#include <gtopt/filtration.hpp>

using namespace gtopt;

TEST_CASE("Filtration construction and default values")
{
  const Filtration filtration;

  CHECK(filtration.uid == Uid {unknown_uid});
  CHECK(filtration.name == Name {});
  CHECK_FALSE(filtration.active.has_value());

  CHECK(filtration.waterway == SingleId {unknown_uid});
  CHECK(filtration.reservoir == SingleId {unknown_uid});
  CHECK(filtration.slope == 0.0);
  CHECK(filtration.constant == 0.0);
}

TEST_CASE("Filtration attribute assignment")
{
  Filtration filtration;

  filtration.uid = 8001;
  filtration.name = "TestFiltration";
  filtration.active = true;

  filtration.waterway = Uid {6001};
  filtration.reservoir = Uid {9001};
  filtration.slope = 0.15;
  filtration.constant = 2.5;

  CHECK(filtration.uid == 8001);
  CHECK(filtration.name == "TestFiltration");
  CHECK(std::get<IntBool>(filtration.active.value()) == 1);

  CHECK(std::get<Uid>(filtration.waterway) == Uid {6001});
  CHECK(std::get<Uid>(filtration.reservoir) == Uid {9001});
  CHECK(filtration.slope == doctest::Approx(0.15));
  CHECK(filtration.constant == doctest::Approx(2.5));
}

TEST_CASE("Filtration with zero slope")
{
  Filtration filtration;

  filtration.uid = 8002;
  filtration.name = "ConstantFiltration";
  filtration.slope = 0.0;
  filtration.constant = 5.0;

  CHECK(filtration.slope == 0.0);
  CHECK(filtration.constant == doctest::Approx(5.0));
}
